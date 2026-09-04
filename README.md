# Vulkan Shader Starter

一个面向“有 OpenGL 基础、第一次学 Vulkan”的最小桌面工程：

- Vulkan 1.1 基线，方便以后迁移 Android。
- GLFW 只负责桌面窗口和 `VkSurfaceKHR`。
- 没有顶点缓冲；顶点着色器用 `gl_VertexIndex` 生成全屏三角形。
- 片元着色器绘制动态霓虹效果，鼠标可轻微扰动图案。
- 包含验证层、Swapchain 重建、双帧并行、Fence/Semaphore。
- 附带 ShaderToy 单 Pass 适配模板。

这个项目故意不用引擎或大型封装库。第一次调试时，你能直接把每个 Vulkan 对象与一帧渲染流程对应起来。

## 1. Windows 环境

安装：

1. Visual Studio 2022，勾选“使用 C++ 的桌面开发”和 CMake 工具。
2. 最新显卡驱动。
3. [LunarG Vulkan SDK](https://vulkan.lunarg.com/)。安装后重新打开终端。
4. [vcpkg](https://github.com/microsoft/vcpkg)：

```powershell
git clone https://github.com/microsoft/vcpkg C:\dev\vcpkg
C:\dev\vcpkg\bootstrap-vcpkg.bat
$env:VCPKG_ROOT = "C:\dev\vcpkg"
```

先验证 Vulkan 环境：

```powershell
vulkaninfo --summary
vkcube
glslc --version
```

`vkcube` 能显示立方体，才继续编译工程。

## 2. 一键构建和运行

在 PowerShell 中进入本目录：

```powershell
Set-ExecutionPolicy -Scope Process Bypass
.\scripts\build_windows.ps1 -VcpkgRoot C:\dev\vcpkg -Config Debug
```

脚本会通过 `vcpkg.json` 安装 GLFW、配置 Visual Studio 工程、编译 GLSL 为 SPIR-V 并运行程序。

也可以手动执行：

```powershell
cmake -S . -B build -G "Visual Studio 17 2022" -A x64 `
  -DCMAKE_TOOLCHAIN_FILE=C:\dev\vcpkg\scripts\buildsystems\vcpkg.cmake
cmake --build build --config Debug
.\build\Debug\vulkan_shader_starter.exe
```

Debug 构建默认开启 `VK_LAYER_KHRONOS_validation`；Release 构建关闭验证层。

## 3. Linux 快速构建

Ubuntu/Debian 示例：

```bash
sudo apt install build-essential cmake ninja-build libglfw3-dev vulkan-tools glslc
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Debug
cmake --build build
./build/vulkan_shader_starter
```

不同发行版的软件包名可能不同。若使用 LunarG SDK，先执行 SDK 的 `setup-env.sh`。

## 4. 先改哪几处

- `shaders/neon.frag`：当前效果。优先改颜色、循环次数、环半径和波形。
- `shaders/fullscreen.vert`：全屏三角形，不需要 VBO。
- `shaders/shadertoy_adapter.frag`：复制单 Pass ShaderToy 的 `mainImage()` 到这里。
- `src/main.cpp::recordCommandBuffer()`：一帧的命令录制。
- `src/main.cpp::drawFrame()`：Acquire → Submit → Present 和同步。
- `src/main.cpp::createGraphicsPipeline()`：固定功能状态和 Shader 组合。

修改 Shader 后再次执行 `cmake --build build --config Debug`，CMake 会自动重新生成 SPIR-V。

要运行适配模板，把 `CMakeLists.txt` 中 `neon.frag` 换成 `shadertoy_adapter.frag`，并在 `createGraphicsPipeline()` 中把读取文件名改成 `shadertoy_adapter.frag.spv`。

## 5. ShaderToy 移植清单

单 Pass、无纹理的 Shader 通常只需：

1. 保留源代码许可证和作者信息；没有明确兼容许可证时不要直接用于产品。
2. 保留 `mainImage(out vec4, in vec2)`，由模板的 `main()` 调用。
3. 把 `iTime`、`iResolution`、`iMouse` 映射到 Push Constants。
4. 把 WebGL 特有语法改为 Vulkan GLSL 450。
5. 用 `glslc --target-env=vulkan1.1` 编译，先消除所有编译/验证错误。

以下内容不能“一贴即用”：

- `iChannel0..3`：要创建 `VkImage`、`VkImageView`、`VkSampler` 和 Descriptor Set。
- Buffer A/B/C/D 多 Pass：要为每个 Pass 建 Render Target、Pipeline 和 Pass 间同步。
- 音频、键盘、摄像头输入：要由宿主程序提供数据。
- 导数、高精度、格式和扩展依赖：必须查询目标 GPU 的 Feature/Format 支持。

## 6. 成功标志与常见错误

成功时：窗口中出现动画霓虹圆环，控制台输出 GPU 名称，Debug 运行没有 Validation Error。

常见错误：

| 症状 | 检查项 |
|---|---|
| CMake 找不到 Vulkan | 确认 `VULKAN_SDK`，重开终端，运行 `glslc --version` |
| 找不到 GLFW | 确认传入 vcpkg toolchain，且使用 x64 |
| 找不到 `.spv` | 先构建 `compile_shaders`，不要移动 build 目录中的 Shader |
| 验证层不可用 | 修复/重装 Vulkan SDK，或临时使用 Release 验证驱动路径 |
| 黑屏但无报错 | 用 RenderDoc 检查 Draw、Render Pass、输出附件；确认 Shader 未产生 NaN |
| 窗口缩放后崩溃 | 看 Swapchain 重建时旧 Framebuffer/Pipeline 的生命周期 |

## 7. 推荐的第一次实验

1. 把循环次数从 6 改为 1，观察 GPU 工作量和画面。
2. 用常量红色替换全部片元逻辑，确认管线而非数学有问题。
3. 把 `VK_PRESENT_MODE_MAILBOX_KHR` 强制改为 `FIFO`，理解呈现节奏。
4. 故意删掉 Render Pass 的外部依赖，看验证层如何描述同步问题。
5. 加一个 `iChannel0` 纹理；这是从“会画 Shader”到“理解资源绑定”的关键一步。

更完整的概念、流程图、接口地图、Android 迁移和能力标准见同包的 `vulkan_opengl_learning_guide.md`。

