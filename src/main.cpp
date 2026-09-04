#define GLFW_INCLUDE_VULKAN
#include <GLFW/glfw3.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <optional>
#include <set>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

/*
Vulkan 架构速览（本文件中的对象大致按下面的依赖关系创建）：

  当前进程 / CPU 主线程
    -> Vulkan Loader -> VkInstance                         全局入口、扩展和校验层
      -> VkPhysicalDevice                                 枚举出的物理 GPU，只描述能力
        -> VkDevice                                       为某块 GPU 创建的逻辑设备
          -> VkQueue                                      GPU 工作的提交入口
          -> VkCommandPool -> VkCommandBuffer             在 CPU 端录制的 GPU 命令
          -> VkPipeline / VkRenderPass / VkFramebuffer    渲染状态与目标
      -> VkSurfaceKHR -> VkSwapchainKHR -> VkImage        窗口系统和呈现引擎管理的图像

需要特别区分三件事：
1. Vulkan API 调用发生在 CPU（host）上；录制命令不等于 GPU 已执行命令。
   vkQueueSubmit 之后，GPU 才会异步消费 command buffer。
2. “CPU 代码的线程安全”与“GPU 命令的同步”是两层问题。许多 Vulkan 对象要求
   externally synchronized，即同一个对象被多个 CPU 线程访问时由应用自己加锁；
   semaphore / fence / pipeline barrier 则用来描述 CPU、队列及设备内工作的先后关系。
3. 本示例是单进程、单 CPU 提交线程、单 VkDevice。下面出现的两个 queue family 只可能是
   同一逻辑设备上的 graphics/present 队列族，并不是两块 GPU。真正的跨进程或跨设备共享
   需要 external memory/semaphore/fence 扩展及操作系统句柄，或 device-group/peer-memory
   能力；仅传递 VkImage、VkSemaphore 等句柄无效，因为普通 Vulkan 句柄只在其创建范围内有效。

如果把本例扩展到更复杂的同步场景，可以用下面的流程建立心智模型：

  多 CPU 线程：每线程 command pool -> 并行录制 command buffer -> 对共享 queue 加互斥锁后 submit。
               互斥锁保护 host 对象访问；GPU 间的读写依赖仍要用 semaphore/barrier 描述。

  跨进程共享：生产进程创建可导出的 VkDeviceMemory 和 semaphore -> 导出 Win32 HANDLE/文件描述符
               -> 通过操作系统 IPC 传给消费进程 -> 消费进程导入内存和 semaphore
               -> 生产队列“写资源 + release/布局转换 + signal”
               -> 消费队列“wait + acquire/布局转换 + 读资源”。
               必须同时共享“存储”和“完成信号”；只共享内存而不同步会产生数据竞争。

  跨物理设备：先查询 external-memory 或 device-group 的兼容性。可直连时采用与上面类似的
               release/signal -> wait/acquire；不可直连时退化为 GPU A -> host staging memory
               -> GPU B，并在每一段用 fence/semaphore/barrier 保证完成、可见性和正确布局。

同步不只表示“先后顺序”，还要回答：(a) 哪个阶段必须等，(b) 哪类写入要对哪类读取可见，
(c) 图像当前是什么 layout，(d) 资源由哪个 queue family/外部实体拥有。Semaphore/fence 主要连接
任务时间线，pipeline barrier/render-pass dependency 主要限定设备内访问范围、内存可见性和布局/所有权转换。
*/

constexpr uint32_t kInitialWidth = 1280;
constexpr uint32_t kInitialHeight = 720;
constexpr int kMaxFramesInFlight = 2;

#ifdef NDEBUG
constexpr bool kEnableValidationLayers = false;
#else
constexpr bool kEnableValidationLayers = true;
#endif

const std::vector<const char*> kValidationLayers = {
    "VK_LAYER_KHRONOS_validation",
};

const std::vector<const char*> kDeviceExtensions = {
    VK_KHR_SWAPCHAIN_EXTENSION_NAME,
};

struct PushConstants {
    // 一小段 CPU -> shader 数据。vkCmdPushConstants 会把这些字节记录进命令缓冲；
    // 它不需要单独的 VkBuffer、映射显存或 flush。布局必须与 fragment shader 完全一致。
    float time;
    float width;
    float height;
    float aspect;
    float mouseX;
    float mouseY;
    float padding0;
    float padding1;
};

struct QueueFamilyIndices {
    // queue family 表示同一物理设备上能力相同的一组队列。某些驱动用同一族同时完成绘制和呈现，
    // 另一些驱动则分开提供；因此代码必须分别查询，不能把两者当成固定相同。
    std::optional<uint32_t> graphicsFamily;
    std::optional<uint32_t> presentFamily;

    [[nodiscard]] bool complete() const {
        return graphicsFamily.has_value() && presentFamily.has_value();
    }
};

struct SwapChainSupportDetails {
    VkSurfaceCapabilitiesKHR capabilities{};
    std::vector<VkSurfaceFormatKHR> formats;
    std::vector<VkPresentModeKHR> presentModes;
};

VKAPI_ATTR VkBool32 VKAPI_CALL debugCallback(
    VkDebugUtilsMessageSeverityFlagBitsEXT severity,
    VkDebugUtilsMessageTypeFlagsEXT,
    const VkDebugUtilsMessengerCallbackDataEXT* callbackData,
    void*) {
    std::cerr << "[validation] " << callbackData->pMessage << '\n';
    return VK_FALSE;
}

void populateDebugMessengerCreateInfo(VkDebugUtilsMessengerCreateInfoEXT& info) {
    info = {};
    info.sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT;
    info.messageSeverity = VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT |
                           VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT;
    info.messageType = VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT |
                       VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT |
                       VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT;
    info.pfnUserCallback = debugCallback;
}

VkResult createDebugUtilsMessengerEXT(
    VkInstance instance,
    const VkDebugUtilsMessengerCreateInfoEXT* createInfo,
    VkDebugUtilsMessengerEXT* messenger) {
    const auto function = reinterpret_cast<PFN_vkCreateDebugUtilsMessengerEXT>(
        vkGetInstanceProcAddr(instance, "vkCreateDebugUtilsMessengerEXT"));
    if (function == nullptr) {
        return VK_ERROR_EXTENSION_NOT_PRESENT;
    }
    return function(instance, createInfo, nullptr, messenger);
}

void destroyDebugUtilsMessengerEXT(VkInstance instance, VkDebugUtilsMessengerEXT messenger) {
    const auto function = reinterpret_cast<PFN_vkDestroyDebugUtilsMessengerEXT>(
        vkGetInstanceProcAddr(instance, "vkDestroyDebugUtilsMessengerEXT"));
    if (function != nullptr) {
        function(instance, messenger, nullptr);
    }
}

std::vector<char> readBinaryFile(const std::filesystem::path& path) {
    std::ifstream file(path, std::ios::ate | std::ios::binary);
    if (!file.is_open()) {
        throw std::runtime_error("Could not open shader: " + path.string());
    }

    const auto fileSize = static_cast<size_t>(file.tellg());
    if (fileSize == 0 || fileSize % sizeof(uint32_t) != 0) {
        throw std::runtime_error("Invalid SPIR-V file size: " + path.string());
    }

    std::vector<char> buffer(fileSize);
    file.seekg(0);
    file.read(buffer.data(), static_cast<std::streamsize>(fileSize));
    return buffer;
}

class VulkanApp {
public:
    void run() {
        // 生命周期遵循“先创建依赖，后销毁依赖”的规则；cleanup() 基本按 initVulkan() 的逆序执行。
        initWindow();
        initVulkan();
        mainLoop();
        cleanup();
    }

private:
    // Instance 层对象不属于某个逻辑设备：surface 连接 Vulkan 与 GLFW 所代表的窗口系统。
    GLFWwindow* window_ = nullptr;
    VkInstance instance_ = VK_NULL_HANDLE;
    VkDebugUtilsMessengerEXT debugMessenger_ = VK_NULL_HANDLE;
    VkSurfaceKHR surface_ = VK_NULL_HANDLE;

    VkPhysicalDevice physicalDevice_ = VK_NULL_HANDLE;
    // VkDevice 是应用与选定 GPU 的接口；VkQueue 是向该设备提交异步工作的入口。
    // Queue 句柄本身要求外部同步：若以后从多个 CPU 线程调用同一个 queue，应在应用侧串行化提交。
    VkDevice device_ = VK_NULL_HANDLE;
    VkQueue graphicsQueue_ = VK_NULL_HANDLE;
    VkQueue presentQueue_ = VK_NULL_HANDLE;

    VkSwapchainKHR swapChain_ = VK_NULL_HANDLE;
    // swapchain 图像由呈现引擎拥有，本程序只取得句柄并为其创建 view/framebuffer，不能自行释放 VkImage。
    std::vector<VkImage> swapChainImages_;
    std::vector<VkImageView> swapChainImageViews_;
    std::vector<VkFramebuffer> swapChainFramebuffers_;
    VkFormat swapChainImageFormat_ = VK_FORMAT_UNDEFINED;
    VkExtent2D swapChainExtent_{};

    VkRenderPass renderPass_ = VK_NULL_HANDLE;
    VkPipelineLayout pipelineLayout_ = VK_NULL_HANDLE;
    VkPipeline graphicsPipeline_ = VK_NULL_HANDLE;

    VkCommandPool commandPool_ = VK_NULL_HANDLE;
    // 每个“在途帧槽”各有一份可重复录制的 command buffer，避免 CPU 覆盖 GPU 尚未消费的命令。
    std::array<VkCommandBuffer, kMaxFramesInFlight> commandBuffers_{};

    // 两个 binary semaphore 组成 GPU/呈现引擎一侧的链：
    // acquire --(imageAvailable)--> graphics --(renderFinished)--> present。
    // fence 则让 CPU 知道某次 graphics submit 已完成。Semaphore 不能由 CPU 直接 wait。
    std::array<VkSemaphore, kMaxFramesInFlight> imageAvailableSemaphores_{};
    std::array<VkSemaphore, kMaxFramesInFlight> renderFinishedSemaphores_{};
    std::array<VkFence, kMaxFramesInFlight> inFlightFences_{};
    // 帧槽和 swapchain 图像不是同一个概念：前者按 currentFrame_ 轮转，后者由 acquire 返回。
    // 此表记录“每张图像最后由哪个 fence 占用”，防止重新 acquire 到仍在渲染的图像。
    std::vector<VkFence> imagesInFlight_;

    size_t currentFrame_ = 0;
    bool framebufferResized_ = false;
    std::chrono::steady_clock::time_point startTime_ = std::chrono::steady_clock::now();

    static void framebufferResizeCallback(GLFWwindow* window, int, int) {
        // GLFW 在调用 glfwPollEvents/glfwWaitEvents 的线程上派发此回调；本程序因此仍是单线程访问。
        // 回调只置标志，不在回调中销毁 Vulkan 对象，可避免与 drawFrame 的半帧状态交错。
        auto* app = reinterpret_cast<VulkanApp*>(glfwGetWindowUserPointer(window));
        app->framebufferResized_ = true;
    }

    void initWindow() {
        if (glfwInit() != GLFW_TRUE) {
            throw std::runtime_error("GLFW initialization failed");
        }
        if (glfwVulkanSupported() != GLFW_TRUE) {
            throw std::runtime_error("GLFW cannot find a Vulkan loader/driver");
        }

        glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);
        glfwWindowHint(GLFW_RESIZABLE, GLFW_TRUE);
        window_ = glfwCreateWindow(
            static_cast<int>(kInitialWidth),
            static_cast<int>(kInitialHeight),
            "Vulkan Shader Starter",
            nullptr,
            nullptr);
        if (window_ == nullptr) {
            throw std::runtime_error("GLFW window creation failed");
        }

        glfwSetWindowUserPointer(window_, this);
        glfwSetFramebufferSizeCallback(window_, framebufferResizeCallback);
    }

    void initVulkan() {
        // 这些步骤也展示 Vulkan 的分层：实例 -> 表面/物理设备 -> 逻辑设备 -> 交换链 -> 渲染对象。
        createInstance();
        setupDebugMessenger();
        createSurface();
        pickPhysicalDevice();
        createLogicalDevice();
        createSwapChain();
        createImageViews();
        createRenderPass();
        createGraphicsPipeline();
        createFramebuffers();
        createCommandPool();
        createCommandBuffers();
        createSyncObjects();
    }

    void mainLoop() {
        while (glfwWindowShouldClose(window_) == GLFW_FALSE) {
            glfwPollEvents();
            drawFrame();
        }
        // vkQueueSubmit 是异步的。退出前必须等待设备空闲，之后才能安全销毁仍被 GPU 引用的资源。
        // 正常逐帧同步优先用 fence；vkDeviceWaitIdle 是粗粒度同步，适合退出和本例的交换链重建。
        vkDeviceWaitIdle(device_);
    }

    void cleanupSwapChain() {
        // framebuffer -> image view -> swapchain 存在引用/所有权依赖，因此按这个方向销毁。
        for (const auto framebuffer : swapChainFramebuffers_) {
            vkDestroyFramebuffer(device_, framebuffer, nullptr);
        }
        swapChainFramebuffers_.clear();

        vkDestroyPipeline(device_, graphicsPipeline_, nullptr);
        vkDestroyPipelineLayout(device_, pipelineLayout_, nullptr);
        vkDestroyRenderPass(device_, renderPass_, nullptr);
        graphicsPipeline_ = VK_NULL_HANDLE;
        pipelineLayout_ = VK_NULL_HANDLE;
        renderPass_ = VK_NULL_HANDLE;

        for (const auto imageView : swapChainImageViews_) {
            vkDestroyImageView(device_, imageView, nullptr);
        }
        swapChainImageViews_.clear();

        vkDestroySwapchainKHR(device_, swapChain_, nullptr);
        swapChain_ = VK_NULL_HANDLE;
    }

    void cleanup() {
        cleanupSwapChain();

        for (int i = 0; i < kMaxFramesInFlight; ++i) {
            vkDestroySemaphore(device_, renderFinishedSemaphores_[i], nullptr);
            vkDestroySemaphore(device_, imageAvailableSemaphores_[i], nullptr);
            vkDestroyFence(device_, inFlightFences_[i], nullptr);
        }

        vkDestroyCommandPool(device_, commandPool_, nullptr);
        vkDestroyDevice(device_, nullptr);

        if (kEnableValidationLayers) {
            destroyDebugUtilsMessengerEXT(instance_, debugMessenger_);
        }
        vkDestroySurfaceKHR(instance_, surface_, nullptr);
        vkDestroyInstance(instance_, nullptr);

        glfwDestroyWindow(window_);
        glfwTerminate();
    }

    void recreateSwapChain() {
        int width = 0;
        int height = 0;
        glfwGetFramebufferSize(window_, &width, &height);
        while (width == 0 || height == 0) {
            // 窗口最小化时 framebuffer 尺寸可能为 0；此时不能创建合法的 swapchain。
            glfwWaitEvents();
            glfwGetFramebufferSize(window_, &width, &height);
        }

        // 这里用最简单的全设备屏障保证旧 swapchain 资源不再被任何队列使用。
        // 更复杂的引擎通常等待相关帧 fence，以免阻塞设备上的无关工作。
        vkDeviceWaitIdle(device_);
        cleanupSwapChain();
        createSwapChain();
        createImageViews();
        createRenderPass();
        createGraphicsPipeline();
        createFramebuffers();
        imagesInFlight_.assign(swapChainImages_.size(), VK_NULL_HANDLE);
    }

    void createInstance() {
        if (kEnableValidationLayers && !checkValidationLayerSupport()) {
            throw std::runtime_error(
                "VK_LAYER_KHRONOS_validation is unavailable. Install/repair the Vulkan SDK.");
        }

        VkApplicationInfo applicationInfo{};
        // Vulkan 的可扩展结构通常都要求填写 sType，并通过 pNext 串接扩展结构。
        // applicationInfo 中的名称/版本主要供驱动诊断与兼容策略参考，不会自动创建任何设备资源。
        applicationInfo.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
        applicationInfo.pApplicationName = "Vulkan Shader Starter";
        applicationInfo.applicationVersion = VK_MAKE_VERSION(0, 1, 0);
        applicationInfo.pEngineName = "Learning Renderer";
        applicationInfo.engineVersion = VK_MAKE_VERSION(0, 1, 0);
        applicationInfo.apiVersion = VK_API_VERSION_1_1;

        const auto extensions = requiredInstanceExtensions();
        VkInstanceCreateInfo createInfo{};
        createInfo.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
        createInfo.pApplicationInfo = &applicationInfo;
        createInfo.enabledExtensionCount = static_cast<uint32_t>(extensions.size());
        createInfo.ppEnabledExtensionNames = extensions.data();

        VkDebugUtilsMessengerCreateInfoEXT debugCreateInfo{};
        if (kEnableValidationLayers) {
            createInfo.enabledLayerCount = static_cast<uint32_t>(kValidationLayers.size());
            createInfo.ppEnabledLayerNames = kValidationLayers.data();
            populateDebugMessengerCreateInfo(debugCreateInfo);
            // 把 debug create info 接到 pNext，可捕获 vkCreateInstance/vkDestroyInstance 附近的消息。
            createInfo.pNext = &debugCreateInfo;
        }

        if (vkCreateInstance(&createInfo, nullptr, &instance_) != VK_SUCCESS) {
            throw std::runtime_error("vkCreateInstance failed");
        }
    }

    void setupDebugMessenger() {
        if (!kEnableValidationLayers) {
            return;
        }
        VkDebugUtilsMessengerCreateInfoEXT createInfo{};
        populateDebugMessengerCreateInfo(createInfo);
        if (createDebugUtilsMessengerEXT(instance_, &createInfo, &debugMessenger_) != VK_SUCCESS) {
            throw std::runtime_error("Debug messenger creation failed");
        }
    }

    void createSurface() {
        // Surface 只是“可呈现目标”的抽象，不存放像素；真正轮换的可显示图像由 swapchain 创建。
        if (glfwCreateWindowSurface(instance_, window_, nullptr, &surface_) != VK_SUCCESS) {
            throw std::runtime_error("Window surface creation failed");
        }
    }

    void pickPhysicalDevice() {
        // VkPhysicalDevice 由 instance 枚举，不能由应用创建或销毁。这里只做能力筛选，
        // 尚未创建队列、分配显存或向 GPU 提交任何工作。
        uint32_t count = 0;
        vkEnumeratePhysicalDevices(instance_, &count, nullptr);
        if (count == 0) {
            throw std::runtime_error("No Vulkan-capable GPU was found");
        }

        std::vector<VkPhysicalDevice> devices(count);
        vkEnumeratePhysicalDevices(instance_, &count, devices.data());

        int bestScore = -1;
        for (const auto candidate : devices) {
            if (!isDeviceSuitable(candidate)) {
                continue;
            }
            VkPhysicalDeviceProperties properties{};
            vkGetPhysicalDeviceProperties(candidate, &properties);
            const int score = properties.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU ? 100 : 10;
            if (score > bestScore) {
                physicalDevice_ = candidate;
                bestScore = score;
            }
        }

        if (physicalDevice_ == VK_NULL_HANDLE) {
            throw std::runtime_error("No GPU provides graphics + presentation + swapchain support");
        }

        VkPhysicalDeviceProperties properties{};
        vkGetPhysicalDeviceProperties(physicalDevice_, &properties);
        std::cout << "GPU: " << properties.deviceName << '\n';
    }

    void createLogicalDevice() {
        const auto indices = findQueueFamilies(physicalDevice_);
        // 同一族同时支持 graphics/present 时 set 会去重，只创建一个 queue；此时两个句柄值通常相同。
        const std::set<uint32_t> uniqueFamilies = {
            indices.graphicsFamily.value(),
            indices.presentFamily.value(),
        };

        constexpr float queuePriority = 1.0F;
        std::vector<VkDeviceQueueCreateInfo> queueCreateInfos;
        for (const uint32_t family : uniqueFamilies) {
            VkDeviceQueueCreateInfo queueInfo{};
            queueInfo.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
            queueInfo.queueFamilyIndex = family;
            queueInfo.queueCount = 1;
            queueInfo.pQueuePriorities = &queuePriority;
            queueCreateInfos.push_back(queueInfo);
        }

        VkPhysicalDeviceFeatures features{};
        VkDeviceCreateInfo createInfo{};
        createInfo.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
        createInfo.queueCreateInfoCount = static_cast<uint32_t>(queueCreateInfos.size());
        createInfo.pQueueCreateInfos = queueCreateInfos.data();
        createInfo.pEnabledFeatures = &features;
        createInfo.enabledExtensionCount = static_cast<uint32_t>(kDeviceExtensions.size());
        createInfo.ppEnabledExtensionNames = kDeviceExtensions.data();
        if (kEnableValidationLayers) {
            createInfo.enabledLayerCount = static_cast<uint32_t>(kValidationLayers.size());
            createInfo.ppEnabledLayerNames = kValidationLayers.data();
        }

        if (vkCreateDevice(physicalDevice_, &createInfo, nullptr, &device_) != VK_SUCCESS) {
            throw std::runtime_error("vkCreateDevice failed");
        }

        // vkGetDeviceQueue 取得创建逻辑设备时请求的队列，不是新建队列。queue index 0 对应 queueCount=1。
        vkGetDeviceQueue(device_, indices.graphicsFamily.value(), 0, &graphicsQueue_);
        vkGetDeviceQueue(device_, indices.presentFamily.value(), 0, &presentQueue_);
    }

    void createSwapChain() {
        // Swapchain 是应用与窗口呈现引擎之间的生产者/消费者队列：应用 acquire 一张图像，
        // GPU 写入它，再由 present 把它交回呈现引擎。图像数量通常比在途帧数多，两者不要混用。
        const auto support = querySwapChainSupport(physicalDevice_);
        const auto surfaceFormat = chooseSwapSurfaceFormat(support.formats);
        const auto presentMode = chooseSwapPresentMode(support.presentModes);
        const auto extent = chooseSwapExtent(support.capabilities);

        uint32_t imageCount = support.capabilities.minImageCount + 1;
        if (support.capabilities.maxImageCount > 0 && imageCount > support.capabilities.maxImageCount) {
            imageCount = support.capabilities.maxImageCount;
        }

        VkSwapchainCreateInfoKHR createInfo{};
        createInfo.sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR;
        createInfo.surface = surface_;
        createInfo.minImageCount = imageCount;
        createInfo.imageFormat = surfaceFormat.format;
        createInfo.imageColorSpace = surfaceFormat.colorSpace;
        createInfo.imageExtent = extent;
        createInfo.imageArrayLayers = 1;
        createInfo.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;

        const auto indices = findQueueFamilies(physicalDevice_);
        const uint32_t queueFamilyIndices[] = {
            indices.graphicsFamily.value(),
            indices.presentFamily.value(),
        };
        if (indices.graphicsFamily != indices.presentFamily) {
            // CONCURRENT 在同一 VkDevice 的两个 queue family 间共享 swapchain 图像，省去显式 ownership transfer。
            // 这是“跨队列族访问”，不是跨 GPU/跨进程共享；代价可能是比 EXCLUSIVE 略低效。
            createInfo.imageSharingMode = VK_SHARING_MODE_CONCURRENT;
            createInfo.queueFamilyIndexCount = 2;
            createInfo.pQueueFamilyIndices = queueFamilyIndices;
        } else {
            // 只有一个队列族访问时 EXCLUSIVE 最直接，通常也有最好的实现路径。
            createInfo.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
        }

        createInfo.preTransform = support.capabilities.currentTransform;
        createInfo.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
        createInfo.presentMode = presentMode;
        createInfo.clipped = VK_TRUE;
        createInfo.oldSwapchain = VK_NULL_HANDLE;

        if (vkCreateSwapchainKHR(device_, &createInfo, nullptr, &swapChain_) != VK_SUCCESS) {
            throw std::runtime_error("Swapchain creation failed");
        }

        // Vulkan 常用“两次调用”枚举模式：第一次查询数量，分配容器，第二次取得句柄。
        vkGetSwapchainImagesKHR(device_, swapChain_, &imageCount, nullptr);
        swapChainImages_.resize(imageCount);
        vkGetSwapchainImagesKHR(device_, swapChain_, &imageCount, swapChainImages_.data());
        swapChainImageFormat_ = surfaceFormat.format;
        swapChainExtent_ = extent;
    }

    void createImageViews() {
        // VkImage 是存储，VkImageView 是 shader/render pass 访问该存储时所用的格式和子资源解释。
        swapChainImageViews_.resize(swapChainImages_.size());
        for (size_t i = 0; i < swapChainImages_.size(); ++i) {
            VkImageViewCreateInfo createInfo{};
            createInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
            createInfo.image = swapChainImages_[i];
            createInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
            createInfo.format = swapChainImageFormat_;
            createInfo.components = {
                VK_COMPONENT_SWIZZLE_IDENTITY,
                VK_COMPONENT_SWIZZLE_IDENTITY,
                VK_COMPONENT_SWIZZLE_IDENTITY,
                VK_COMPONENT_SWIZZLE_IDENTITY,
            };
            createInfo.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
            createInfo.subresourceRange.baseMipLevel = 0;
            createInfo.subresourceRange.levelCount = 1;
            createInfo.subresourceRange.baseArrayLayer = 0;
            createInfo.subresourceRange.layerCount = 1;

            if (vkCreateImageView(device_, &createInfo, nullptr, &swapChainImageViews_[i]) != VK_SUCCESS) {
                throw std::runtime_error("Swapchain image view creation failed");
            }
        }
    }

    void createRenderPass() {
        // Render pass 声明 attachment 在一次渲染中的用途和布局变化。
        // 这里驱动可把 UNDEFINED -> COLOR_ATTACHMENT_OPTIMAL -> PRESENT_SRC_KHR 的转换
        // 与渲染过程合并；initialLayout=UNDEFINED 表示不保留图像原内容，本帧会先 clear。
        VkAttachmentDescription colorAttachment{};
        colorAttachment.format = swapChainImageFormat_;
        colorAttachment.samples = VK_SAMPLE_COUNT_1_BIT;
        colorAttachment.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
        colorAttachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        colorAttachment.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
        colorAttachment.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        colorAttachment.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        colorAttachment.finalLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;

        VkAttachmentReference colorReference{};
        colorReference.attachment = 0;
        colorReference.layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;

        VkSubpassDescription subpass{};
        subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
        subpass.colorAttachmentCount = 1;
        subpass.pColorAttachments = &colorReference;

        VkSubpassDependency dependency{};
        // 此 dependency 描述“subpass 外部 -> 颜色输出”的执行与内存依赖：
        // 写 swapchain 图像必须等到颜色输出阶段允许访问它，并使本 subpass 的 color write 合法可见。
        // drawFrame 中 acquire semaphore 解决“何时取得图像”，这里则解决 render pass 内的访问/布局依赖。
        dependency.srcSubpass = VK_SUBPASS_EXTERNAL;
        dependency.dstSubpass = 0;
        dependency.srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
        dependency.dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
        dependency.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;

        VkRenderPassCreateInfo createInfo{};
        createInfo.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
        createInfo.attachmentCount = 1;
        createInfo.pAttachments = &colorAttachment;
        createInfo.subpassCount = 1;
        createInfo.pSubpasses = &subpass;
        createInfo.dependencyCount = 1;
        createInfo.pDependencies = &dependency;

        if (vkCreateRenderPass(device_, &createInfo, nullptr, &renderPass_) != VK_SUCCESS) {
            throw std::runtime_error("Render pass creation failed");
        }
    }

    VkShaderModule createShaderModule(const std::vector<char>& code) const {
        // Vulkan 驱动接收 SPIR-V 字节码，而非 GLSL 源码；shader module 是创建 pipeline 时的输入。
        VkShaderModuleCreateInfo createInfo{};
        createInfo.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
        createInfo.codeSize = code.size();
        createInfo.pCode = reinterpret_cast<const uint32_t*>(code.data());

        VkShaderModule module = VK_NULL_HANDLE;
        if (vkCreateShaderModule(device_, &createInfo, nullptr, &module) != VK_SUCCESS) {
            throw std::runtime_error("Shader module creation failed");
        }
        return module;
    }

    void createGraphicsPipeline() {
        // Graphics pipeline 将 shader 及绝大部分固定功能状态预先编译成不可变对象。
        // 本例 viewport/scissor 也是静态状态，所以窗口尺寸变化时必须重建 pipeline。
        const auto vertexCode = readBinaryFile(
            std::filesystem::path(VULKAN_SHADER_DIR) / "fullscreen.vert.spv");
        const auto fragmentCode = readBinaryFile(
            std::filesystem::path(VULKAN_SHADER_DIR) / "neon.frag.spv");
        const VkShaderModule vertexModule = createShaderModule(vertexCode);
        const VkShaderModule fragmentModule = createShaderModule(fragmentCode);

        VkPipelineShaderStageCreateInfo vertexStage{};
        vertexStage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        vertexStage.stage = VK_SHADER_STAGE_VERTEX_BIT;
        vertexStage.module = vertexModule;
        vertexStage.pName = "main";

        VkPipelineShaderStageCreateInfo fragmentStage{};
        fragmentStage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        fragmentStage.stage = VK_SHADER_STAGE_FRAGMENT_BIT;
        fragmentStage.module = fragmentModule;
        fragmentStage.pName = "main";

        const VkPipelineShaderStageCreateInfo stages[] = {vertexStage, fragmentStage};

        VkPipelineVertexInputStateCreateInfo vertexInput{};
        vertexInput.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
        // vertex shader 用 gl_VertexIndex 生成全屏三角形，因此没有 vertex buffer 或顶点属性。

        VkPipelineInputAssemblyStateCreateInfo inputAssembly{};
        inputAssembly.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
        inputAssembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;

        VkViewport viewport{};
        viewport.x = 0.0F;
        viewport.y = 0.0F;
        viewport.width = static_cast<float>(swapChainExtent_.width);
        viewport.height = static_cast<float>(swapChainExtent_.height);
        viewport.minDepth = 0.0F;
        viewport.maxDepth = 1.0F;

        VkRect2D scissor{};
        scissor.offset = {0, 0};
        scissor.extent = swapChainExtent_;

        VkPipelineViewportStateCreateInfo viewportState{};
        viewportState.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
        viewportState.viewportCount = 1;
        viewportState.pViewports = &viewport;
        viewportState.scissorCount = 1;
        viewportState.pScissors = &scissor;

        VkPipelineRasterizationStateCreateInfo rasterizer{};
        rasterizer.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
        rasterizer.polygonMode = VK_POLYGON_MODE_FILL;
        rasterizer.cullMode = VK_CULL_MODE_NONE;
        rasterizer.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
        rasterizer.lineWidth = 1.0F;

        VkPipelineMultisampleStateCreateInfo multisampling{};
        multisampling.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
        multisampling.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

        VkPipelineColorBlendAttachmentState colorBlendAttachment{};
        colorBlendAttachment.colorWriteMask = VK_COLOR_COMPONENT_R_BIT |
                                              VK_COLOR_COMPONENT_G_BIT |
                                              VK_COLOR_COMPONENT_B_BIT |
                                              VK_COLOR_COMPONENT_A_BIT;

        VkPipelineColorBlendStateCreateInfo colorBlending{};
        colorBlending.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
        colorBlending.attachmentCount = 1;
        colorBlending.pAttachments = &colorBlendAttachment;

        VkPushConstantRange pushConstantRange{};
        // Pipeline layout 是 shader 可见资源（descriptor set 与 push constant）的接口契约。
        // 本例没有 descriptor set，只有 fragment shader 可读的一段 push constant。
        pushConstantRange.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
        pushConstantRange.offset = 0;
        pushConstantRange.size = sizeof(PushConstants);

        VkPipelineLayoutCreateInfo pipelineLayoutInfo{};
        pipelineLayoutInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
        pipelineLayoutInfo.pushConstantRangeCount = 1;
        pipelineLayoutInfo.pPushConstantRanges = &pushConstantRange;
        if (vkCreatePipelineLayout(device_, &pipelineLayoutInfo, nullptr, &pipelineLayout_) != VK_SUCCESS) {
            vkDestroyShaderModule(device_, fragmentModule, nullptr);
            vkDestroyShaderModule(device_, vertexModule, nullptr);
            throw std::runtime_error("Pipeline layout creation failed");
        }

        VkGraphicsPipelineCreateInfo pipelineInfo{};
        pipelineInfo.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
        pipelineInfo.stageCount = 2;
        pipelineInfo.pStages = stages;
        pipelineInfo.pVertexInputState = &vertexInput;
        pipelineInfo.pInputAssemblyState = &inputAssembly;
        pipelineInfo.pViewportState = &viewportState;
        pipelineInfo.pRasterizationState = &rasterizer;
        pipelineInfo.pMultisampleState = &multisampling;
        pipelineInfo.pColorBlendState = &colorBlending;
        pipelineInfo.layout = pipelineLayout_;
        pipelineInfo.renderPass = renderPass_;
        pipelineInfo.subpass = 0;

        const auto result = vkCreateGraphicsPipelines(
            device_, VK_NULL_HANDLE, 1, &pipelineInfo, nullptr, &graphicsPipeline_);
        // Pipeline 已吸收所需的 shader 代码，之后 module 即可销毁；pipeline 本身仍然有效。
        vkDestroyShaderModule(device_, fragmentModule, nullptr);
        vkDestroyShaderModule(device_, vertexModule, nullptr);
        if (result != VK_SUCCESS) {
            throw std::runtime_error("Graphics pipeline creation failed");
        }
    }

    void createFramebuffers() {
        // Render pass 描述“怎样渲染”，framebuffer 则为每张 swapchain image 绑定“渲染到哪里”。
        swapChainFramebuffers_.resize(swapChainImageViews_.size());
        for (size_t i = 0; i < swapChainImageViews_.size(); ++i) {
            const VkImageView attachments[] = {swapChainImageViews_[i]};
            VkFramebufferCreateInfo createInfo{};
            createInfo.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
            createInfo.renderPass = renderPass_;
            createInfo.attachmentCount = 1;
            createInfo.pAttachments = attachments;
            createInfo.width = swapChainExtent_.width;
            createInfo.height = swapChainExtent_.height;
            createInfo.layers = 1;

            if (vkCreateFramebuffer(device_, &createInfo, nullptr, &swapChainFramebuffers_[i]) !=
                VK_SUCCESS) {
                throw std::runtime_error("Framebuffer creation failed");
            }
        }
    }

    void createCommandPool() {
        // Command pool 管理 command buffer 的宿主端内存，并绑定到一个 queue family。
        // 对同一个 pool 的 allocate/free/reset/record 需要外部同步；多线程录制时通常每线程各用一个 pool，
        // 最终仍可把这些 command buffer 提交给同一 graphics queue（提交同一 queue 时应用侧加锁）。
        const auto indices = findQueueFamilies(physicalDevice_);
        VkCommandPoolCreateInfo createInfo{};
        createInfo.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
        createInfo.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
        createInfo.queueFamilyIndex = indices.graphicsFamily.value();
        if (vkCreateCommandPool(device_, &createInfo, nullptr, &commandPool_) != VK_SUCCESS) {
            throw std::runtime_error("Command pool creation failed");
        }
    }

    void createCommandBuffers() {
        // Primary command buffer 可以直接提交到 queue；大型渲染器可让工作线程并行录制 secondary buffers，
        // 再由主线程把它们组合进 primary buffer。本示例规模小，只在主线程录制 primary buffer。
        VkCommandBufferAllocateInfo allocateInfo{};
        allocateInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
        allocateInfo.commandPool = commandPool_;
        allocateInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        allocateInfo.commandBufferCount = static_cast<uint32_t>(commandBuffers_.size());
        if (vkAllocateCommandBuffers(device_, &allocateInfo, commandBuffers_.data()) != VK_SUCCESS) {
            throw std::runtime_error("Command buffer allocation failed");
        }
    }

    void recordCommandBuffer(VkCommandBuffer commandBuffer, uint32_t imageIndex) {
        // 以下 vkCmd* 只把命令编码到 CPU 侧 command buffer；此函数返回时 GPU 可能完全没有开始本帧。
        // command buffer 在 pending（已提交、未完成）期间不能 reset 或再次录制，drawFrame 的 fence 保证了这一点。
        VkCommandBufferBeginInfo beginInfo{};
        beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
        beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        if (vkBeginCommandBuffer(commandBuffer, &beginInfo) != VK_SUCCESS) {
            throw std::runtime_error("Command buffer recording failed to begin");
        }

        const VkClearValue clearColor = {{{0.003F, 0.005F, 0.012F, 1.0F}}};
        VkRenderPassBeginInfo renderPassInfo{};
        renderPassInfo.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
        renderPassInfo.renderPass = renderPass_;
        renderPassInfo.framebuffer = swapChainFramebuffers_[imageIndex];
        renderPassInfo.renderArea.offset = {0, 0};
        renderPassInfo.renderArea.extent = swapChainExtent_;
        renderPassInfo.clearValueCount = 1;
        renderPassInfo.pClearValues = &clearColor;

        vkCmdBeginRenderPass(commandBuffer, &renderPassInfo, VK_SUBPASS_CONTENTS_INLINE);
        vkCmdBindPipeline(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, graphicsPipeline_);

        const auto now = std::chrono::steady_clock::now();
        const float elapsed = std::chrono::duration<float>(now - startTime_).count();
        double cursorX = 0.0;
        double cursorY = 0.0;
        glfwGetCursorPos(window_, &cursorX, &cursorY);

        // 本帧 host 数据路径：CPU 读取时钟/鼠标 -> 写入 constants -> 录入 push-constant 命令
        // -> vkQueueSubmit 后由 GPU 在 fragment shader 中读取。这里没有 CPU/GPU 共享映射内存，
        // 因而不涉及 non-coherent memory 的 vkFlushMappedMemoryRanges。
        PushConstants constants{};
        constants.time = elapsed;
        constants.width = static_cast<float>(swapChainExtent_.width);
        constants.height = static_cast<float>(swapChainExtent_.height);
        constants.aspect = constants.width / constants.height;
        constants.mouseX = static_cast<float>(cursorX) / constants.width;
        constants.mouseY = 1.0F - static_cast<float>(cursorY) / constants.height;
        vkCmdPushConstants(
            commandBuffer,
            pipelineLayout_,
            VK_SHADER_STAGE_FRAGMENT_BIT,
            0,
            sizeof(constants),
            &constants);

        // 3 个顶点组成覆盖屏幕的单个三角形；fragment shader 为 swapchain image 的像素着色。
        vkCmdDraw(commandBuffer, 3, 1, 0, 0);
        vkCmdEndRenderPass(commandBuffer);

        if (vkEndCommandBuffer(commandBuffer) != VK_SUCCESS) {
            throw std::runtime_error("Command buffer recording failed to end");
        }
    }

    void createSyncObjects() {
        imagesInFlight_.assign(swapChainImages_.size(), VK_NULL_HANDLE);

        // 这里使用 Vulkan 1.0 binary semaphore。它表达 queue/acquire/present 间依赖，应用不能查询其值。
        // 更复杂的任务图可使用 timeline semaphore（Vulkan 1.2 或扩展），用递增计数值表达多个里程碑。
        VkSemaphoreCreateInfo semaphoreInfo{};
        semaphoreInfo.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
        VkFenceCreateInfo fenceInfo{};
        fenceInfo.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
        // 初始设为 signaled，使第一帧的 vkWaitForFences 立即通过；提交前会把它 reset 为 unsignaled。
        fenceInfo.flags = VK_FENCE_CREATE_SIGNALED_BIT;

        for (int i = 0; i < kMaxFramesInFlight; ++i) {
            if (vkCreateSemaphore(device_, &semaphoreInfo, nullptr, &imageAvailableSemaphores_[i]) !=
                    VK_SUCCESS ||
                vkCreateSemaphore(device_, &semaphoreInfo, nullptr, &renderFinishedSemaphores_[i]) !=
                    VK_SUCCESS ||
                vkCreateFence(device_, &fenceInfo, nullptr, &inFlightFences_[i]) != VK_SUCCESS) {
                throw std::runtime_error("Frame synchronization object creation failed");
            }
        }
    }

    void drawFrame() {
        /*
        一帧的同步时间线（CPU 调用在左，异步消费者在右）：

          CPU: wait frameFence -> acquire image -> record -> queueSubmit -> queuePresent -> 下一帧
                                      |             |              |
          Present engine: -------- signal imageAvailable           wait renderFinished -> 显示/回收图像
                                                    |
          Graphics queue:             wait imageAvailable -> execute commands -> signal renderFinished + frameFence

        - semaphore 连接无需 CPU 往返的设备/队列工作；
        - fence 连接 GPU -> CPU，使 CPU 能安全复用该帧槽的 command buffer 和同步对象；
        - imagesInFlight_ 额外保护具体 swapchain image，因为 acquire 顺序不保证等于 currentFrame_ 顺序。
        */

        // (1) CPU 等待本帧槽上一次提交完成。VK_TRUE 表示等待全部 fence（这里只有一个）。
        vkWaitForFences(device_, 1, &inFlightFences_[currentFrame_], VK_TRUE, UINT64_MAX);

        uint32_t imageIndex = 0;
        // (2) 从呈现引擎取得一张可渲染图像。函数返回并不代表 GPU 可以无条件写入；
        // imageAvailable semaphore 被 signal 后才建立正确的异步依赖。
        const VkResult acquireResult = vkAcquireNextImageKHR(
            device_,
            swapChain_,
            UINT64_MAX,
            imageAvailableSemaphores_[currentFrame_],
            VK_NULL_HANDLE,
            &imageIndex);

        if (acquireResult == VK_ERROR_OUT_OF_DATE_KHR) {
            recreateSwapChain();
            return;
        }
        if (acquireResult != VK_SUCCESS && acquireResult != VK_SUBOPTIMAL_KHR) {
            throw std::runtime_error("Swapchain image acquisition failed");
        }

        if (imagesInFlight_[imageIndex] != VK_NULL_HANDLE) {
            // (3) 若该图像仍关联另一帧槽的未完成提交，CPU 等待它，避免两帧同时写同一 VkImage。
            vkWaitForFences(device_, 1, &imagesInFlight_[imageIndex], VK_TRUE, UINT64_MAX);
        }
        imagesInFlight_[imageIndex] = inFlightFences_[currentFrame_];

        // 只有确定会 submit 后才 reset；若 acquire 返回 OUT_OF_DATE 并提前退出，保持 signaled 可防止下帧死锁。
        vkResetFences(device_, 1, &inFlightFences_[currentFrame_]);
        vkResetCommandBuffer(commandBuffers_[currentFrame_], 0);
        recordCommandBuffer(commandBuffers_[currentFrame_], imageIndex);

        const VkSemaphore waitSemaphores[] = {imageAvailableSemaphores_[currentFrame_]};
        // semaphore wait 的 stage mask 很关键：GPU 可提前执行不依赖 swapchain 图像的阶段，
        // 到 COLOR_ATTACHMENT_OUTPUT 真正写该图像前才等待 acquire 完成。
        const VkPipelineStageFlags waitStages[] = {VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT};
        const VkSemaphore signalSemaphores[] = {renderFinishedSemaphores_[currentFrame_]};

        VkSubmitInfo submitInfo{};
        submitInfo.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
        submitInfo.waitSemaphoreCount = 1;
        submitInfo.pWaitSemaphores = waitSemaphores;
        submitInfo.pWaitDstStageMask = waitStages;
        submitInfo.commandBufferCount = 1;
        submitInfo.pCommandBuffers = &commandBuffers_[currentFrame_];
        submitInfo.signalSemaphoreCount = 1;
        submitInfo.pSignalSemaphores = signalSemaphores;
        // (4) 提交通常很快返回；graphics queue 异步等待 imageAvailable、执行命令、signal renderFinished，
        // 完整 submit 结束时 signal fence。CPU 此后可以准备另一个帧槽，所以最多有 2 帧在途。
        if (vkQueueSubmit(graphicsQueue_, 1, &submitInfo, inFlightFences_[currentFrame_]) != VK_SUCCESS) {
            throw std::runtime_error("Graphics queue submission failed");
        }

        VkPresentInfoKHR presentInfo{};
        presentInfo.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
        presentInfo.waitSemaphoreCount = 1;
        presentInfo.pWaitSemaphores = signalSemaphores;
        presentInfo.swapchainCount = 1;
        presentInfo.pSwapchains = &swapChain_;
        presentInfo.pImageIndices = &imageIndex;

        // (5) present queue 先等待 renderFinished，确保颜色写入和必要的布局转换完成，再交给呈现引擎。
        // vkQueuePresentKHR 的返回不等于显示器已经扫描出这一帧，也不表示 present 已完全结束。
        const VkResult presentResult = vkQueuePresentKHR(presentQueue_, &presentInfo);
        if (presentResult == VK_ERROR_OUT_OF_DATE_KHR || presentResult == VK_SUBOPTIMAL_KHR ||
            framebufferResized_) {
            framebufferResized_ = false;
            recreateSwapChain();
        } else if (presentResult != VK_SUCCESS) {
            throw std::runtime_error("Queue presentation failed");
        }

        currentFrame_ = (currentFrame_ + 1) % kMaxFramesInFlight;
    }

    [[nodiscard]] bool checkValidationLayerSupport() const {
        uint32_t count = 0;
        vkEnumerateInstanceLayerProperties(&count, nullptr);
        std::vector<VkLayerProperties> layers(count);
        vkEnumerateInstanceLayerProperties(&count, layers.data());

        for (const char* requested : kValidationLayers) {
            bool found = false;
            for (const auto& available : layers) {
                if (std::strcmp(requested, available.layerName) == 0) {
                    found = true;
                    break;
                }
            }
            if (!found) {
                return false;
            }
        }
        return true;
    }

    [[nodiscard]] std::vector<const char*> requiredInstanceExtensions() const {
        uint32_t count = 0;
        const char** glfwExtensions = glfwGetRequiredInstanceExtensions(&count);
        if (glfwExtensions == nullptr) {
            throw std::runtime_error("GLFW did not provide Vulkan instance extensions");
        }

        std::vector<const char*> extensions(glfwExtensions, glfwExtensions + count);
        if (kEnableValidationLayers) {
            extensions.push_back(VK_EXT_DEBUG_UTILS_EXTENSION_NAME);
        }
        return extensions;
    }

    [[nodiscard]] QueueFamilyIndices findQueueFamilies(VkPhysicalDevice device) const {
        // graphics 是队列族能力位；present 支持取决于“队列族 + 当前 surface”的组合，需单独查询。
        QueueFamilyIndices indices;
        uint32_t count = 0;
        vkGetPhysicalDeviceQueueFamilyProperties(device, &count, nullptr);
        std::vector<VkQueueFamilyProperties> families(count);
        vkGetPhysicalDeviceQueueFamilyProperties(device, &count, families.data());

        for (uint32_t i = 0; i < count; ++i) {
            if ((families[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) != 0U) {
                indices.graphicsFamily = i;
            }
            VkBool32 presentSupport = VK_FALSE;
            vkGetPhysicalDeviceSurfaceSupportKHR(device, i, surface_, &presentSupport);
            if (presentSupport == VK_TRUE) {
                indices.presentFamily = i;
            }
            if (indices.complete()) {
                break;
            }
        }
        return indices;
    }

    [[nodiscard]] bool checkDeviceExtensionSupport(VkPhysicalDevice device) const {
        uint32_t count = 0;
        vkEnumerateDeviceExtensionProperties(device, nullptr, &count, nullptr);
        std::vector<VkExtensionProperties> available(count);
        vkEnumerateDeviceExtensionProperties(device, nullptr, &count, available.data());

        std::set<std::string> required(kDeviceExtensions.begin(), kDeviceExtensions.end());
        for (const auto& extension : available) {
            required.erase(extension.extensionName);
        }
        return required.empty();
    }

    [[nodiscard]] bool isDeviceSuitable(VkPhysicalDevice device) const {
        const auto indices = findQueueFamilies(device);
        const bool extensionsSupported = checkDeviceExtensionSupport(device);
        bool swapchainAdequate = false;
        if (extensionsSupported) {
            const auto support = querySwapChainSupport(device);
            swapchainAdequate = !support.formats.empty() && !support.presentModes.empty();
        }
        return indices.complete() && extensionsSupported && swapchainAdequate;
    }

    [[nodiscard]] SwapChainSupportDetails querySwapChainSupport(VkPhysicalDevice device) const {
        SwapChainSupportDetails details;
        vkGetPhysicalDeviceSurfaceCapabilitiesKHR(device, surface_, &details.capabilities);

        uint32_t formatCount = 0;
        vkGetPhysicalDeviceSurfaceFormatsKHR(device, surface_, &formatCount, nullptr);
        details.formats.resize(formatCount);
        if (formatCount != 0) {
            vkGetPhysicalDeviceSurfaceFormatsKHR(
                device, surface_, &formatCount, details.formats.data());
        }

        uint32_t presentModeCount = 0;
        vkGetPhysicalDeviceSurfacePresentModesKHR(device, surface_, &presentModeCount, nullptr);
        details.presentModes.resize(presentModeCount);
        if (presentModeCount != 0) {
            vkGetPhysicalDeviceSurfacePresentModesKHR(
                device, surface_, &presentModeCount, details.presentModes.data());
        }
        return details;
    }

    [[nodiscard]] VkSurfaceFormatKHR chooseSwapSurfaceFormat(
        const std::vector<VkSurfaceFormatKHR>& formats) const {
        for (const auto& format : formats) {
            if (format.format == VK_FORMAT_B8G8R8A8_SRGB &&
                format.colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR) {
                return format;
            }
        }
        return formats.front();
    }

    [[nodiscard]] VkPresentModeKHR chooseSwapPresentMode(
        const std::vector<VkPresentModeKHR>& modes) const {
        for (const auto mode : modes) {
            if (mode == VK_PRESENT_MODE_MAILBOX_KHR) {
                return mode;
            }
        }
        return VK_PRESENT_MODE_FIFO_KHR;
    }

    [[nodiscard]] VkExtent2D chooseSwapExtent(
        const VkSurfaceCapabilitiesKHR& capabilities) const {
        if (capabilities.currentExtent.width != std::numeric_limits<uint32_t>::max()) {
            return capabilities.currentExtent;
        }

        int width = 0;
        int height = 0;
        glfwGetFramebufferSize(window_, &width, &height);
        VkExtent2D extent = {
            static_cast<uint32_t>(width),
            static_cast<uint32_t>(height),
        };
        extent.width = std::clamp(
            extent.width, capabilities.minImageExtent.width, capabilities.maxImageExtent.width);
        extent.height = std::clamp(
            extent.height, capabilities.minImageExtent.height, capabilities.maxImageExtent.height);
        return extent;
    }
};

} // namespace

int main() {
    VulkanApp app;
    try {
        app.run();
    } catch (const std::exception& error) {
        std::cerr << "Fatal: " << error.what() << '\n';
        return EXIT_FAILURE;
    }
    return EXIT_SUCCESS;
}
