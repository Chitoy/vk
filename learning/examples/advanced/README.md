# 高级示例：先标明集成边界

这些示例对应 [05：窗口与寿命](../../05_resize_lifetime.md) 和 [06：线程、进程、设备](../../06_threads_processes_devices.md)。没有修改主程序，没有在本机安装、构建或运行。

| 示例 | 形式 | 接入要求 |
|---|---|---|
| 第 05 章按 image 的 present semaphores | 当前 `VulkanApp` 的集成片段 | 同时修改成员、创建、draw、重建和退休 |
| A：secondary 录制 | C++ Vulkan API 集成片段 | 已有每线程每帧 pool、primary、secondary 与同步调度 |
| B：timeline 创建与等待 | C++ Vulkan API 集成片段 | 明确选用 Vulkan 1.2 core，查询并启用 feature |
| C：Windows 生产/消费共享槽 | 架构伪代码 | 尚须实现 capability 查询、资源、IPC、所有权与失败处理 |

这里不把含有未实现 helper 的伪代码称为“可直接编译的完整程序”。完整项目入口见第 06 章末尾的 Khronos Samples。

## 例 A：经典 render pass 中的 secondary 集成片段

前置：`secondary` 已从该 worker 专用、本帧专用且未 pending 的 pool 分配，等级为 secondary。`primary` 由提交线程管理。多线程分工产生的不同 draw 应指定不同场景数据；下面仅演示当前全屏三角形的状态绑定，不能把同一 draw 重复 N 次就当作加速。

```cpp
// worker 内部：所有句柄和 pc 是该 job 的只读输入快照。
VkCommandBufferInheritanceInfo inheritance{};
inheritance.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_INHERITANCE_INFO;
inheritance.renderPass = renderPass;
inheritance.subpass = 0;
inheritance.framebuffer = framebufferForAcquiredImage;

VkCommandBufferBeginInfo begin{};
begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT |
              VK_COMMAND_BUFFER_USAGE_RENDER_PASS_CONTINUE_BIT;
begin.pInheritanceInfo = &inheritance;
if (vkBeginCommandBuffer(secondary, &begin) != VK_SUCCESS) {
    throw std::runtime_error("begin secondary failed");
}
vkCmdBindPipeline(secondary, VK_PIPELINE_BIND_POINT_GRAPHICS, graphicsPipeline);
vkCmdPushConstants(secondary, pipelineLayout, VK_SHADER_STAGE_FRAGMENT_BIT,
                   0, sizeof(pc), &pc);
// 若 pipeline 改为动态 viewport/scissor，要在此 secondary 设置它们。
// 若 shader 需要 descriptors、顶点或索引数据，也在此绑定。
vkCmdDraw(secondary, 3, 1, 0, 0);
if (vkEndCommandBuffer(secondary) != VK_SUCCESS) {
    throw std::runtime_error("end secondary failed");
}
```

主线程先完成 CPU job join，再在已经 begin 的 primary 中执行：

```cpp
// renderPassBegin 指向与上面继承信息兼容的 render pass/framebuffer，
// secondaryBuffers 是 worker 产出的有效且已结束录制的 secondary 列表。
vkCmdBeginRenderPass(primary, &renderPassBegin,
                     VK_SUBPASS_CONTENTS_SECONDARY_COMMAND_BUFFERS);
vkCmdExecuteCommands(primary,
                      static_cast<uint32_t>(secondaryBuffers.size()),
                      secondaryBuffers.data());
vkCmdEndRenderPass(primary);
// end primary，再像原 drawFrame 一样 submit primary，关联本帧 fence。
```

如果一个 frame 的 worker 命令还会提交到别的 queue，这个 graphics fence 必须通过任务依赖覆盖它们，或者分别等待那些提交的完成信号；不能未经证明就 reset 所有 pools。[官方完整示例](https://docs.vulkan.org/samples/latest/samples/performance/command_buffer_usage/README.html)。

## 例 B：timeline 的 feature、创建、提交、host wait

这是 **Vulkan 1.2 core 集成路径**。首先确认 loader 和所选 physical device 支持所选 API 版本，并把当前 instance 的应用 API 请求改为对应 1.2 路径。若保留项目 1.1，请使用 KHR 扩展启用及 KHR 函数入口，不要混用下面的前置条件。

在 `vkCreateDevice` 之前查询：

```cpp
VkPhysicalDeviceTimelineSemaphoreFeatures supportedTimeline{};
supportedTimeline.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TIMELINE_SEMAPHORE_FEATURES;
VkPhysicalDeviceFeatures2 supportedFeatures{};
supportedFeatures.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2;
supportedFeatures.pNext = &supportedTimeline;
vkGetPhysicalDeviceFeatures2(physicalDevice, &supportedFeatures);
if (supportedTimeline.timelineSemaphore != VK_TRUE) {
    throw std::runtime_error("timelineSemaphore unsupported");
}

// 新建“需要启用”的结构；不要把查询到的所有功能一股脑启用。
VkPhysicalDeviceTimelineSemaphoreFeatures enabledTimeline{};
enabledTimeline.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TIMELINE_SEMAPHORE_FEATURES;
enabledTimeline.timelineSemaphore = VK_TRUE;
enabledTimeline.pNext = deviceCreateInfo.pNext; // 保留已有合法的 feature 链
deviceCreateInfo.pNext = &enabledTimeline;
// 在上述局部结构仍存活时，执行项目已有的 vkCreateDevice。
```

创建一个初始为 0 的 timeline semaphore：

```cpp
VkSemaphoreTypeCreateInfo type{};
type.sType = VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO;
type.semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE;
type.initialValue = 0;
VkSemaphoreCreateInfo create{};
create.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
create.pNext = &type;
VkSemaphore uploadTimeline = VK_NULL_HANDLE;
if (vkCreateSemaphore(device, &create, nullptr, &uploadTimeline) != VK_SUCCESS) {
    throw std::runtime_error("create upload timeline failed");
}
```

假设 `uploadCommands` 已录制完成，所属 family 与 `uploadQueue` 匹配。提交后把里程碑从 0 推进到 1：

```cpp
const uint64_t uploaded = 1;
VkTimelineSemaphoreSubmitInfo values{};
values.sType = VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO;
values.signalSemaphoreValueCount = 1;
values.pSignalSemaphoreValues = &uploaded;

VkSubmitInfo submit{};
submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
submit.pNext = &values;
submit.commandBufferCount = 1;
submit.pCommandBuffers = &uploadCommands;
submit.signalSemaphoreCount = 1;
submit.pSignalSemaphores = &uploadTimeline;
if (vkQueueSubmit(uploadQueue, 1, &submit, VK_NULL_HANDLE) != VK_SUCCESS) {
    throw std::runtime_error("upload submit failed");
}

VkSemaphoreWaitInfo wait{};
wait.sType = VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO;
wait.semaphoreCount = 1;
wait.pSemaphores = &uploadTimeline;
wait.pValues = &uploaded;
const VkResult waited = vkWaitSemaphores(device, &wait, UINT64_MAX);
if (waited != VK_SUCCESS) {
    throw std::runtime_error("wait upload timeline failed");
}
// 现在对应上传命令已结束；若是 GPU→CPU 读回，还须遵守 HOST_READ
// 可见性依赖和 non-coherent invalidate，不能把 wait 当成全部内存协议。
```

这个例子故意展示 host wait 以便建立计数模型。真实上传→渲染通常让 graphics submit 直接等待 timeline=1，避免 CPU 阻塞；图像布局和 queue family ownership 仍按实际资源处理。只有所有使用完成后才销毁 `uploadTimeline`。[规范入口](https://docs.vulkan.org/refpages/latest/refpages/source/VK_KHR_timeline_semaphore.html)。

## 例 C：Windows 跨进程同 GPU 的单槽协议——架构伪代码

**以下所有大写动作均为尚待实现的应用逻辑，不是 Vulkan API。** 目的在于把必须补齐的环节写在同一张图上，避免只展示“导出/导入两个函数”造成已经完整的错觉。

选择：两个进程、同一块 GPU、兼容驱动、固定大小的外部 buffer、OPAQUE_WIN32 NT handles、两个永久导入的 binary semaphores。第一帧 A 初始拥有槽；A/B 永远交替访问，暂不流水线化。

```text
共同初始化：
  通过 IPC 交换协议版本、deviceUUID / driverUUID、buffer size/usage、句柄类型
  双方校验 external memory/semaphore capabilities
  A 创建外部 buffer + exportable allocation，按要求 dedicated allocation
  A 创建 exportable ready / consumed 两个 binary semaphores
  A 导出三个 NT HANDLE，并 DuplicateHandle 到 B 进程
  A 将 B 有效的句柄值和资源元数据发给 B
  B 创建兼容的本地 buffer，导入 allocation，再 bind
  B 创建自己的 semaphore 对象并永久导入 ready / consumed payload
  B 的 descriptor 引用 B 的 buffer；A 的 descriptor 引用 A 的 buffer
  双方完成导入后关闭各自不再需要的 NT HANDLE
  双方对握手成功确认，再开始 GPU 任务

A 循环 n=0,1,...：
  若 n>0：
    先等 IPC 的 consumed-submit(n-1) 已成功排队通知
    本次 submit 等 consumed；命令中 acquire external→A
  命令写入本地 buffer（即共享存储中的第 n 帧）
  命令 release A→external
  submit signal ready；确认 submit 成功后发送 ready-submit(n) 通知
  只有到下一轮确实等 consumed，才允许覆盖这个槽

B 循环 n=0,1,...：
  先等 IPC 的 ready-submit(n) 已成功排队通知
  submit wait ready；命令中 acquire external→B
  命令读取共享 buffer，把结果复制/绘制到 B 自己管理的输出
  命令 release B→external
  submit signal consumed；确认 submit 成功后发送 consumed-submit(n) 通知

退出：
  协商停止提交新帧；处理已经发布但尚未消费的槽
  各自等待本地已提交 GPU 工作；双方确认不再访问共享 payload
  销毁本地 descriptors、buffer、memory、semaphore 等对象
  关闭剩余 OS 句柄；对方异常退出时走专门的超时/错误路径
```

二进制 semaphore 的 signal 在下一次相应 wait 前不能重复使用；此处一来一回的依赖保证上一轮消费完成。IPC 通知只用于确定相关 signal 已经入队及传递序号，不代表 GPU 已经完成。为了看清逻辑，例子从单槽开始；多槽版需为每槽分别维护 ready/consumed 状态。

必须补充的实现包括：检查所有返回值；按具体访问阶段生成 release/acquire barriers；句柄类型的资源兼容限制；非 coherent host 访问；部分创建失败的回滚；安全地处理 IPC 断连。若共享 image，还要增加外部交接 layout、format/tiling/extent 一致性、子资源和 dedicated 约束。

这个 Windows 导入路径要求同一 underlying physical device；它不是双显卡互通示例。详见 [内存导入规范](https://docs.vulkan.org/refpages/latest/refpages/source/VkImportMemoryWin32HandleInfoKHR.html)。
