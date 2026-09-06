# 从读懂一帧到能够设计 Vulkan 渲染器

这套材料从你已经理解的 `drawFrame()` 出发，补上“计算什么、数据放在哪里、接口怎样对应、什么时候可以访问与回收”。不要求先学完 OpenGL，也不要求先背完线性代数。

原项目仍在 [`../src/main.cpp`](../src/main.cpp) 和 [`../shaders/`](../shaders/)。这里新增教材与独立示例，没有修改原来的运行代码或构建入口。本次只做源码阅读、规范查证、文件编写和静态检查，没有安装、下载依赖、配置环境、构建、编译或运行项目。示例的实际执行与验证留给你的另一台电脑。

## 你现在的位置

根据你的描述，你已经建立了**任务的时间线**：CPU 录制/提交，GPU 异步处理，WSI 获取/呈现图像，fence 和 semaphore 连接这些任务。下一步需要把每个任务展开成“对哪些字节做什么”。

例如看到 `vkCmdDraw(commandBuffer, 3, 1, 0, 0)`，最终应能解释：

> 它记录一次三顶点绘制。当前 vertex shader 按顶点索引产生 clip 坐标和 UV；固定功能阶段裁剪、除以 w、映射 viewport、光栅化并插值；fragment shader 读取插值 UV 和命令中记录的 32 字节参数，计算颜色；颜色附件输出把结果存到 framebuffer 选中的 swapchain image。哪些阶段能够执行，以及何时能复用这些对象，要由同步关系和生命周期证明。

读完这段仍有陌生词很正常。下面每一章负责把其中一部分拆到可以手算、定位代码和修改示例。

## 阅读顺序与产物

| 顺序 | 教材 | 读完后应该能做到 | 对应实践 |
|---|---|---|---|
| 0 | [认知模型与源码地图](00_mental_model.md) | 分清资源、存储、接口、执行、生命周期 | 用一张表追踪时间参数和颜色 |
| 1 | [图形学：坐标、投影、光栅化](01_graphics_foundations.md) | 手算 clip/NDC/屏幕坐标，解释近大远小和插值 | [投影与插值实验](examples/projection/README.md) |
| 2 | [现有 shader 与数据接口](02_shader_dataflow.md) | 读懂三个原 shader，自己写圆环与颜色效果 | [片元 shader 示例](examples/shaders/README.md) |
| 3 | [内存、Buffer、Image、Descriptor](03_memory_resources.md) | 追踪 CPU 字节如何到 shader 再回 CPU | [完整无窗口 compute 示例](examples/compute_buffer/README.md) |
| 4 | [命令记录与渲染状态](04_commands_pipeline.md) | 逐步解释 `recordCommandBuffer()`，接入顶点与资源 | 本章的顶点接口集成片段 |
| 5 | [窗口变化与资源生命周期](05_resize_lifetime.md) | 从依赖关系推导重建，区分 graphics 与 present 完成 | 呈现 semaphore 修正集成说明 |
| 6 | [线程、队列、进程与设备](06_threads_processes_devices.md) | 明确每次跨边界需要共享什么、同步什么 | [高级架构片段](examples/advanced/README.md) |
| 7 | [练习、调试路径与专家能力](07_practice_and_debug.md) | 用证据解释错误，建立逐级验收标准 | 阅读题、故障实验与长期项目 |

如果第 1 章的三维投影暂时较难，可以先读到二维坐标和向量运算，再读第 2 章的 UV/圆环实验。读懂圆环不需要相机；理解真实三维模型和纹理透视时，再完成投影部分。

## 示例的可用范围

| 示例 | 交付形式 | 如何使用 | 额外依赖 |
|---|---|---|---|
| UV、圆环、修正霓虹 | 完整 `.frag` 文件 | 在另一台电脑的项目副本中替换 `shaders/neon.frag`，沿用当前构建流程 | 无 |
| 透视/正交与插值 | 完整 shader 配对 + 两处 C++ 接口调整 | 按该目录 README 操作，仍由当前项目绘制三个顶点 | 无 |
| SSBO compute 往返 | 独立 `main.cpp`、`.comp`、`CMakeLists.txt` | 用已准备好的 SDK 单独构建；输入与结果在控制台验证 | Vulkan SDK、C++20、CMake；不需 GLFW、GLM、VMA |
| 顶点上传、纹理、secondary、跨进程/设备 | 集成片段或架构伪代码，正文明确标注 | 作为你后续扩展渲染器的设计与接入指南 | 见第 6 章能力矩阵 |

“完整 shader”表示文件包含入口和接口，并不表示它能脱离宿主程序独立显示。“集成片段”需要宿主对象与前置步骤。“架构伪代码”用于解释协议，不是已完成的跨进程播放器。跨设备没有能够保证任意两块显卡互通的通用开关。

## 先修正三处容易固化的理解

1. **Shader 阶段不是 CPU 函数之间的直接调用。** 阶段之间靠声明的接口和固定功能处理连接；UV 会插值，资源通过 descriptor 或 push constant 接入。
2. **同步要解释访问范围和内存可见性。** Fence 证明某次提交完成，不能凭一个“信号到达了”推断任何资源、任何缓存、任何呈现请求都已经可以复用。Semaphore 也能建立内存依赖，不能把它概括成“只负责排队”。
3. **当前 starter 存在教学简化与需修正处。** 原 `neon.frag` 的倒序 `smoothstep` 不应依赖；`renderFinished` 按帧槽复用缺少充分的 present 完成证明；原文第 10 节已经提到后者，新第 5 章展开到具体生命周期。鼠标坐标还涉及窗口逻辑尺寸、framebuffer 像素尺寸和 Y 方向约定。

GLSL 的 `smoothstep` 边界与 Vulkan WSI 的 semaphore 复用规则分别以 [GLSL Common Functions](https://docs.vulkan.org/glsl/latest/chapters/builtinfunctions.html) 和 [Khronos Swapchain Semaphore Reuse](https://docs.vulkan.org/guide/latest/swapchain_semaphore_reuse.html) 为依据，详细推导见对应章节。

## 版本与术语约定

主线匹配本项目的 **Vulkan 1.1、C++ Vulkan C API、GLSL 450、经典 Render Pass、`vkQueueSubmit`/`vkCmdPipelineBarrier`**。后续谈 Timeline Semaphore、Synchronization2、Dynamic Rendering 时会另外说明查询、启用与版本条件，不将新接口直接混入 1.1 示例。

“CPU/host”描述调用者和宿主内存访问；“GPU/device”描述设备执行。图中队列是 Vulkan 的抽象，不能据此推断驱动线程数量、硬件引擎数量或显示器真实扫描时刻。规范规定可观察行为，具体微架构需要另查设备资料并测量。

官方链接在 2026-09-07 编写过程中查证；`latest` 页面会随规范更新。阅读新扩展时，要同时核对你运行机器的实际能力。课程里新增的示例没有要求自动获取任何第三方库。

## 第一轮学习任务

先完成这三件事，不需要在当前电脑运行：

1. 阅读第 0 章，画出“32 字节 push constant → 一个 fragment 的颜色”的路径。
2. 阅读第 2 章，手算 `vUV=(0.5,0.5)` 时 `p` 的值，以及半径 0.2 的圆环在哪些点最亮。
3. 读第 4 章，把 `recordCommandBuffer()` 每一行标成“修改命令状态”“记录参数”“记录工作”或“结束作用域”。

然后在另一台电脑先执行 UV 与圆环实验。判断是否理解的标准是：修改前能预测画面，修改后能说明差异，出错时能指出该检查哪个接口或资源。
