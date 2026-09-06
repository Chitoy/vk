# 06：把同一套模型扩展到线程、进程与多个 GPU

现在不急着给当前三顶点程序增加线程。先沿着一项任务问四个问题：**谁录制，谁执行，数据存在哪里，谁证明它能被下一位使用者访问？** 这四问适用于一个线程，也适用于多个进程和设备。

本章的高级代码放在 [advanced 示例说明](examples/advanced/README.md)。它们明确属于集成片段或架构伪代码，没有在本机编译运行，也没有声称已经实现完整跨进程渲染器。

## 1. 五个独立维度，不要把“多”混为一谈

| 维度 | 它改变什么 | 不会自动带来什么 |
|---|---|---|
| 多个 CPU 线程 | CPU 可以并行准备场景、录制命令 | GPU 工作自动正确排序 |
| 同一 device 的多个 VkQueue | 多条设备提交时间线 | 必然使用不同硬件引擎；必然更快 |
| 不同 queue family | 队列能力与资源所有权边界 | 第二块 GPU |
| 多个进程/多个 VkDevice | 独立对象命名空间、独立 host 地址空间 | 普通 VkImage/VkSemaphore 句柄跨界有效 |
| 多个 physical devices | 不同执行设备及其内存可达性 | 显存互通或显存自动合并 |

原项目中，`createLogicalDevice()` 对 graphics/present family 去重，再各取 queue 0。若来自同一 family，这两个成员实际可以是**同一个 queue 句柄**。当前没有创建第二块 GPU 的逻辑设备，也没有 device group，更没有外部内存导出。

## 2. 多线程：先把 host 的所有权分清

“externally synchronized” 在这里意为：Vulkan 要求**应用负责**有关 host 访问的串行化。它没有说“需要 external semaphore”。互斥锁保护驱动对象的 host 访问；GPU semaphore/barrier 保护设备任务之间的依赖。

最容易实现的布局是：

```text
frame slot 0
  worker 0：command pool、descriptor pool、自己更新的数据区
  worker 1：command pool、descriptor pool、自己更新的数据区
  submit thread：primary command buffer / submit
frame slot 1
  worker 0：另一组 pools / 数据区
  worker 1：另一组 pools / 数据区
```

一帧的 host 顺序是：等该帧 GPU fence → 确认没有 worker 仍在使用旧 pools → reset 本帧 pools → 分派工作 → worker 各录自己的命令 → 等 CPU jobs 完成 → 组合/submit。下一次复用 frame slot 前，再等待真正覆盖这些命令的 GPU 完成信号。

为什么每线程一个 command pool？同一 pool 的 host 使用包含从它分配的 command buffers 的录制；“两个线程录两个不同 command buffer，但它们来自同一 pool”也不能直接并行。每帧一份 pool 则方便整体 reset，不会重置上一帧仍在 GPU 上 pending 的命令。

同样，不能多个线程同时从一个普通 descriptor pool 分配/释放 sets。共享 descriptor set 的修改和与 pending GPU 使用的关系还要单独检查；把“每线程一个 pool”误读为“所有 descriptor 更新都自动安全”也不对。初学先使用每帧独占的可更新数据与 descriptors。[官方多线程录制示例](https://docs.vulkan.org/samples/latest/samples/performance/command_buffer_usage/README.html)。

### Primary 与 secondary 怎么接起来

Primary 能直接提交给 queue。Secondary 通常由 primary 的 `vkCmdExecuteCommands` 执行，便于多个 worker 分别录制场景的一部分。对于本项目的经典 render pass：

1. worker 从其 pool 分配 `VK_COMMAND_BUFFER_LEVEL_SECONDARY`。
2. `VkCommandBufferInheritanceInfo` 填兼容 render pass、subpass 0；可提供本帧 framebuffer，或者按规范使用 null。
3. begin 带 `VK_COMMAND_BUFFER_USAGE_RENDER_PASS_CONTINUE_BIT`，表示该 secondary 会在 primary 的 render pass 内执行。
4. worker 在自己的 secondary 中 bind pipeline、bind 自己需要的 descriptors/vertex buffers、push constants、draw；不在其中重复 begin/end 外层 render pass。
5. primary 以 `VK_SUBPASS_CONTENTS_SECONDARY_COMMAND_BUFFERS` 开始 render pass，执行这些 secondary，再 end render pass。

不要假设 secondary 自动继承 primary 先前绑定的 pipeline、descriptor sets、push constants 或全部动态状态。render pass 继承描述的是执行环境，所需渲染状态应在 secondary 自己设置。原函数使用 `VK_SUBPASS_CONTENTS_INLINE`，不能保持这项配置同时直接套用上述结构。示例见 [advanced/README.md](examples/advanced/README.md#例-a经典-render-pass-中的-secondary-集成片段)。

本例只有一次 draw，多线程录制很可能增加调度成本。先扩展到许多独立物体，再用 CPU profile 判断录制是否瓶颈；线程数、secondary 数量和提交批次数都不是越多越好。

### 同一个 queue 必须共用同一把锁

如果以后 worker 也 submit，正常创建的同一个 `VkQueue` 的相关 host 操作需要外部同步。实际做法是“一份 queue 对象封装持有一把 mutex”，所有别名通过这份封装提交。不能 `graphicsMutex` 和 `presentMutex` 各锁各的，而它们底下其实指向同一个 queue。

当前程序只由主线程提交，已经自然串行。新增多线程时让 worker 只录制、专门提交线程负责所有 queue 操作，通常更容易保证正确性。调用 device/queue idle、销毁设备时还要阻止其他线程继续提交。[普通 present queue 的 host 同步要求](https://docs.vulkan.org/refpages/latest/refpages/source/vkQueuePresentKHR.html)。

`std::thread::join()` 仅证明 CPU 线程结束；如果它只是调用完 `vkQueueSubmit()` 就退出，GPU 很可能仍未结束。C++ mutex 也不会替你做设备内 cache 可见性操作。

## 3. 多队列：共享资源时，画出生产者与消费者

假设 compute shader 写 storage buffer B，随后 vertex shader 读取 B。先写出：

```text
生产者：COMPUTE_SHADER / SHADER_WRITE / B[指定字节范围]
消费者：VERTEX_SHADER / SHADER_READ / 同一个范围
```

如果实际把 B 当顶点属性读取，消费者是 `VERTEX_INPUT / VERTEX_ATTRIBUTE_READ`，不是 `VERTEX_SHADER / SHADER_READ`。同一份字节用什么路径进入 GPU，决定 stage/access 应该怎样写。

| 执行位置 | 通常怎样连接 | 还要关注什么 |
|---|---|---|
| 同一个 queue | 合适的 pipeline barrier / render pass dependency | CPU 调用顺序不等于充分的执行和内存依赖 |
| 不同 queue、同一个 family | 生产 submit signal semaphore；消费 submit wait semaphore | wait stage 应覆盖最早消费者；image 若需换 layout 还要处理 |
| 不同 family、资源为 EXCLUSIVE | semaphore 跨队列，加成对 release/acquire ownership barriers | 两边必须对应同一资源范围、队列族与布局转换 |
| 不同 family、资源为 CONCURRENT | 在创建时列出这些 families；仍用 semaphore 等依赖 | concurrent 省去这些 family 间的 ownership transfer，不消除读写冲突 |

Semaphore 信号/等待本身可以建立设备内存依赖，不应一律理解成“只有排序、完全不提供可见性”。对于无需 layout/ownership 改变的跨队列 buffer 传递，正确范围的 semaphore 依赖可以足够；barrier 用来进一步处理范围、图像布局、所有权等要求。反过来，只在队列 A 录一条普通 barrier，不能凭空让独立的队列 B 等待 A。官方有不同场景的 [Synchronization Examples](https://docs.vulkan.org/guide/latest/synchronization_examples.html)。

不同 family 的典型 EXCLUSIVE 图像流程是：

```text
queue A：写 image → release A→B（记录所需布局转换） → signal ready
queue B：wait ready → acquire A→B（匹配的转换信息） → 读 image
```

release/acquire 的访问范围和阶段参数不是简单复制相同数值就总是正确；传统同步与 Synchronization2 在写法上也有差异。先对照一份指定生产/消费用途的例子；不要从未知用途的 barrier 模板机械填空。本项目使用传统 `vkQueueSubmit` 和 render pass，不需要为了学会这件事立刻改为 Vulkan 1.3。

## 4. Timeline semaphore：用进度值表达完成条件

Binary semaphore 适合一次 signal 对应一次消费。Timeline semaphore 保存单调递增的 64 位值，等待 `>= N`，不会因某次 wait 而归零。它能被 host 查询、等待、signal，也能被 queue 等待和 signal。

例如给上传任务使用一个 timeline：`upload=17` 表示第 17 批上传已经结束。许多 draw 都可等这个值。若 graphics 读完又需要让上传任务覆盖同一个槽，仍需反向的“读取完成”里程碑；“可以读”不等于“可以覆盖”。

使用前必须完成：

1. 选择有效的 Vulkan 1.2+ core 路径，或 Vulkan 1.1 + `VK_KHR_timeline_semaphore` 扩展路径。
2. 通过 `vkGetPhysicalDeviceFeatures2` 查询 `VkPhysicalDeviceTimelineSemaphoreFeatures::timelineSemaphore`。
3. device 创建时显式启用该 feature；走 1.1 扩展路径则列入 device extensions，并获取 KHR 函数入口。
4. 创建 semaphore 时通过 `VkSemaphoreTypeCreateInfo` 选 `VK_SEMAPHORE_TYPE_TIMELINE` 与初值。
5. 本项目若继续使用 `VkSubmitInfo`，用 `VkTimelineSemaphoreSubmitInfo` 在 pNext 中指定 wait/signal 值。

当前 `apiVersion = VK_API_VERSION_1_1`，device extensions 只有 swapchain，不能直接粘贴 core `vkWaitSemaphores` 就假设可用。也不能只检测 API 版本，省掉 feature 的查询与启用。[Timeline feature 定义](https://docs.vulkan.org/refpages/latest/refpages/source/VkPhysicalDeviceTimelineSemaphoreFeatures.html)、[扩展及 core 提升说明](https://docs.vulkan.org/refpages/latest/refpages/source/VK_KHR_timeline_semaphore.html)。

当一个 submit 同时等待 acquire binary semaphore 和某个 timeline semaphore，`VkTimelineSemaphoreSubmitInfo` 的值数组数量/顺序要与 submit semaphore 数组对应，binary 对应的值被忽略，不是只填 timeline 的那几项。WSI 的 acquire/present semaphore 仍要求 binary；不能拿 timeline 直接替换本项目的两类 WSI 信号。[官方 Timeline Semaphore 示例](https://docs.vulkan.org/samples/latest/samples/extensions/timeline_semaphore/README.html)。

不要认为 timeline 会解决环形依赖：A 等 B=8、B 等 A=9，而没人能先 signal，仍然死锁。跨多个 queue 发出的 signal 也要安排合法的递增顺序；并发 CPU 分配了递增数值，不代表 GPU 完成顺序自然递增。

## 5. 跨进程：同时共享“存储”和“同步协议”

先限定一个可以说清的目标：**两个 Windows 进程使用同一块 physical GPU；A 写 RGBA 数据，B 用它生成或显示图像。** 这已经需要比多线程多出一层协议，暂时不要同时增加多 GPU。

普通 `VkImage`、`VkBuffer`、`VkDeviceMemory`、`VkSemaphore` 的数值不能当作 IPC 数据直接在 B 中使用。双方各自创建 instance/device、本地资源与本地 descriptor。共享的是导出对象的底层 payload，通过匹配的外部句柄导入，形成另一套本地 Vulkan 对象。

```text
进程 A                          操作系统 IPC                       进程 B
本地 buffer/image ──绑定── memoryA ─导出/复制 HANDLE─→ 导入 memoryB ──绑定──本地 buffer/image
本地 semaphore readyA            ─导出/复制 HANDLE─→ readyB
本地 semaphore consumedA         ─导出/复制 HANDLE─→ consumedB
descriptorA 引用本地对象                                       descriptorB 引用本地对象
```

选择同一 GPU 不能只靠“枚举序号都是 0”。交换 `VkPhysicalDeviceIDProperties` 中的 `deviceUUID`，按句柄类型要求核验 `driverUUID` 等兼容性信息；Windows 还可通过有效的 LUID/节点信息与 DXGI 适配器对应。[设备 ID 的用途和限制](https://docs.vulkan.org/refpages/latest/refpages/source/VkPhysicalDeviceIDProperties.html)。

### 需要先查询什么

| 项目 | 查询/声明位置 | 应验证什么 |
|---|---|---|
| 外部 buffer | `vkGetPhysicalDeviceExternalBufferProperties` | 同一 flags/usage/handleType 是否 importable/exportable，是否要求 dedicated allocation |
| 外部 image | `vkGetPhysicalDeviceImageFormatProperties2` 链 `VkPhysicalDeviceExternalImageFormatInfo`，输出 `VkExternalImageFormatProperties` | 对这组 format/type/tiling/usage/flags/handleType 的支持，不能只说“GPU 支持 external memory” |
| 外部 semaphore | `vkGetPhysicalDeviceExternalSemaphoreProperties` | 选定类型是否 importable/exportable；若用 timeline，其类型也参与查询 |
| 资源创建 | `VkExternalMemoryBufferCreateInfo` / `VkExternalMemoryImageCreateInfo` | 在创建资源时声明外部内存句柄类型 |
| 内存分配 | export 或 import 对应的 pNext 结构 | size、memory type、dedicated 约束及导入兼容性 |
| 导出同步对象 | `VkExportSemaphoreCreateInfo` | 外部句柄类型与双方 semaphore 类型匹配 |

相关规范入口：[外部 buffer 查询参数](https://docs.vulkan.org/refpages/latest/refpages/source/VkPhysicalDeviceExternalBufferInfo.html)、[外部 semaphore 查询](https://docs.vulkan.org/refpages/latest/refpages/source/vkGetPhysicalDeviceExternalSemaphoreProperties.html)。开始时用单独分配的一只 buffer 和固定字节格式，会比直接共享复杂 optimal-tiled image 更容易核查协议。

### Windows HANDLE 不是任意进程通用的整数

典型设备扩展为 `VK_KHR_external_memory_win32`、`VK_KHR_external_semaphore_win32`。Vulkan 1.1 提供基础 external-memory/semaphore 能力，但平台导入导出扩展仍须枚举、启用，并获取相应函数入口。包含平台声明时配置 `VK_USE_PLATFORM_WIN32_KHR` 及 Windows/Vulkan 头文件；这是另一台电脑的编译配置要求，本次不安装任何依赖。

以 `OPAQUE_WIN32` NT handle 为例：A 用 `vkGetMemoryWin32HandleKHR` / `vkGetSemaphoreWin32HandleKHR` 导出，再通过 `DuplicateHandle` 得到**目标进程中有效**的句柄值，并用命名管道等 IPC 告诉 B。直接把 A 的原始 HANDLE 数字发送给 B，并不等于共享成功。[Microsoft DuplicateHandle](https://learn.microsoft.com/en-us/windows/win32/api/handleapi/nf-handleapi-duplicatehandle)。

B 导入 NT handle 后，Vulkan 不替应用取得 HANDLE 的关闭责任；双方按协议关闭各自不再需要的本地 HANDLE。关闭 NT handle 与销毁 `VkDeviceMemory` 是两个操作，导入对象持有其 payload 引用。`OPAQUE_WIN32_KMT` 等非 NT 类型的引用和寿命规则不同，不能照搬 `CloseHandle` 和持有关系。[内存导入与 HANDLE 寿命](https://docs.vulkan.org/refpages/latest/refpages/source/VkImportMemoryWin32HandleInfoKHR.html)、[semaphore 导入与关闭责任](https://docs.vulkan.org/refpages/latest/refpages/source/vkImportSemaphoreWin32HandleKHR.html)。

### 一个槽需要双向握手

只实现 `A 写 → ready → B 读`，A 可能在 B 读完前覆盖下一帧，因此不够。最简单的单槽协议：

```text
初始槽由 A 使用
A：写第 n 帧 → release 给外部 → signal ready[n]
B：wait ready[n] → acquire → 读第 n 帧 → release 给外部 → signal consumed[n]
A：wait consumed[n] → acquire → 才能写第 n+1 帧
```

两只 binary semaphore 可以轮流使用；对于 binary wait，其 signal 及依赖必须按规范先提交，因此协议还要让对方知道对应 signal submit 已成功排入。IPC 的“readySubmitted”通知表示**已经排队**，GPU 的 semaphore wait 表示**设备工作完成到可继续的位置**，两者职责不同。如果选择可导出的 timeline，也先核验其外部句柄能力，不要从本地 timeline 支持推断外部 timeline 支持。

跨本地 device/外部实体的资源交接应按句柄类型的要求处理 `VK_QUEUE_FAMILY_EXTERNAL` 等 ownership 边界；图像还要约定 layout 与相匹配的 release/acquire。导入方若想保留生产方数据，不能用“第一次见到这只本地 VkImage，所以从 UNDEFINED 开始并丢弃内容”的思路。建议在协议里固定外部交接 layout，并记录尺寸、format、usage、每帧序号、槽号、offset/stride。

再扩展到两槽/三槽时，每个槽都有自己的状态：空闲 → A 正写 → B 可读 → B 正读 → 空闲。协议还必须定义对方退出、导入失败、设备丢失与超时后的处理。单个进程里一直等 `UINT64_MAX` 的习惯，搬到不可信的对方寿命上很容易造成永久挂起。

## 6. 多 GPU：先证明路径存在，再设计资源流

**“另一进程”与“另一块显卡”是两件事。** 尤其不能把刚才的 Windows opaque handle 教程说成任意显卡互通：`VkImportMemoryWin32HandleInfoKHR` 的 `handle-00659` 要求被导出的内存在同一 underlying physical device 创建。[该导入限制](https://docs.vulkan.org/refpages/latest/refpages/source/VkImportMemoryWin32HandleInfoKHR.html)。

### 路径 A：驱动枚举出的 device group

`vkEnumeratePhysicalDeviceGroups` 会报告哪些设备可以组成组。应用把其中允许的设备集经 `VkDeviceGroupDeviceCreateInfo` 链到 device 创建，得到一个代表组的逻辑设备。不能把独显、核显任意拼入数组，就要求驱动建立互联。

之后需要分别处理：内存在哪个 device 分配，绑定如何对应，命令在什么 device mask 上执行，跨设备同步如何指定，最终哪一块设备/哪一种 group present 模式可以连接 surface。`subsetAllocation` 还决定可否只在组的子集分配内存。[组属性](https://docs.vulkan.org/refpages/latest/refpages/source/VkPhysicalDeviceGroupProperties.html)。

对每组 `heapIndex + localDeviceIndex + remoteDeviceIndex`，调用 `vkGetDeviceGroupPeerMemoryFeatures`：

| 标志 | 应如何理解 |
|---|---|
| `COPY_SRC` | 本地设备可以把远端内存作为复制源 |
| `COPY_DST` | 本地设备可以把远端内存作为复制目标 |
| `GENERIC_SRC` | 支持比复制源更一般的远端读取 |
| `GENERIC_DST` | 支持比复制目标更一般的远端写入 |

能 copy 不代表能任意 shader 读取。方向 A→B 和 B→A、heap 0 和 heap 1 都可能有不同能力；存在 device group 也不意味着显存自动合并成无差别的一大块。[Peer memory 查询](https://docs.vulkan.org/refpages/latest/refpages/source/vkGetDeviceGroupPeerMemoryFeatures.html)。

Device group 属于 Vulkan 1.1 的一部分，但本项目目前的 `createLogicalDevice()` 没有组创建信息。升级版本号和实际采用组结构是两回事。

### 路径 B：明确支持的特定外部互操作路径

某些 API、平台、句柄类型或厂商扩展可以提供专门的共享/传输方式。必须按其文档证明设备身份限制、handleType、资源格式、读写能力和同步兼容性全部满足，才采用它。这里不提供一个虚构的“跨任意两块 GPU export/import”通用代码。一般 external memory 的存在，不是这条路径存在的证据。

### 路径 C：通过 host staging 搬运

当缺乏直接互通能力，先实现这条明确的后备路径：

```text
GPU A 输出 image
  → A 上的图像布局转换 / copy image to readback buffer
  → TRANSFER_WRITE → HOST_READ 的设备内存依赖
  → 等 A 的 copy submit fence
  → 非 coherent 内存 invalidate 后 CPU 读取
  → memcpy 到 B 的 host-visible upload buffer
  → 非 coherent 内存 flush
  → submit B 的 copy，并建立 TRANSFER_WRITE → 目标读取 的依赖
  → GPU B 使用新数据
```

这里 fence 证明执行完成，invalidate/flush 处理非 coherent host cache 域，barrier 描述 GPU/host 访问和后续设备访问关系；三者不是可互换的按钮。对非 coherent 范围遵守 `nonCoherentAtomSize` 对齐。若改为跨进程 host shared memory，另加 IPC 的发布/消费同步和元数据协议；两块 GPU 不会因为 CPU 指针相同而自动使用同一个 Vulkan allocation。

与显存直连相比，这条路径可能占用 CPU 带宽和互连带宽，并增加延迟。因此先从低分辨率、固定格式开始验证内容，再比较瓶颈。若图片有行间填充，应按实际 copy 布局/stride 解释，不能假定任意图像映射后都紧密连续。

## 7. 能力与依赖矩阵：在另一台电脑上检查即可

| 练习 | 当前项目是否已有基础 | 另一台电脑需要的能力/依赖 | 不支持时怎么办 |
|---|---|---|---|
| 多线程录制 secondary | Vulkan 1.1、经典 render pass 已够 | C++ 线程库；每线程 pool 的应用实现 | 保持单线程 |
| 多 queue 提交 | 目前只取 graphics/present queue | 物理设备的 queueCount/family 能力；正确同步 | 同 queue + barrier |
| Timeline | 尚未启用 | Vulkan 1.2+ feature 或 1.1 + KHR 扩展 | binary + fence |
| Synchronization2 | 尚未启用 | Vulkan 1.3+ feature 或 KHR 扩展 | 当前传统同步 API |
| Present 资源回收 fence | 尚未启用 | KHR/EXT swapchain maintenance1、对应 surface 依赖和 feature | 按官方 sample 跟踪未扩展 WSI 的回收；理解 shutdown 局限 |
| Windows 跨进程同 GPU | 尚未实现 | Win32 external-memory/semaphore 扩展、兼容 GPU/driver、Windows SDK、IPC 协议 | CPU 共享内存/拷贝协议 |
| Device group | 1.1 有 API，当前未创建组 | 驱动报告的多设备组、peer memory、group present 能力 | 分别创建 device，经 host 传输 |
| 任意两块 GPU 经 host 复制 | 需新增 buffer/image copy | 两边 Vulkan 设备、可用 host-visible 内存 | 降分辨率/频率，或只用一块 GPU |

不需要因为学习这些章节就安装 CUDA、OpenCL 或第三方共享库。若选用某个特定互操作 sample，才按照该 sample 的依赖说明另行准备；本次仅给链接。

## 8. 学习顺序与可观察结果

1. **同一 queue：compute 写 buffer → graphics 读 buffer。** 先写出正确 stage/access，再说明去掉依赖会产生哪类读写竞争。
2. **相同工作移动到另一个 queue。** 对比同 family 与不同 family；能解释 semaphore 和 ownership 各解决什么。
3. **许多 draw 分给两个 CPU worker 录制。** 保持 GPU 数据相同，验收 CPU 线程/pool 不冲突，视觉结果相同；用 profile 判定有没有收益。
4. **换一条任务链为 timeline。** 日志记录“谁 signal 哪个值、谁等待哪个值”，证明无环且数值有实际含义。
5. **两个进程先共享单槽 buffer。** A 写已知序列，B 读回校验；先验证 ready/consumed 双向协议，再用作图像。
6. **最后探索多 GPU。** 先输出 capability report，再选择真实可用的路径；不把设备列表当互通证明。

完整官方工程参考：[多线程 command buffer sample](https://docs.vulkan.org/samples/latest/samples/performance/command_buffer_usage/README.html)、[timeline sample](https://docs.vulkan.org/samples/latest/samples/extensions/timeline_semaphore/README.html)、[swapchain recreation sample](https://docs.vulkan.org/samples/latest/samples/api/swapchain_recreation/README.html)。这些链接是供另一台电脑阅读/运行的完整工程入口，本次没有下载或执行。

完成本章的标准不是记住所有扩展名，而是看到一条新数据路径时，能写出：本地对象与共享 payload 的关系、谁拥有数据、谁能访问、每个读写之间的依赖、最后使用者以及资源回收依据。
