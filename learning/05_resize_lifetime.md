# 05：窗口变化、资源寿命与呈现完成

前置：读完本目录前面的资源、命令和同步章节，再对照 [`main.cpp`](../src/main.cpp) 的 `drawFrame()`、`cleanupSwapChain()`、`recreateSwapChain()`。本章只读原实现并给出集成示例，没有修改或编译主程序。

你已经理解“先等信号再做下一步”。现在要补上：**这个信号究竟证明哪个使用者结束了对哪一组资源的使用？** 窗口重建不是一份特殊的 API 清单，而是这个问题的直接应用。

## 1. 分开三种“完成”

| 事件 | 可以证明什么 | 不能由它直接证明什么 |
|---|---|---|
| `vkQueueSubmit` 返回 | CPU 已完成该 API 调用；成功时提交被接受 | GPU 已执行完 |
| graphics submit 的 fence 已 signal | 这次提交的命令完成，可回收其对应的帧槽命令/数据 | present 已消耗 `renderFinished`；显示器已显示 |
| 对同一张 swapchain image 再次 acquire，且该 acquire 的 semaphore/fence **已被等待** | 上一次对这张图像的呈现已结束，相关 present wait semaphore 可以复用 | 另一张图像的 present 已结束 |
| maintenance1 的 present fence 已 signal | 满足规范规定的 present 资源回收条件 | 该画面已经扫描到显示器；整个实际呈现过程必已结束 |
| 显示/呈现时间反馈 | 相应扩展所定义的呈现时刻或阶段 | 任意 CPU/GPU 资源都可销毁 |

`vkQueuePresentKHR` 返回也不是显示完成。桌面合成器可能还要合成；MAILBOX 中一张图像也可能被更新的图像替换而未实际显示。先用“呈现系统”作为抽象边界，不必把每次 signal 想成两套驱动之间的一次物理中断。

上面第三行是 [Khronos 的 semaphore 复用说明](https://docs.vulkan.org/guide/latest/swapchain_semaphore_reuse.html) 给出的可移植依据。第四行尤其容易误读：present fence 允许实现取得资源 payload 引用后就通知应用释放相应对象，规范明确不要求它等到整个呈现操作结束。见 [VkSwapchainPresentFenceInfoKHR](https://docs.vulkan.org/refpages/latest/refpages/source/VkSwapchainPresentFenceInfoKHR.html)。

## 2. 原项目有一处必须纠正的同步模型

原程序中：

```cpp
std::array<VkSemaphore, kMaxFramesInFlight> renderFinishedSemaphores_{};
// drawFrame():
const VkSemaphore signalSemaphores[] = {
    renderFinishedSemaphores_[currentFrame_]
};
```

但 `inFlightFences_[currentFrame_]` 只跟 graphics submit 关联。可能发生：

```text
帧槽 0：graphics signal S0 和 F0
CPU：  等到 F0，认为帧槽 0 可复用
present：上一次对 S0 的 wait 尚未完成
CPU：  又提交一次 signal S0       ← 不能靠 F0 排除这种复用冲突
```

Binary semaphore 要让上一次 signal 被对应 wait 消费，才能按规则开始下一轮 signal/wait。推荐将 **present 等待的 semaphore 按 swapchain image 分配和索引**。图像 I 下一轮提交先等待 acquire(I)，随后才 signal S[I]，这一依赖链覆盖上一次对 S[I] 的 present 消费。它不依赖 `currentFrame_ == imageIndex`。

| 对象 | 正确的管理维度 | 复用依据 |
|---|---|---|
| `commandBuffers_` | frame slot | 对应 graphics fence |
| `inFlightFences_` | frame slot | 等到 signal，再于下一次 submit 前 reset |
| `imageAvailableSemaphores_` | frame slot | 对应 graphics submit 已完成其中的 wait |
| present 等待的 `renderFinishedSemaphores_` | swapchain image | 下一次 acquire 同图像的依赖链，或显式 present fence |
| framebuffer/image view | swapchain image、swapchain 代次 | 所有使用者完成 |
| `imagesInFlight_` | swapchain image → graphics fence 映射 | 只跟踪 graphics 使用，不是 present 完成表 |

这里的 acquire 依赖必须真的被等待。CPU 仅拿到 `imageIndex` 后立即销毁旧 semaphore，不成立。图像重新使用由 WSI acquire 的设备侧依赖保护；单纯录制命令还没有访问 GPU 图像。`imagesInFlight_` 可作为保守的 host 端跟踪，但它并不替代 acquire 等待。

这是对原文“fence 使整组同步对象均可复用”的收窄，不是增加一个可有可无的性能优化。[官方问题与修正示意](https://docs.vulkan.org/guide/latest/swapchain_semaphore_reuse.html)。

## 3. 按图像分配的全部整合位置

以下是**对应当前类的集成片段**，不是独立程序。创建、每帧选择、重建、销毁必须一起调整；只改数组下标不完整。本节解决正常帧循环的复用；旧交换链退休仍须遵循第 6 节。

### 3.1 成员分组

```cpp
// 仍然按 kMaxFramesInFlight 分配：
std::array<VkSemaphore, kMaxFramesInFlight> imageAvailableSemaphores_{};
std::array<VkFence, kMaxFramesInFlight> inFlightFences_{};

// 替换原来的 std::array；绑定当前 swapchain 的这一代 image 列表：
std::vector<VkSemaphore> renderFinishedSemaphores_;

static void requireVk(VkResult r, const char* operation) {
    if (r != VK_SUCCESS) {
        throw std::runtime_error(std::string(operation) + ": " +
                                 std::to_string(static_cast<int>(r)));
    }
}
```

### 3.2 帧同步对象与呈现同步对象分开创建

```cpp
void createFrameSyncObjects() {
    VkSemaphoreCreateInfo si{};
    si.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
    VkFenceCreateInfo fi{};
    fi.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
    fi.flags = VK_FENCE_CREATE_SIGNALED_BIT;
    for (int i = 0; i < kMaxFramesInFlight; ++i) {
        requireVk(vkCreateSemaphore(device_, &si, nullptr,
                                    &imageAvailableSemaphores_[i]), "create acquire semaphore");
        requireVk(vkCreateFence(device_, &fi, nullptr, &inFlightFences_[i]),
                  "create frame fence");
    }
}

void createPresentSyncForCurrentSwapchain() {
    // 前置条件：新 swapchain 的 image 列表已获取；旧一代信号已移入退休记录，
    // 或已按第 6 节证明安全并销毁。本函数不能覆盖仍在使用的旧句柄。
    renderFinishedSemaphores_.assign(swapChainImages_.size(), VK_NULL_HANDLE);
    VkSemaphoreCreateInfo si{};
    si.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
    for (auto& semaphore : renderFinishedSemaphores_) {
        requireVk(vkCreateSemaphore(device_, &si, nullptr, &semaphore),
                  "create present semaphore");
    }
    imagesInFlight_.assign(swapChainImages_.size(), VK_NULL_HANDLE);
}
```

初始化时，在取得 swapchain images 后分别调用这两个函数，取代原 `createSyncObjects()`。真正工程应使用 RAII/作用域清理处理创建一半失败的情况；此处沿用原类的异常报告风格，异常即终止本次渲染流程。

### 3.3 每帧选择信号；保持两边使用同一个句柄

在 acquire 成功或返回 `VK_SUBOPTIMAL_KHR`、得到 `imageIndex` 后：

```cpp
const VkSemaphore waitSemaphores[] = {
    imageAvailableSemaphores_[currentFrame_]
};
const VkPipelineStageFlags waitStages[] = {
    VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT
};
const VkSemaphore signalSemaphores[] = {
    renderFinishedSemaphores_.at(imageIndex) // 这里必须是 imageIndex
};

VkSubmitInfo submitInfo{};
submitInfo.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
submitInfo.waitSemaphoreCount = 1;
submitInfo.pWaitSemaphores = waitSemaphores;
submitInfo.pWaitDstStageMask = waitStages;
submitInfo.commandBufferCount = 1;
submitInfo.pCommandBuffers = &commandBuffers_[currentFrame_];
submitInfo.signalSemaphoreCount = 1;
submitInfo.pSignalSemaphores = signalSemaphores;

// 建议把原来的 reset fence 移到录制成功之后、submit 之前。
requireVk(vkResetFences(device_, 1, &inFlightFences_[currentFrame_]), "reset fence");
requireVk(vkQueueSubmit(graphicsQueue_, 1, &submitInfo,
                        inFlightFences_[currentFrame_]), "submit");
imagesInFlight_[imageIndex] = inFlightFences_[currentFrame_];

VkPresentInfoKHR pi{};
pi.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
pi.waitSemaphoreCount = 1;
pi.pWaitSemaphores = signalSemaphores; // 同一个 S[imageIndex]
pi.swapchainCount = 1;
pi.pSwapchains = &swapChain_;
pi.pImageIndices = &imageIndex;
const VkResult presentResult = vkQueuePresentKHR(presentQueue_, &pi);
// 接回原有 presentResult 状态处理；重建和退出须采用第 6 节的退休策略。
```

`requireVk` 不能直接套在 acquire/present 外面：它们的 `SUBOPTIMAL`、`OUT_OF_DATE` 是需要单独处理的 WSI 状态。`vkWaitForFences`、`vkResetCommandBuffer`、`vkResetFences` 的错误也应检查。原程序部分等待/重置忽略了返回值，扩展工程时要补上。

若 submit 失败，reset 后的 fence 可能永远不会 signal。这个片段会抛出异常离开正常帧循环；不能 catch 后简单继续下一帧。设备丢失要进入设备丢失恢复/退出路径。

### 3.4 重建与销毁的两个额外整合点

新 swapchain 的图像数可能变化，因此每次重建都要为新图像列表调用 `createPresentSyncForCurrentSwapchain()`。旧 semaphore 列表跟着旧 swapchain 进入退休记录，不能直接 `clear()` 丢失句柄，也不能照旧只销毁前两个元素。

当第 6 节的退休条件已满足后，才可用下面的循环销毁那一代的信号：

```cpp
// 前置：这组 semaphore 的 graphics signal 与 present wait 都已不再使用它们。
for (VkSemaphore semaphore : retiredPresentSemaphores) {
    if (semaphore != VK_NULL_HANDLE) {
        vkDestroySemaphore(device_, semaphore, nullptr);
    }
}
retiredPresentSemaphores.clear();
```

`cleanup()` 原来按帧的循环仅保留 `imageAvailableSemaphores_` 和 `inFlightFences_` 的销毁。present semaphore 的销毁迁到每代 swapchain 的退休路径。需要记录的不是“某个 frame 编号”，而是“某一代 swapchain 的全部 present 使用”。

## 4. 为什么窗口尺寸变化会连锁重建

可以把 swapchain 的每次成功创建视为一代：G0、G1、G2。即使 G0 与 G1 的 `imageIndex` 都是 0，它们也是不同的 image。

| 资源/状态 | 依赖尺寸或格式的原因 | 当前程序怎么做 | 可以优化吗 |
|---|---|---|---|
| Swapchain 及其 images | 图像 extent、格式、呈现配置 | 重建 | 仍须按 WSI 状态判断 |
| Image views | 引用旧的 image | 重建 | 新 images 必须有新 views |
| Framebuffers | 引用 views，创建时记录宽高 | 重建 | 本例经典 render pass 路径需要 |
| Render pass | 记录 attachment format，不记录窗口宽高 | 每次重建 | 若新旧 attachment 兼容，可以保留 |
| Graphics pipeline | **本例 viewport/scissor 是静态状态**，还依赖 render pass 兼容性 | 每次重建 | 改为动态 viewport/scissor 后，单纯尺寸变化可保留；格式等兼容性仍检查 |
| Pipeline layout | push constant 范围和 descriptor set 布局 | 每次重建 | 本例布局未变，通常可保留 |
| Command buffers | 录下旧 framebuffer、pipeline 等引用 | 每帧 reset 后重录 | pool 不需因窗口变化重建；旧引用失效的命令不能再提交 |
| `imagesInFlight_` | 索引旧 image 列表 | 重新设为 null | 必须清掉旧代映射 |
| 按 image 的 present semaphores | 数量/复用链属于旧代 images | 建議随代管理 | 旧代保留到退休 |
| Depth/MSAA/offscreen images | 本例没有；以后若使用通常与渲染分辨率相关 | 尚无 | 加入后也要重建 views、framebuffers、相关 descriptors |
| 相机投影/分辨率参数 | aspect 改变，shader 使用分辨率 | push constants 每帧更新 | 以后 UBO 中的投影矩阵也要更新 |
| Device、queues、command pools | 与窗口尺寸没有直接依赖 | 保留 | 不需要重建整个 Vulkan |

销毁需要满足两条独立条件：没有尚未完成的使用者；没有后续仍会提交的旧引用。比如已录制但未提交的 command buffer 不会自己保护被引用对象。资源销毁可能使这样的命令失效，应 reset/重录它们。

## 5. 先处理状态机，再考虑重建速度

推荐按下面的顺序理解 `drawFrame()`：

```text
wait 本 frame 的 fence
acquire
  OUT_OF_DATE → 没有取得可用的本帧 image → 重建/退出，fence 不 reset
  SUCCESS / SUBOPTIMAL → 已取得 image，继续提交并消费 acquire 信号
wait 必要的旧 graphics 使用
reset command buffer，录制新命令
reset fence，submit
present
  OUT_OF_DATE / SUBOPTIMAL / resize 标志 → 请求重建
  其他错误 → 专门的错误处理
轮换 frame slot
```

原代码在 acquire 的 `OUT_OF_DATE` 之后才 reset fence，这个位置关系是对的。若先 reset 后因重建 return，下次就可能等一个永远没人 signal 的 fence。[Khronos 的重建教程也专门解释这个死锁](https://docs.vulkan.org/tutorial/latest/03_Drawing_a_triangle/04_Swap_chain_recreation.html)。

`SUBOPTIMAL` 表示仍可使用但配置已不理想。原代码允许 acquire 返回它，然后完成本帧；可另外记录 `acquireSuboptimal`，在 present 后统一触发重建。若 acquire 成功后直接取消本帧，就需要处理已取得的 image 和已经安排的 acquire 信号；没有 maintenance1 时，走完本帧通常更简单。

窗口最小化时 framebuffer 可以是 0×0；不能拿它创建普通 swapchain。原代码会 `glfwWaitEvents()`，但循环没有检查关闭窗口，因此建议用此片段替换零尺寸循环：

```cpp
// 放在 recreateSwapChain() 开始处；保留 void 返回类型时，return 表示结束本次重建。
int width = 0, height = 0;
for (;;) {
    if (glfwWindowShouldClose(window_) == GLFW_TRUE) {
        return; // 让 mainLoop 退出；随后进入正常退休/清理路径
    }
    glfwGetFramebufferSize(window_, &width, &height);
    if (width > 0 && height > 0) {
        break;
    }
    glfwWaitEvents();
}
```

也可令重建函数返回 `bool`，显式告诉调用者“窗口已关闭”。在主循环 `glfwPollEvents()` 后也检查关闭标志，避免关闭事件后再启动一帧。读 framebuffer 像素尺寸；鼠标/窗口逻辑尺寸与它可能因 DPI 缩放不同。

## 6. 旧交换链什么时候才能销毁

原项目采用 `vkDeviceWaitIdle()` 后立即销毁并重建，属于常见教学做法。它便于等待普通 GPU 命令使用完旧 framebuffer、pipeline 等对象，但**不要把它升级成“所有 WSI 资源完成的严格证据”**。未扩展的 `vkQueuePresentKHR` 没有直接的 present fence；Khronos Guide 特别指出，未扩展 WSI 下，仅靠 WaitIdle 在退出时释放 present semaphore/swapchain 存在规范上的缺口，很多程序实际仍这样做。验证层没有报错也不是更强的完成证明。[官方对 shutdown 的说明](https://docs.vulkan.org/guide/latest/swapchain_semaphore_reuse.html)。

因此有两条工程路线：

**路线 A：查询并启用 swapchain maintenance1。** 当前名称为 `VK_KHR_swapchain_maintenance1`；也有 `VK_EXT_swapchain_maintenance1` 路径及相应别名。不能因为头文件定义了结构就直接使用：须枚举扩展、满足所选扩展的依赖、通过 `vkGetPhysicalDeviceFeatures2` 查询 `swapchainMaintenance1`，并在 `vkCreateDevice` 的 `pNext` 中启用。本项目目前只启用了 `VK_KHR_swapchain`，尚不满足这些条件。[KHR 扩展定义](https://docs.vulkan.org/refpages/latest/refpages/source/VK_KHR_swapchain_maintenance1.html)。

对当前 Vulkan 1.1 基线，KHR 路径在 instance 侧还需要 `VK_KHR_surface_maintenance1` 与其依赖 `VK_KHR_get_surface_capabilities2`、`VK_KHR_surface`；保留 GLFW 所需的平台 surface 扩展。device 侧启用 `VK_KHR_swapchain_maintenance1` 和已有的 `VK_KHR_swapchain`。EXT 路径使用 `VK_EXT_surface_maintenance1` / `VK_EXT_swapchain_maintenance1`，并满足各自列出的依赖。instance 扩展必须在创建 instance 时启用，不能等到 device 创建后补填。[KHR surface 依赖](https://docs.vulkan.org/refpages/latest/refpages/source/VK_KHR_surface_maintenance1.html)、[EXT surface 依赖](https://docs.vulkan.org/refpages/latest/refpages/source/VK_EXT_surface_maintenance1.html)。

启用后，每次 present 可附加一个专用且当前未 signal 的 fence。下面是 API 集成片段，假设 `presentFence` 已创建、未被其他 pending 操作使用，且应用会记录哪些请求已被接受：

```cpp
VkSwapchainPresentFenceInfoKHR presentFenceInfo{};
presentFenceInfo.sType = VK_STRUCTURE_TYPE_SWAPCHAIN_PRESENT_FENCE_INFO_KHR;
presentFenceInfo.swapchainCount = 1;
presentFenceInfo.pFences = &presentFence; // 不是 graphics submit 的同一个 pending fence
presentInfo.pNext = &presentFenceInfo;
const VkResult r = vkQueuePresentKHR(presentQueue_, &presentInfo);
// 保存 {swapchainGeneration, imageIndex, waitSemaphore, presentFence, r}。
// 实际请求/错误状态须按 vkQueuePresentKHR 规范处理，不能对从未入队的 fence 无限等。
```

回收某代时，等待该代普通 GPU 使用完成，并等待该代所有已提交 present 请求对应的 present fences。这样才能释放那一代的 swapchain 与 present 等待信号。记录式回收可用 `vkGetFenceStatus` 轮询，避免每帧阻塞；fence 的重置也必须晚于对应请求完成。present 返回 `OUT_OF_DATE` 不表示其信号等待必然没有入队，不能把所有错误都当作“没有任何操作发生”。[present fence 的精确定义](https://docs.vulkan.org/refpages/latest/refpages/source/VkSwapchainPresentFenceInfoKHR.html)、[vkQueuePresentKHR](https://docs.vulkan.org/refpages/latest/refpages/source/vkQueuePresentKHR.html)。

**路线 B：保持未扩展 WSI，维护 acquire/present 历史并延后回收。** 稳定渲染时按图像复用 semaphore；重建时旧链不会再正常 acquire，故需要将旧 swapchain 及剩余信号放入退休列表。Khronos 的官方 sample 展示如何依据新链呈现及随后 acquire 的完成回收旧代，并处理连续 resize 产生多代旧链的情况。不要用一个 `waitFrameFences()` 函数假装替代这些跟踪。未扩展 shutdown 的缺口也必须单独理解。[完整官方 Swapchain Recreation 示例](https://docs.vulkan.org/samples/latest/samples/api/swapchain_recreation/README.html)。

`oldSwapchain` 参数能让实现利用旧链信息，应用仍负责其寿命。新链创建不会替你完成所有旧帧；被退休的旧链不能继续像活动链那样 acquire。初始化失败的 rollback、连续 resize、surface lost、device lost 都应是状态机分支，不是随意忽略返回值。

## 7. 在另一台电脑上的练习与验收

本次没有执行以下实验。建议逐项进行，每次保留一个可以回退的版本。

1. 先按第 3 节完成 frame/image 两类同步对象的拆分，打印 `frameSlot、swapchainGeneration、imageIndex`。看到三者不一致时，能解释每个对象该取哪个下标。
2. 将 viewport/scissor 改为动态状态，在绑定 pipeline 后录 `vkCmdSetViewport`、`vkCmdSetScissor`。尺寸变化时保留 pipeline，格式/兼容性改变时仍重建。验收：拉伸窗口无旧尺寸裁剪。
3. 最小化后关闭窗口。验收：不在零尺寸等待循环中挂住；没有等待一个从未提交的 fence。
4. 实现一张退休记录表，给每个资源标明“最后使用它的提交/呈现事件”。能说明为什么纹理的 graphics fence 和 swapchain 的 present fence 生命周期不同。
5. 学习官方 recreation sample 的扩展与非扩展路径；能指出“资源可回收”与“画面显示完成”不是同一事件。

能独立回答这两个问题，就说明你真正掌握了本章：**这个对象最后一个异步使用者是谁？我等待的信号是否恰好覆盖它？**
