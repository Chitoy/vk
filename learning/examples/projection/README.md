# 投影与插值：继续使用原工程的三顶点 draw

对应[第 1 章](../../01_graphics_foundations.md)。这是三个完整 shader 文件与两处 C++ 接口调整，使用原工程作为宿主；不是独立可执行程序。本机未编译、未运行。无额外库或资产。

这次故意仍不引入 vertex buffer：先看懂“位置怎样变成像素”，第 3、4 章再解决“位置怎样从 CPU buffer 送进来”。

## 在另一台电脑的项目副本中接入

1. 保留原 `shaders/fullscreen.vert` 和 `shaders/neon.frag` 的副本。
2. 用本目录 `perspective.vert` 的完整内容替换副本工程的 `shaders/fullscreen.vert`。
3. 用本目录 `interpolation.frag` 的完整内容替换副本工程的 `shaders/neon.frag`。
4. 在 `createGraphicsPipeline()` 中，把原 push constant range 的 stageFlags 改成下面的两阶段并集。
5. 在 `recordCommandBuffer()` 的 `vkCmdPushConstants` 中，也把 stageFlags 改成相同并集。
6. 在那台电脑沿用原有构建流程，重新生成两个 `.spv` 并启动新进程。原 CMake 已经处理这两个目标文件名，不必增加新的 shader target。

两处必要代码：

```cpp
// createGraphicsPipeline(): still offset=0, size=sizeof(PushConstants).
pushConstantRange.stageFlags =
    VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;

// recordCommandBuffer(): same 32 bytes as before, now both stages can read them.
vkCmdPushConstants(
    commandBuffer,
    pipelineLayout_,
    VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
    0,
    sizeof(constants),
    &constants);
```

原工程只把参数声明和更新给 fragment stage；新 vertex shader 为宽高比读取了同一块数据，必须同时修改 layout 和写入调用。无需改变 `PushConstants` 的八个 float，无需新增 descriptor，也无需改变 `vkCmdDraw(commandBuffer,3,1,0,0)`。

输出 location 0、1 与本目录 fragment 成对匹配，location 1 两侧均为 `noperspective`。不要只替换片元文件后继续使用没有 location 1 输出的原 fullscreen vertex shader。

## 预期现象与解释

启动后是深色背景上的棋盘三角形。棋盘是 shader 用 `floor/mod` 计算的颜色，不是外部纹理。

`interpolation.frag` 的 `MODE=2` 将屏幕分为两侧：左侧使用默认透视正确 UV，右侧使用屏幕线性 UV，中间有细橙线。三角形顶点处的参数一致，内部格线间距不同，因为顶点的 clip.w 为 `2,2,5`。为了对比同一位置，可将 `MODE` 分别改为 0、1，重编译后观察整个三角形。

换用 `orthographic.vert`，其他接口保持不变：所有 clip.w 都是 1，两种插值一致，除了比较模式的人为分隔线。正交下顶点 Z 仍决定深度，却不影响 X/Y 投影缩放。

原 pipeline 没有 depth test，本例仅画一个三角形，足够观察投影。它不是完整三维场景或深度测试示例。

## 建议单变量实验

| 修改 | 修改前应预测 |
|---|---|
| 透视例第三顶点 z 从 -5 改成 -2 | 所有 w 相等，smooth 与 noperspective 一致；第三顶点的屏幕位置也变化 |
| `60°` 改成 `90°` | 视野更宽，三角形投影变小 |
| 正交例只改变一个顶点 Z，仍在裁剪范围内 | 该顶点 X/Y 屏幕位置不变 |
| 去掉投影中的 Y 负号 | 画面上下翻转；本例无剔除所以仍能看到 |
| nearPlane 改成 3，farPlane 保持 10 | 两个 z=-2 顶点在近裁剪面外，图元被裁剪，不是按顶点越界直接整形丢弃 |
| 调整窗口宽高比 | 读取新的 aspect 后保持投影比例；窗口重建流程仍由原工程处理 |

修改 near/far 时始终保持 `0 < near < far`。修改顶点数量或 `firstVertex` 前，要先改变 shader 数组/索引设计，否则可能越界。

## 调试时看哪里

在另一台电脑捕帧后，先检查 pipeline 两个 shader 的接口，再看 vertex shader 输出的 `gl_Position.w`，然后看 fragment 输入。原始三点在透视例的 w 应为 `2,2,5`；正交例应全为 1。你要验证的是“输入 → 变换 → 插值 → 输出”的因果链，而不仅是截图像不像。

此实验沿用原窗口宿主，因此也继承其 WSI 简化；呈现 semaphore 与 resize 生命周期改进见[第 5 章](../../05_resize_lifetime.md)。
