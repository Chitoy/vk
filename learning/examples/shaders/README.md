# 片元 shader 实验：与当前项目的接口保持一致

对应教程：[02_shader_dataflow.md](../../02_shader_dataflow.md)。四个 `.frag` 都是完整片元程序，配合原始 `shaders/fullscreen.vert`，使用当前 C++ 的 32 字节 fragment push constants，不需要新库、图片或 descriptor。

| 文件 | 目的 | 预期画面 |
|---|---|---|
| [01_uv.frag](01_uv.frag) | 看见顶点输出插值和坐标方向 | 红色向右增加、绿色向下增加 |
| [02_ring.frag](02_ring.frag) | 坐标比例、距离场、边缘过渡 | 居中青色圆环，宽窗口中保持圆形 |
| [03_neon_fixed.frag](03_neon_fixed.frag) | 阅读原 neon 并修复数学边界问题 | 六层变色扭曲光环；修复倒序 smoothstep 与 atan 原点 |
| [04_shadertoy_y_up.frag](04_shadertoy_y_up.frag) | 演示 y 向上的适配入口 | 呼吸圆环、向上增强的绿色、橙色鼠标点 |

**验证状态：只做了源码和接口静态审阅，未在本机编译或运行。预期画面不是实际运行截图。原始源码与 CMakeLists.txt 没有因这些例子而修改。**

## 在你另一台电脑上使用

当前 CMake 只编译 `shaders/fullscreen.vert` 与 `shaders/neon.frag`，而 `main.cpp::createGraphicsPipeline()` 固定加载构建目录里的 `shaders/neon.frag.spv`。把例子放在这个学习目录里不会自动改变运行结果。

你可以选择下面任一方法。这些是供另一台电脑执行的说明，本次没有执行这些命令。

**方法 A：沿用你现有的构建流程。** 在另一台电脑先把原 `shaders/neon.frag` 备份为自己选定的文件，再把某个例子全文复制到 `shaders/neon.frag`。随后使用那台电脑已经能成功工作的原有构建流程，再重新启动应用。不需要修改 CMake 或 C++，因为输出名和接口不变。恢复原文件即可结束实验。

**方法 B：只生成实际加载的片元 SPIR-V。** 如果另一台电脑已拥有构建好的可执行文件及 Vulkan SDK，可直接把例子编译到该可执行文件实际使用的 shader 路径。先在该电脑备份原 `neon.frag.spv`；确认应用已关闭，并把下面变量改成真实路径：

```powershell
# 仅在另一台已配置好的电脑上执行；工作目录为包含 src/、shaders/ 的项目目录。
$shaderOutputPath = 'D:/你的实际构建目录/shaders/neon.frag.spv'
glslc --target-env=vulkan1.1 learning/examples/shaders/01_uv.frag -o $shaderOutputPath
```

其他实验替换命令中的 `.frag` 文件名。输出必须叫当前程序实际读取的 `neon.frag.spv`，且目录必须与该可执行文件编译时的 `VULKAN_SHADER_DIR` 一致；不是随便选一个 `shaders` 目录。然后用你原来的方式重启已构建的程序。原程序没有热重载；覆盖 `.spv` 不会自动更新已经创建的 pipeline。再次运行 CMake 构建时，原 shader 构建规则也可能重新生成输出，因此长期实验使用方法 A 更直观。

## 阅读例子的注意项

01 声明了完整 push constant block，但默认不使用其中数值；编译器优化掉未用成员不妨碍应用声明和写入既有 32 字节范围。

02 使用片元导数做近似边缘过渡，且保证 smoothstep 两个边界递增。这不要求打开 MSAA，也不需要扩展。

03 保留原有鼠标位移与旋转矩阵约定，便于逐句对照。所谓 fixed 指修复倒序 smoothstep 和原点角度的未定义结果，不表示高 DPI 鼠标输入或所有输出格式都已在 C++ 中修复。数学修复后不能承诺与原先未定义表达式在所有驱动上逐像素相同。

04 保留 32 字节接口，因此没有鼠标点击状态。鼠标点在窗口逻辑尺寸与 framebuffer 像素尺寸相同时应跟随实际光标；高 DPI 下需要教程给出的 C++ 归一化修改。这个例子展示适配方法，未实现 iChannel 或多 pass。

所有例子按当前首选 sRGB swapchain 路径输出线性 RGB，不手动重复做 gamma 编码。如果程序退回了其他 surface format/colorSpace，先核对实际选中值，再调整最终颜色转换。
