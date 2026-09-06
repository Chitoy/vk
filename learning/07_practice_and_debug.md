# 07 · 把“读懂了”变成可验证的开发能力

这一章提供阅读题、另一台电脑上的实验，以及从当前 starter 走向渲染器开发的验收路线。这里没有执行项目、性能测量或 GPU 验证；下面的观察结果是根据源码和接口推导的预期，实际结果需要你在目标设备记录。

## 1. 每个实验固定留下四项记录

```text
我的预测：修改后哪些数值/像素/依赖会变化？
实际证据：源码位置、Validation 信息、捕帧数据、CPU/GPU 时间。
因果解释：具体哪个输入通过什么接口影响了哪个输出？
修复与反例：为什么修改能解决，什么时候这个方法不适用？
```

不要以“加了 waitIdle 就不闪了”结束分析。它是缩小了并发范围的证据，尚未定位是写坏 UBO、提前销毁 image，还是漏了 GPU 内存依赖。

## 2. 第一轮：只改变一个因素

| 实验 | 对应材料 | 修改前要预测 | 实际验收 |
|---|---|---|---|
| UV 颜色图 | [shader 示例](examples/shaders/README.md) | 左/右/上/下各是什么颜色 | 能从 vUV 推出 RG 分量 |
| 圆环半径/宽度 | 第 2 章 | 哪些点最亮，半径与 glow 各改变什么 | 能手算 `length(p)` 与距离 |
| 去掉 aspect 修正 | 第 1、2 章 | 非正方形窗口中圆变形 | 用每像素坐标增量解释 |
| 固定 time | 原 `recordCommandBuffer` | shader 不读历史状态，画面停止相应动画 | 区分 CPU 录制时刻与 GPU 执行时刻 |
| 正交/透视 | [投影实验](examples/projection/README.md) | 改变 Z 是否影响屏幕 X/Y | 记录三顶点 clip.w 与屏幕位置 |
| smooth/noperspective | 投影实验 | w 相同则一致，不同可能不同 | 能手算一个边上插值值 |
| CPU→compute→CPU | [compute 示例](examples/compute_buffer/README.md) | `0..15` 变为 `1,3,..31` | 能追踪每个 buffer/descriptor/barrier/fence |
| 非 coherent 内存 | compute README 的可选模式 | 若存在合适类型，需要 flush/invalidate | 记录实际 memory flags；若没有，不伪造“已覆盖” |

改变算法输入与故意破坏 Vulkan 有效使用要分开。前一类适合初学；后一类只在可丢弃的实验副本中使用验证工具观察，不把未定义行为是否“恰好能跑”当作规范结论。

## 3. 十二道阅读题与答案

### 题 1：为什么只有三次顶点数据生成，却能覆盖 1280×720？

三个顶点定义一个大三角形。裁剪与光栅化根据几何覆盖产生许多片元位置，插值器提供每个位置的输入，fragment shader 计算颜色。CPU 没为每个像素调用一次 `vkCmdDraw`。

### 题 2：两个 shader 都写了 `location=0`，是否读同一块内存？

不一定。一个阶段的输出与下个阶段输入按接口匹配，期间可有插值；fragment 输出 location 0 又属于颜色输出接口。location 不是共享内存地址，更不是 descriptor binding。

### 题 3：C++ 将 `float width` 改成 `uint32_t width`，shader 保持 float，都是四字节是否可用？

字节数一致不保证解释一致。整数 1280 的位模式被当作浮点读取不会得到浮点 1280。修改要同时匹配类型语义与布局，或在 CPU 侧显式转换到 shader 所需 float。

### 题 4：HOST_COHERENT UBO 能否在 GPU 读取时任意改写？

不能。Coherent 处理特定 host/device 缓存维护需求，不保护并发读写。先等覆盖该范围使用的完成证据，或使用另一份帧槽/ring 范围；然后写新数据并按提交规则让后续设备访问看到它。

### 题 5：`vkUpdateDescriptorSets` 后，纹理是否已上传、已完成布局转换？

都不能据此推断。它更新资源描述，不复制纹理像素，不执行 image layout transition。image 内容上传与访问依赖要另外完成，descriptor 记录的 imageLayout 必须与访问时的实际状态相符。

### 题 6：为什么 buffer 用 `offset`，shader 又有 `offset`，descriptor 也有 `offset`？

它们是不同层的偏移：资源绑定到 allocation 的位置；descriptor 在 buffer 内选择的起点；block 字段在所选范围中的布局；vertex binding 和 attribute 又有自己的偏移。排查时写成“allocation + bindOffset + descriptorOffset + fieldOffset”，并分别检查各层对齐规则。

### 题 7：Fence 已 signaled，CPU 读回 non-coherent 内存还缺什么？

先确认 GPU 写入已通过恰当设备→host 内存依赖到达 host 域，再在 CPU 读取前 invalidate 对应映射范围。Fence 提供完成证明，invalidate 处理 host 侧可见数据；本课程 compute 示例包含完整链。

### 题 8：一个 queue 上 transfer 后紧跟 fragment sampling，还需要同步吗？

提交顺序不等于完整的写后读内存依赖。应明确 transfer write → fragment shader read 的阶段/访问范围，图像还要转换布局。若跨 queue，则还需信号/等待；若跨 family 且 exclusive，另需 ownership 处理。[Khronos Synchronization Examples](https://docs.vulkan.org/guide/latest/synchronization_examples.html)

### 题 9：CPU 两个 worker 已 join，GPU 是否已完成它们的 draw？

join 只证明 CPU 工作结束。如果 worker 只是记录命令，GPU 可能还没收到提交；即使已经提交，仍要等设备完成证明。CPU mutex、join 与 Vulkan 设备同步分工不同。

### 题 10：为什么 resize 不是 destroy 所有对象再全部 init？

尺寸变化只使依赖旧 swapchain/extent/format 的那部分对象需要更新。Instance、device、静态网格和材质通常仍可用。先证明旧工作使用结束，再按依赖重建；present 相关资源还有独立的退休条件。

### 题 11：graphics fence 为什么不能自动保护 renderFinished 的下一次 signal？

它证明该 graphics submit 已完成，未直接证明 present 已消费该二值 semaphore 的 signal。需要按 image 复用并使用 acquire 依赖，或按支持的呈现维护机制跟踪退休。[Swapchain Semaphore Reuse](https://docs.vulkan.org/guide/latest/swapchain_semaphore_reuse.html)

### 题 12：同一台电脑两个进程都看到 GPU，发送 VkImage 数字为什么不行？

普通 handle 不是跨进程资源协议。需要双方资源对象、兼容 external memory 导入导出、真实的 OS handle 传递、元数据，以及双向生产/消费同步。不同物理 GPU 还须另外证明跨设备路径受支持，不能直接套用同 GPU 的 OPAQUE handle 方案。

## 4. 遇到错误时，按数据路径排查

### 完全黑屏、只剩清屏色

1. 捕帧中是否存在目标 draw？没有就查 host 分支、acquire/submit 返回值和命令记录。
2. Render target 是否是正确的 `imageIndex` 对应图像？renderArea、viewport、scissor 是否覆盖可见区域？
3. Vertex 输出是否有限、w 是否合理、图元是否被裁剪或剔除？
4. Fragment 是否执行？输入接口和资源是否匹配？先临时输出固定红色缩小范围。
5. 颜色写 mask、blend、depth/stencil 是否阻止结果？最终展示的是不是这次渲染的图？

### 画面正确形状但颜色不对

检查颜色值的空间与变换：shader 输出、tone mapping、attachment 的 SRGB/UNORM format、纹理是否按正确 sRGB 语义读取、blend 是否在线性空间。宽高比错误一般查坐标而非同步。

### 偶发闪烁、几帧才出现一次

优先查可变资源是否按帧槽分离、buffer ring 范围是否过早复用、descriptor 是否在使用中更新、upload/barrier 是否缺失、present semaphore 是否安全复用。固定时间参数有助于把动画变化和数据 race 分开。

### resize 或最小化后死锁

画出所有 fence 的 reset→submit→signal 路径，检查 acquire 错误提前 return 是否留下无提交会 signal 的 fence；检查零尺寸等待是否允许关闭窗口；检查旧对象引用和 present 资源退休协议。不要把 `SUBOPTIMAL` 当成没有成功取得图像的同义词。

### 同一 shader 在另一机器不同

先查未定义数值运算、越界、未初始化输入、布局与同步，再查 feature/format/limit 查询和启用。像原 `smoothstep(edge0>edge1)` 就属于不应依赖的语言边界行为。不能直接归因于“某厂商驱动有 bug”。

## 5. 工具要回答明确的问题

| 工具/证据 | 用来回答 | 不能据此保证 |
|---|---|---|
| Validation Layer / VUID | 哪条 Vulkan 有效使用条件被违反 | 你的光照、数学和所有 race 都正确 |
| Synchronization Validation | 哪些已追踪的 GPU 访问存在危险依赖 | 捕获所有平台/外部同步/应用协议问题 |
| GPU-Assisted Validation | 某些只能在设备执行时发现的访问问题 | 与正常模式性能一致 |
| RenderDoc 捕帧 | draw 输入、资源、pipeline、附件输出是否符合预期 | 所有真实多帧时序保持不变 |
| CPU 时间测量 | API 调用、命令生成、等待开销 | GPU shader 的独立执行时长 |
| GPU timestamp/counter | GPU 时间区间及部分硬件瓶颈证据 | 跨队列重叠时间可以简单相加 |

验证功能启用方式与支持范围看 [Vulkan Validation Layers](https://github.com/KhronosGroup/Vulkan-ValidationLayers) 和 [Synchronization Validation](https://github.com/KhronosGroup/Vulkan-ValidationLayers/blob/main/docs/syncval_usage.md)。实际捕帧/调试能力还依赖 shader 调试信息、GPU 与工具支持。不要把“没有验证消息”写成形式化正确性证明。

GPU timestamp 要查询 queue family 的 `timestampValidBits`、设备时间戳属性及支持条件，用 `timestampPeriod` 换算纳秒，并在结果可用后读取。不能把 `vkQueueSubmit` 的 CPU 耗时当作 GPU 一帧耗时。[Queries](https://docs.vulkan.org/spec/latest/chapters/queries.html)

## 6. 从当前工程到专家能力：按验收推进

没有一份文档能代替长期设备调试与项目取舍。这里把目标拆为可以完成、评审和积累的产物，不承诺固定几周变成专家。

| 阶段 | 你要做出的产物 | 必须解释的核心问题 | 通过标准 |
|---|---|---|---|
| A：读懂当前渲染 | UV/圆环/投影实验记录 | 数值、接口、像素、提交怎样接通 | 不看原注释也能讲清一帧和一个像素 |
| B：控制资源 | 顶点/索引/UBO/纹理/compute 往返 | 字节布局、上传、descriptor、可见性 | 每个资源有创建/使用/回收记录，实际验证无已知错误 |
| C：独立三维渲染 | 带相机、纹理、深度、基本光照的场景 | MVP、法线、sRGB、采样、遮挡 | 可控制相机并用捕帧证明各 pass 正确 |
| D：多 pass 与多帧 | 离屏颜色 + 后处理 + 帧槽资源系统 | 每个读写危险、layout、资源退休 | 常规帧不靠 deviceWaitIdle，连续 resize/最小化可解释 |
| E：并行与架构 | worker 录制、上传队列、任务图 | CPU 互斥与 GPU 同步、ownership、缓存 | 同步评审通过，有测量证明并行带来收益 |
| F：跨平台与诊断 | 多厂商/不同内存架构的兼容报告 | feature/format/limit、设备丢失、平台生命周期 | 有降级策略、复现材料与回归矩阵 |
| G：高级专项 | external IPC、device group 或可靠 fallback | handle 权属、双向协议、兼容性、失败恢复 | 在明确硬件条件下闭环验证，失败时不静默读坏数据 |
| H：专家实践 | 能评审和演进真实渲染后端 | 规范、硬件与产品约束的取舍 | 可复现地解决正确性/性能/维护性问题并给出证据 |

学习高级同步时不必等待第 F 阶段才读第 6 章，但落地跨设备前应先独立完成单设备的上传、同步与回收。

## 7. 一个适合你的小型渲染器毕业项目

逐项添加，不一次性大改：

1. 在当前工程中画一个真实 vertex/index buffer 的彩色矩形。
2. 用每帧 UBO 控制 model/view/projection，增加深度缓冲，显示立方体。
3. 加入棋盘纹理，区分纹理 sRGB 与线性法线/数据纹理；做 sampler 过滤实验。
4. 引入离屏颜色目标，再用当前全屏三角形做后处理。现在你会看到最初的 neon 框架在渲染器中的位置。
5. 为每个资源记录 usage、format、内存、当前访问、最后使用完成点；设计延迟回收队列。
6. 把 draw 列表拆到多个 worker，比较命令生成与总帧时间。工作太少时，多线程可能更慢，需记录这个结果。
7. 在支持的设备上引入 timeline/synchronization2，再比较它们是否让依赖表达更清晰。升级 API 前先实现查询与启用。
8. 最后单独做“进程 A 生成图像、进程 B 采样展示”，只对声明支持的同 GPU external handle 路径负责；跨 GPU先实现 host staging fallback。

专家式的产物应包含：源码、资源依赖图、能力检查表、同步证明、捕帧、测量方法、已知限制和回归步骤。

## 8. 查官方资料的方式

遇到新 API，按“我想证明什么”打开对应材料：

| 问题 | 主资料 |
|---|---|
| 为什么有这个概念 | [Vulkan Guide](https://docs.vulkan.org/guide/latest/) |
| 一种特性怎样组织成例子 | [Khronos Vulkan Samples](https://docs.vulkan.org/samples/latest/README.html) |
| 函数具体前置条件与 host synchronization | [Vulkan Reference Pages](https://docs.vulkan.org/refpages/latest/refpages/index.html) |
| VUID、内存模型、stage/access 的精确定义 | [Vulkan Specification](https://docs.vulkan.org/spec/latest/) |
| GLSL 的类型、构造器、内建函数、语言边界 | [GLSL Specification](https://docs.vulkan.org/glsl/latest/) |

官方示例也有版本和能力前提。当前课程保持 Vulkan 1.1 C API 主线；不要把使用新版本/扩展的示例只复制函数名就塞进当前 `VkDevice`。

下一次你实际调试时，最有价值的反馈不是“这里没懂”，而是“我预测这段数据在 location 0 应为多少，捕帧读到多少；这是 shader 与 pipeline 接口，以及对应的 resource/fence 状态”。你会逐渐能独立缩小问题范围。
