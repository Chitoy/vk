#include <vulkan/vulkan.h>

#include <array>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>
#include <vector>

#ifndef COMPUTE_SHADER_PATH
#define COMPUTE_SHADER_PATH "transform.comp.spv"
#endif

namespace {

constexpr uint32_t kCount = 16;
constexpr uint32_t kLocalSize = 64; // 必须与 transform.comp 一致。
constexpr uint32_t kGroupCount = (kCount + kLocalSize - 1) / kLocalSize;
constexpr VkDeviceSize kBytes = VkDeviceSize{kCount} * sizeof(uint32_t);
static_assert(sizeof(uint32_t) == 4);
static_assert(sizeof(std::array<uint32_t, kCount>) == kBytes);

void check(VkResult result, const char* operation) {
    if (result != VK_SUCCESS) {
        throw std::runtime_error(std::string(operation) + " failed: VkResult=" +
                                 std::to_string(static_cast<int>(result)));
    }
}

// Vulkan 的两阶段枚举允许第二次调用返回 VK_INCOMPLETE，需要重新查询。
template<class T, class Query>
std::vector<T> enumerate(const char* operation, Query query) {
    for (;;) {
        uint32_t count = 0;
        check(query(&count, nullptr), operation);
        if (count == 0) {
            return {};
        }
        std::vector<T> values(count);
        const VkResult result = query(&count, values.data());
        if (result == VK_INCOMPLETE) {
            continue;
        }
        check(result, operation);
        values.resize(count);
        return values;
    }
}

std::vector<uint32_t> readSpirv(const std::filesystem::path& path) {
    std::ifstream file(path, std::ios::binary | std::ios::ate);
    if (!file) {
        throw std::runtime_error("Cannot open shader: " + path.string());
    }
    const std::streamoff bytes = file.tellg();
    if (bytes <= 0 || bytes % 4 != 0 ||
        static_cast<uintmax_t>(bytes) > std::numeric_limits<size_t>::max() ||
        bytes > std::numeric_limits<std::streamsize>::max()) {
        throw std::runtime_error("Invalid SPIR-V byte count: " + path.string());
    }
    // uint32_t 容器满足 VkShaderModuleCreateInfo::pCode 的对齐要求。
    std::vector<uint32_t> words(static_cast<size_t>(bytes) / sizeof(uint32_t));
    file.seekg(0);
    file.read(reinterpret_cast<char*>(words.data()), static_cast<std::streamsize>(bytes));
    if (!file || words[0] != 0x07230203u) {
        throw std::runtime_error("Cannot read SPIR-V or invalid magic: " + path.string());
    }
    return words;
}

class ComputeExample {
public:
    ComputeExample() = default;
    ComputeExample(const ComputeExample&) = delete;
    ComputeExample& operator=(const ComputeExample&) = delete;

    ~ComputeExample() {
        // 正常路径已等 fence；异常路径也先结束设备工作再释放资源。
        // 析构函数不能抛异常，仍报告 vkDeviceWaitIdle 的错误。
        if (device_ != VK_NULL_HANDLE) {
            const VkResult result = vkDeviceWaitIdle(device_);
            if (result != VK_SUCCESS) {
                std::cerr << "Cleanup vkDeviceWaitIdle: " << static_cast<int>(result) << '\n';
            }
            if (mapped_ != nullptr) vkUnmapMemory(device_, memory_);
            vkDestroyFence(device_, fence_, nullptr);
            vkDestroyCommandPool(device_, commandPool_, nullptr); // 隐式释放 command buffer。
            vkDestroyPipeline(device_, pipeline_, nullptr);
            vkDestroyShaderModule(device_, shader_, nullptr);
            vkDestroyPipelineLayout(device_, pipelineLayout_, nullptr);
            vkDestroyDescriptorPool(device_, descriptorPool_, nullptr); // 隐式释放 descriptor set。
            vkDestroyDescriptorSetLayout(device_, setLayout_, nullptr);
            vkDestroyBuffer(device_, buffer_, nullptr); // 先解除资源生命周期，再释放底层内存。
            vkFreeMemory(device_, memory_, nullptr);
            vkDestroyDevice(device_, nullptr);
        }
        if (instance_ != VK_NULL_HANDLE) vkDestroyInstance(instance_, nullptr);
    }

    void run(const std::filesystem::path& shaderPath, bool preferNonCoherent) {
        createInstance();
        chooseDeviceAndQueue();
        createDevice();
        createBufferAndMemory(preferNonCoherent);
        createDescriptors();
        createPipeline(shaderPath);
        writeInput();
        recordCommands();
        submitAndRead();
    }

private:
    VkInstance instance_ = VK_NULL_HANDLE;
    VkPhysicalDevice physical_ = VK_NULL_HANDLE;
    VkPhysicalDeviceProperties properties_{};
    VkPhysicalDeviceMemoryProperties memoryProperties_{};
    uint32_t family_ = 0;
    VkDevice device_ = VK_NULL_HANDLE;
    VkQueue queue_ = VK_NULL_HANDLE;
    VkBuffer buffer_ = VK_NULL_HANDLE;
    VkDeviceMemory memory_ = VK_NULL_HANDLE;
    VkDeviceSize allocationBytes_ = 0;
    void* mapped_ = nullptr;
    bool coherent_ = false;
    VkDescriptorSetLayout setLayout_ = VK_NULL_HANDLE;
    VkDescriptorPool descriptorPool_ = VK_NULL_HANDLE;
    VkDescriptorSet descriptorSet_ = VK_NULL_HANDLE;
    VkPipelineLayout pipelineLayout_ = VK_NULL_HANDLE;
    VkShaderModule shader_ = VK_NULL_HANDLE;
    VkPipeline pipeline_ = VK_NULL_HANDLE;
    VkCommandPool commandPool_ = VK_NULL_HANDLE;
    VkCommandBuffer command_ = VK_NULL_HANDLE;
    VkFence fence_ = VK_NULL_HANDLE;

    void createInstance() {
        const auto layers = enumerate<VkLayerProperties>("vkEnumerateInstanceLayerProperties",
            [](uint32_t* count, VkLayerProperties* data) {
                return vkEnumerateInstanceLayerProperties(count, data);
            });
        const char* validation = "VK_LAYER_KHRONOS_validation";
        bool hasValidation = false;
        for (const auto& layer : layers) {
            if (std::strcmp(layer.layerName, validation) == 0) hasValidation = true;
        }
        VkApplicationInfo app{};
        app.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
        app.pApplicationName = "Learning: CPU -> SSBO -> compute -> CPU";
        app.apiVersion = VK_API_VERSION_1_1;
        VkInstanceCreateInfo info{};
        info.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
        info.pApplicationInfo = &app;
        // 有校验层就启用；没有也可运行。不创建窗口，不需要 surface/swapchain 扩展。
        info.enabledLayerCount = hasValidation ? 1u : 0u;
        info.ppEnabledLayerNames = hasValidation ? &validation : nullptr;
        check(vkCreateInstance(&info, nullptr, &instance_), "vkCreateInstance");
        std::cout << "Validation layer: " << (hasValidation ? "enabled" : "unavailable") << '\n';
    }

    void chooseDeviceAndQueue() {
        const auto devices = enumerate<VkPhysicalDevice>("vkEnumeratePhysicalDevices",
            [this](uint32_t* count, VkPhysicalDevice* data) {
                return vkEnumeratePhysicalDevices(instance_, count, data);
            });
        for (VkPhysicalDevice candidate : devices) {
            VkPhysicalDeviceProperties props{};
            vkGetPhysicalDeviceProperties(candidate, &props);
            if (props.apiVersion < VK_API_VERSION_1_1 ||
                props.limits.maxComputeWorkGroupInvocations < kLocalSize ||
                props.limits.maxComputeWorkGroupSize[0] < kLocalSize ||
                props.limits.maxComputeWorkGroupCount[0] < kGroupCount ||
                props.limits.maxStorageBufferRange < kBytes) {
                continue;
            }
            uint32_t count = 0;
            vkGetPhysicalDeviceQueueFamilyProperties(candidate, &count, nullptr);
            std::vector<VkQueueFamilyProperties> families(count);
            vkGetPhysicalDeviceQueueFamilyProperties(candidate, &count, families.data());
            families.resize(count);
            for (uint32_t i = 0; i < count; ++i) {
                if (families[i].queueCount > 0 &&
                    (families[i].queueFlags & VK_QUEUE_COMPUTE_BIT) != 0) {
                    physical_ = candidate;
                    properties_ = props;
                    family_ = i;
                    vkGetPhysicalDeviceMemoryProperties(physical_, &memoryProperties_);
                    std::cout << "GPU: " << properties_.deviceName << "; compute family: " << family_ << '\n';
                    return;
                }
            }
        }
        throw std::runtime_error("No Vulkan 1.1 device has a suitable compute queue and limits");
    }

    void createDevice() {
        const float priority = 1.0f;
        VkDeviceQueueCreateInfo queueInfo{};
        queueInfo.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
        queueInfo.queueFamilyIndex = family_;
        queueInfo.queueCount = 1;
        queueInfo.pQueuePriorities = &priority;
        VkDeviceCreateInfo info{};
        info.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
        info.queueCreateInfoCount = 1;
        info.pQueueCreateInfos = &queueInfo;
        check(vkCreateDevice(physical_, &info, nullptr, &device_), "vkCreateDevice");
        vkGetDeviceQueue(device_, family_, 0, &queue_);
    }

    uint32_t chooseMemoryType(uint32_t supportedBits, bool preferNonCoherent) const {
        constexpr VkMemoryPropertyFlags basicProperties =
            VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT | VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
            VK_MEMORY_PROPERTY_HOST_COHERENT_BIT | VK_MEMORY_PROPERTY_HOST_CACHED_BIT;
        uint32_t fallback = UINT32_MAX;
        for (uint32_t i = 0; i < memoryProperties_.memoryTypeCount; ++i) {
            const auto flags = memoryProperties_.memoryTypes[i].propertyFlags;
            if ((supportedBits & (uint32_t{1} << i)) == 0 ||
                (flags & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT) == 0 ||
                (flags & ~basicProperties) != 0) {
                // 本例没启用特殊内存特性，避开例如 DEVICE_COHERENT_BIT_AMD 的类型。
                continue;
            }
            if (fallback == UINT32_MAX) fallback = i;
            const bool coherent = (flags & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT) != 0;
            if (coherent != preferNonCoherent) return i;
        }
        if (fallback == UINT32_MAX) {
            throw std::runtime_error("This storage buffer has no compatible HOST_VISIBLE memory type");
        }
        return fallback;
    }

    void createBufferAndMemory(bool preferNonCoherent) {
        VkBufferCreateInfo bufferInfo{};
        bufferInfo.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
        bufferInfo.size = kBytes;
        bufferInfo.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
        bufferInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        check(vkCreateBuffer(device_, &bufferInfo, nullptr, &buffer_), "vkCreateBuffer");

        VkMemoryRequirements requirements{};
        vkGetBufferMemoryRequirements(device_, buffer_, &requirements);
        const uint32_t typeIndex = chooseMemoryType(requirements.memoryTypeBits, preferNonCoherent);
        const auto flags = memoryProperties_.memoryTypes[typeIndex].propertyFlags;
        coherent_ = (flags & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT) != 0;
        allocationBytes_ = requirements.size; // 可能大于 VkBuffer 的 64 字节逻辑长度。
        VkMemoryAllocateInfo allocation{};
        allocation.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
        allocation.allocationSize = allocationBytes_;
        allocation.memoryTypeIndex = typeIndex;
        check(vkAllocateMemory(device_, &allocation, nullptr, &memory_), "vkAllocateMemory");
        // offset=0 自然满足 requirements.alignment；一份 allocation 只绑定这个 buffer。
        check(vkBindBufferMemory(device_, buffer_, memory_, 0), "vkBindBufferMemory");
        // 映射整个 allocation，而非只映射 64 字节的逻辑 buffer。
        check(vkMapMemory(device_, memory_, 0, VK_WHOLE_SIZE, 0, &mapped_), "vkMapMemory");
        std::cout << "Buffer bytes: " << kBytes << "; allocation bytes: " << allocationBytes_
                  << "; binding alignment: " << requirements.alignment << '\n'
                  << "Memory type: " << typeIndex << "; heap: "
                  << memoryProperties_.memoryTypes[typeIndex].heapIndex
                  << "; coherent: " << coherent_
                  << "; device local: " << ((flags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT) != 0)
                  << "; nonCoherentAtomSize: " << properties_.limits.nonCoherentAtomSize << '\n';
        if (preferNonCoherent && coherent_) {
            std::cout << "No compatible non-coherent type; using coherent fallback.\n";
        }
    }

    VkMappedMemoryRange wholeMappedRange() const {
        VkMappedMemoryRange range{};
        range.sType = VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE;
        range.memory = memory_;
        range.offset = 0; // 0 是任意 nonCoherentAtomSize 的倍数。
        range.size = VK_WHOLE_SIZE;
        // 整块已映射；范围终点是 allocation 终点，允许不为 atomSize 的倍数。
        // 没有相邻 suballocation，所以不会误触碰其他在途资源所在的 atom。
        return range;
    }

    void createDescriptors() {
        VkDescriptorSetLayoutBinding binding{};
        binding.binding = 0;
        binding.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        binding.descriptorCount = 1;
        binding.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
        VkDescriptorSetLayoutCreateInfo layoutInfo{};
        layoutInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
        layoutInfo.bindingCount = 1;
        layoutInfo.pBindings = &binding;
        check(vkCreateDescriptorSetLayout(device_, &layoutInfo, nullptr, &setLayout_),
              "vkCreateDescriptorSetLayout");

        VkDescriptorPoolSize poolSize{};
        poolSize.type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        poolSize.descriptorCount = 1;
        VkDescriptorPoolCreateInfo poolInfo{};
        poolInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
        poolInfo.maxSets = 1;
        poolInfo.poolSizeCount = 1;
        poolInfo.pPoolSizes = &poolSize;
        check(vkCreateDescriptorPool(device_, &poolInfo, nullptr, &descriptorPool_),
              "vkCreateDescriptorPool");

        VkDescriptorSetAllocateInfo setInfo{};
        setInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
        setInfo.descriptorPool = descriptorPool_;
        setInfo.descriptorSetCount = 1;
        setInfo.pSetLayouts = &setLayout_;
        check(vkAllocateDescriptorSets(device_, &setInfo, &descriptorSet_), "vkAllocateDescriptorSets");

        VkDescriptorBufferInfo bufferInfo{};
        bufferInfo.buffer = buffer_;
        bufferInfo.offset = 0;
        bufferInfo.range = kBytes;
        VkWriteDescriptorSet write{};
        write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        write.dstSet = descriptorSet_;
        write.dstBinding = 0;
        write.descriptorCount = 1;
        write.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        write.pBufferInfo = &bufferInfo;
        // 更新的是“如何找到 buffer”这条引用，不会复制那 16 个整数。
        vkUpdateDescriptorSets(device_, 1, &write, 0, nullptr);
    }

    void createPipeline(const std::filesystem::path& path) {
        VkPushConstantRange countRange{};
        countRange.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
        countRange.offset = 0;
        countRange.size = sizeof(uint32_t);
        VkPipelineLayoutCreateInfo layoutInfo{};
        layoutInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
        layoutInfo.setLayoutCount = 1;
        layoutInfo.pSetLayouts = &setLayout_; // pSetLayouts[0] 对应 GLSL set=0。
        layoutInfo.pushConstantRangeCount = 1;
        layoutInfo.pPushConstantRanges = &countRange;
        check(vkCreatePipelineLayout(device_, &layoutInfo, nullptr, &pipelineLayout_),
              "vkCreatePipelineLayout");

        const auto words = readSpirv(path);
        VkShaderModuleCreateInfo moduleInfo{};
        moduleInfo.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
        moduleInfo.codeSize = words.size() * sizeof(uint32_t);
        moduleInfo.pCode = words.data();
        check(vkCreateShaderModule(device_, &moduleInfo, nullptr, &shader_), "vkCreateShaderModule");
        VkComputePipelineCreateInfo pipelineInfo{};
        pipelineInfo.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
        pipelineInfo.stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        pipelineInfo.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
        pipelineInfo.stage.module = shader_;
        pipelineInfo.stage.pName = "main";
        pipelineInfo.layout = pipelineLayout_;
        check(vkCreateComputePipelines(device_, VK_NULL_HANDLE, 1, &pipelineInfo, nullptr, &pipeline_),
              "vkCreateComputePipelines");
        vkDestroyShaderModule(device_, shader_, nullptr);
        shader_ = VK_NULL_HANDLE; // Pipeline 创建完后无需保留 shader module。
    }

    void writeInput() {
        std::array<uint32_t, kCount> input{};
        for (uint32_t i = 0; i < kCount; ++i) input[i] = i;
        std::memcpy(mapped_, input.data(), static_cast<size_t>(kBytes));
        if (!coherent_) {
            const auto range = wholeMappedRange();
            check(vkFlushMappedMemoryRanges(device_, 1, &range), "vkFlushMappedMemoryRanges");
        }
        // 写入和必要的 flush 都先于 vkQueueSubmit；提交提供 host -> device 的域操作。
        // CPU 此后不再碰这些字节，直到 fence 完成且必要的 invalidate 已执行。
    }

    void recordCommands() {
        VkCommandPoolCreateInfo poolInfo{};
        poolInfo.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
        poolInfo.queueFamilyIndex = family_;
        check(vkCreateCommandPool(device_, &poolInfo, nullptr, &commandPool_), "vkCreateCommandPool");
        VkCommandBufferAllocateInfo allocation{};
        allocation.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
        allocation.commandPool = commandPool_;
        allocation.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        allocation.commandBufferCount = 1;
        check(vkAllocateCommandBuffers(device_, &allocation, &command_), "vkAllocateCommandBuffers");
        VkCommandBufferBeginInfo begin{};
        begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
        begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        check(vkBeginCommandBuffer(command_, &begin), "vkBeginCommandBuffer");
        vkCmdBindPipeline(command_, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline_);
        vkCmdBindDescriptorSets(command_, VK_PIPELINE_BIND_POINT_COMPUTE, pipelineLayout_,
                                0, 1, &descriptorSet_, 0, nullptr);
        vkCmdPushConstants(command_, pipelineLayout_, VK_SHADER_STAGE_COMPUTE_BIT,
                           0, sizeof(kCount), &kCount);
        vkCmdDispatch(command_, kGroupCount, 1, 1);

        VkBufferMemoryBarrier barrier{};
        barrier.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER;
        barrier.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
        barrier.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
        barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.buffer = buffer_;
        barrier.offset = 0;
        barrier.size = kBytes;
        // Vulkan 1.1 的 legacy barrier：shader 写入 -> host 读取的内存依赖。
        // HOST_BIT 不代表 GPU 执行 CPU 代码；它命名此依赖的 host 访问作用域。
        vkCmdPipelineBarrier(command_, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                             VK_PIPELINE_STAGE_HOST_BIT, 0,
                             0, nullptr, 1, &barrier, 0, nullptr);
        check(vkEndCommandBuffer(command_), "vkEndCommandBuffer");
    }

    void submitAndRead() {
        VkFenceCreateInfo fenceInfo{};
        fenceInfo.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
        check(vkCreateFence(device_, &fenceInfo, nullptr, &fence_), "vkCreateFence");
        VkSubmitInfo submit{};
        submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
        submit.commandBufferCount = 1;
        submit.pCommandBuffers = &command_;
        check(vkQueueSubmit(queue_, 1, &submit, fence_), "vkQueueSubmit");
        check(vkWaitForFences(device_, 1, &fence_, VK_TRUE, UINT64_MAX), "vkWaitForFences");
        if (!coherent_) {
            const auto range = wholeMappedRange();
            check(vkInvalidateMappedMemoryRanges(device_, 1, &range), "vkInvalidateMappedMemoryRanges");
        }
        std::array<uint32_t, kCount> output{};
        std::memcpy(output.data(), mapped_, static_cast<size_t>(kBytes));
        std::cout << "Output:";
        for (uint32_t i = 0; i < kCount; ++i) {
            std::cout << ' ' << output[i];
            if (output[i] != i * 2u + 1u) {
                throw std::runtime_error("Readback mismatch at element " + std::to_string(i));
            }
        }
        std::cout << "\nPASS: all " << kCount << " values match value * 2 + 1.\n";
    }
};

} // namespace

int main(int argc, char** argv) {
    try {
        std::filesystem::path shaderPath = COMPUTE_SHADER_PATH;
        bool preferNonCoherent = false;
        bool hasShaderPath = false;
        for (int i = 1; i < argc; ++i) {
            const std::string argument = argv[i];
            if (argument == "--prefer-noncoherent") {
                preferNonCoherent = true;
            } else if (argument == "--help") {
                std::cout << "Usage: compute_buffer [transform.comp.spv] [--prefer-noncoherent]\n";
                return 0;
            } else if (!hasShaderPath && !argument.starts_with("--")) {
                shaderPath = argument;
                hasShaderPath = true;
            } else {
                throw std::runtime_error("Unknown argument: " + argument);
            }
        }
        ComputeExample example;
        example.run(shaderPath, preferNonCoherent);
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "Error: " << error.what() << '\n';
        return 1;
    }
}
