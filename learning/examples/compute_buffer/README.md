# 完整例：CPU 写数组 → Compute Shader 修改 → CPU 读回

配套教材：[03_memory_resources.md](../../03_memory_resources.md)。这是独立、无窗口的 Vulkan 1.1 项目，不依赖当前窗口程序，也不会修改它。

示例输入是 `uint32_t` 数组 `0, 1, ... 15`。GPU 中每个有效 invocation 计算一个元素：`value = value * 2 + 1`。输出应为：

```text
Output: 1 3 5 7 9 11 13 15 17 19 21 23 25 27 29 31
PASS: all 16 values match value * 2 + 1.
```

这段是**预期输出**；本次仅编写和静态阅读代码，没有配置、编译、运行或安装环境。

## 在另一台电脑上执行

需要你已有的 C++20 编译器、CMake 3.24+、Vulkan SDK（头文件、Loader 链接库、`glslc`）、支持 Vulkan 1.1 和 compute queue 的驱动。程序不需要 GLFW、GLM、VMA、窗口系统扩展或额外设备扩展。发现 `VK_LAYER_KHRONOS_validation` 时自动启用；没有校验层时仍允许执行。这里按 Windows/Linux 原生 Vulkan 驱动设计，未集成 macOS MoltenVK 的 portability enumeration。

PowerShell，工作目录先进入本目录；下列命令留给另一台电脑：

```powershell
cmake -S . -B build
cmake --build build --config Debug
```

Visual Studio 等多配置生成器：

```powershell
.\build\Debug\compute_buffer.exe
.\build\Debug\compute_buffer.exe --prefer-noncoherent
```

Ninja 等单配置生成器生成的程序通常在 `build/compute_buffer.exe`，Linux 则为 `build/compute_buffer`。

默认 shader 路径由 CMake 写入程序，指向构建目录中的 `shaders/transform.comp.spv`，所以从别的工作目录启动也可找到它。若单独移动可执行文件与 SPIR-V，请显式传路径：

```powershell
.\compute_buffer.exe .\transform.comp.spv --prefer-noncoherent
```

`--prefer-noncoherent` 只会优先选择设备真实提供、且与当前 buffer 兼容的非 coherent 类型；若没有，打印提示并退回 coherent。它不会伪造硬件属性。有些设备只能在 coherent 路径上验证本例。

## 对照阅读与断点

| 顺序 | `main.cpp` 中的方法 | 此时应该建立的理解 |
|---|---|---|
| 1 | `createInstance`、`chooseDeviceAndQueue`、`createDevice` | 无需窗口也能使用 Vulkan；compute queue 能运行计算命令 |
| 2 | `createBufferAndMemory` | buffer 定义用途和逻辑长度；allocation 提供存储；bind 连接两者 |
| 3 | `createDescriptors` | set 0 / binding 0 指向 buffer 的前 64 字节 |
| 4 | `createPipeline` | 一个 compute stage、descriptor 接口、4 字节 push constant 组成 pipeline |
| 5 | `writeInput` | CPU memcpy；非 coherent 内存需要 flush；此时尚未 submit |
| 6 | `recordCommands` | bind pipeline / set / push count / dispatch / barrier；只是录制 |
| 7 | `submitAndRead` | submit → fence wait → 必要的 invalidate → memcpy 读回 |
| 8 | 析构函数 | 工作完成后销毁引用者、资源和 allocation；异常路径也清理已创建对象 |

在 `vkQueueSubmit` 前检查 `mapped_` 可看到输入。在 `vkWaitForFences` 返回且必要的 invalidate 完成后，才通过映射指针检查 GPU 输出。不要让调试器在 GPU 正在写时持续展开该映射区；调试器读内存并不会替程序建立 Vulkan 同步。

shader 中 `gl_GlobalInvocationID.x` 范围为 `0..63`；只有 `0..15` 访问数组。`vkCmdDispatch(1, 1, 1)` 的 `1` 是**工作组数量**，不是 invocation 数量。

## 可控练习

1. 将 shader 运算改成 `value * 3u + 7u`，同步修改 CPU 预期结果；重新编译 shader 后观察输出。
2. 将 `kCount` 改为 `100`。dispatch 应变为 2 个工作组；shader 的边界判断不变。极大数组还需要审查设备 limits、大小溢出与内存预算。
3. 录制两次 dispatch，在中间加入 `COMPUTE_SHADER / SHADER_WRITE → COMPUTE_SHADER / SHADER_READ | SHADER_WRITE` 的 buffer barrier，末尾保留 shader → host barrier。每个元素预期为 `4 * i + 3`。同一个 queue 上先录制第一次、再录制第二次，仍不能省掉这段读写依赖。
4. 列出本机 memory types 与 heap，解释“兼容位”“HOST_VISIBLE”“HOST_COHERENT”“DEVICE_LOCAL”分别在回答什么；不要直接把第一块 heap 当成显存。

本例每个元素仅由一个 invocation 读写，没有跨 invocation 通信。日后若要相加、归约或共享计数器，需要另学 workgroup shared memory、`barrier()`、内存可见性和 atomics；不能把这里的无竞争条件照搬过去。

## 同步与范围保证

程序对每个返回 `VkResult` 的正常路径调用检查结果；枚举处理 `VK_INCOMPLETE`；失败会报告具体操作名和结果值。`vkGetPhysicalDevice*Properties`、`vkCmd*` 和销毁函数大多返回 `void`，不存在可检查的结果值。析构中的 `vkDeviceWaitIdle` 报错后不抛异常。

CPU 写入先于提交，非 coherent 时先 flush；提交承担 host → device 域操作。读回使用 `COMPUTE_SHADER / SHADER_WRITE → HOST / HOST_READ` barrier，再等 fence，再 invalidate。coherent 只省掉 flush/invalidate，barrier 和等待照常保留。规则见 [Khronos Host Write 同步章节](https://docs.vulkan.org/spec/latest/chapters/synchronization.html#synchronization-submission-host-writes) 和 [invalidate 的依赖链要求](https://docs.vulkan.org/refpages/latest/refpages/source/vkInvalidateMappedMemoryRanges.html)。

allocation 从偏移 0 开始整体映射，flush/invalidate 使用 `offset=0, size=VK_WHOLE_SIZE`。起点自然按 atom 对齐，终点是整个 allocation 的末尾，符合尾部例外。本例只有一个 buffer、一个 allocation，避免共享同一个非 coherent atom 的相邻资源引入竞争。规则见 [VkMappedMemoryRange](https://docs.vulkan.org/refpages/latest/refpages/source/VkMappedMemoryRange.html)。

这是展示数据路径的教学例：GPU 直接操作 host-visible 内存，便于读写与调试。它不代表大型计算应该总选这种内存，也没有演示 staging copy、纹理、多个提交队列或多 GPU；这些流程在配套教材中分开解释。
