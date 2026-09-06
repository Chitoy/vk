# 04 · 把 recordCommandBuffer() 展开到每一项状态

先读[shader](02_shader_dataflow.md)与[资源](03_memory_resources.md)两章，再回到 [`main.cpp`](../src/main.cpp) 的 `recordCommandBuffer()`。你会发现命令记录主要是在选定“算法、输入、输出与工作量”。

## 1. Pipeline、Render Pass、Framebuffer 各回答一个问题

| 对象 | 当前含义 | 类比的边界 |
|---|---|---|
| `VkPipeline` | vertex/fragment shader + 顶点格式、拓扑、viewport、光栅化、混合等 | 像一份可执行的渲染配置，但没有指定本帧参数与实际目标图像 |
| `VkPipelineLayout` | shader 资源槽位和 push constant ranges | 只声明接口，不能把它当作 buffer 内容 |
| `VkRenderPass` | attachment 格式/采样数、load/store、subpass、布局与依赖 | 是渲染过程的描述，不拥有目标像素 |
| `VkFramebuffer` | 把实际 image view 放入附件槽位，并给出尺寸 | 选定本次写哪张图 |
| `VkCommandBuffer` | 记录这次绑定什么、参数是什么、draw 几次 | 命令的实际存储方式由实现决定，不应假定是应用可读的 CPU 指令数组 |

经典 render pass 的兼容性允许不同对象在满足规定时配套使用，不是所有地方都要求句柄相等。当前代码总用自己新建的一组对象，容易理解。Pipeline 创建输入及兼容约束见 [Pipelines](https://docs.vulkan.org/spec/latest/chapters/pipelines.html)。

## 2. 命令缓冲是有状态的

```text
allocate -> initial
begin    -> recording
vkCmd*   -> 继续 recording
end      -> executable
submit   -> pending
完成     -> executable；若本次使用 ONE_TIME_SUBMIT，则变为 invalid
reset    -> initial，再 begin 新的一次记录
```

这是常规单次提交的简化路径；secondary、同时提交标志和被引用对象销毁会引入更多状态规则。当前代码每次记录使用 `ONE_TIME_SUBMIT_BIT`，含义是这轮记录的内容只提交一次，**不是这个 command buffer 一生只能用一次**。完成后 reset 可以重新记录下一帧。

`vkResetCommandBuffer` 要求它不在 pending 状态，而且所属 pool 允许单独 reset；本例创建 pool 时设置了 `VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT`。不能用这个 flag 绕过 GPU 尚在执行的限制。规则见 [Command Buffer Lifecycle](https://docs.vulkan.org/spec/latest/chapters/cmdbuffers.html)。

“同一个 CPU 线程里先 submit 后 reset”仍然可能非法，因为 CPU 函数返回时 GPU 尚未完成。你已经掌握的 frame fence 正是这里的完成证明。

## 3. 逐步读当前函数

### 3.1 开始记录：还没有执行清屏或 shader

```cpp
VkCommandBufferBeginInfo beginInfo{};
beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
vkBeginCommandBuffer(commandBuffer, &beginInfo);
```

实际源代码检查了 begin 的 `VkResult`。`sType` 告诉实现你传的是什么结构；`{}` 使其它字段为零，但“全零合法”必须按结构检查，不能当成通用规则。

新一轮记录不能假定继承上次记录的 pipeline、descriptor 或 push constant 内容。后续 draw 所需状态必须在当前有效的状态链中建立。

### 3.2 开始 Render Pass：目标在这里确定

```cpp
renderPassInfo.renderPass = renderPass_;
renderPassInfo.framebuffer = swapChainFramebuffers_[imageIndex];
renderPassInfo.renderArea.offset = {0, 0};
renderPassInfo.renderArea.extent = swapChainExtent_;
renderPassInfo.clearValueCount = 1;
renderPassInfo.pClearValues = &clearColor;
vkCmdBeginRenderPass(commandBuffer, &renderPassInfo, VK_SUBPASS_CONTENTS_INLINE);
```

`imageIndex` 来自 acquire，决定输出图像；`currentFrame_` 选择 command buffer，二者职责不同。`INLINE` 表示本 subpass 内直接记录 draw；若采用 secondary 命令缓冲，继承信息和 subpass contents 要相应变化，见[第 6 章](06_threads_processes_devices.md)。

清屏值只是本轮参数。真正是否加载旧内容、清屏、保存结果，来自 `createRenderPass()`：

| 字段 | 原值 | 作用 |
|---|---|---|
| `loadOp` | `CLEAR` | 执行渲染时按附件规则清理 render area 的对应内容 |
| `storeOp` | `STORE` | 保留结果，供后续呈现 |
| `initialLayout` | `UNDEFINED` | 本轮不要求保留原内容；不免除同步前提 |
| subpass layout | `COLOR_ATTACHMENT_OPTIMAL` | 颜色附件用途 |
| `finalLayout` | `PRESENT_SRC_KHR` | 结束后作为呈现图像 |

`renderArea`、viewport、scissor 都有尺寸，但不是同一个开关：renderArea 定义这次渲染实例的区域，viewport 做 NDC→framebuffer 映射，scissor 限定光栅输出区域。当前三者都覆盖全窗口。

### 3.3 绑定 Pipeline：选本次 draw 的算法与固定状态

```cpp
vkCmdBindPipeline(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, graphicsPipeline_);
```

pipeline 中已确定：

| 状态 | 本项目的配置 | 结果 |
|---|---|---|
| shader | `fullscreen.vert.spv` + `neon.frag.spv`，入口均为 `main` | 两个阶段使用各自程序 |
| vertex input | 无 binding/attribute 描述 | 顶点由 shader 索引生成 |
| topology | `TRIANGLE_LIST` | 每三个顶点组成一个三角形 |
| viewport/scissor | 创建时使用 swapchain extent | 尺寸变后需要新的 pipeline 或改为动态状态 |
| polygon mode | `FILL` | 填充三角形内部 |
| cull mode | `NONE` | 不按正反面剔除 |
| multisample | 1 sample | 此例未使用 MSAA |
| depth/stencil | 未配置深度附件与测试 | 不处理三维遮挡 |
| blend | 默认 false | 输出不会因 alpha 自动透明叠加 |
| write mask | RGBA | 允许四个颜色分量写入 |

绑定 pipeline 不会自动写入时间参数、不选择 vertex buffer，也不上传图像。动态状态若被声明为动态，还要在 draw 前通过对应 `vkCmdSet*` 建立。

### 3.4 CPU 采样时间与鼠标，记录 32 字节

`std::chrono` 和 `glfwGetCursorPos` 都是当前 CPU 代码。当 GPU 稍后执行时，读取的是这一轮录制时的值；不是 shader 执行到某条指令才去查询 GLFW。

```cpp
vkCmdPushConstants(commandBuffer, pipelineLayout_,
    VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(constants), &constants);
```

含义是更新命令状态里 fragment stage 可读的指定范围。`offset/size` 以字节为单位，并有四字节粒度及 range 覆盖等有效使用要求。当前 32 字节对应表见[第 2 章](02_shader_dataflow.md)。[vkCmdPushConstants](https://docs.vulkan.org/refpages/latest/refpages/source/vkCmdPushConstants.html)

两个容易混淆的 enum：

| 写法 | 在哪里使用 | 表达什么 |
|---|---|---|
| `VK_SHADER_STAGE_FRAGMENT_BIT` | descriptor/push constant 的可见阶段 | 哪个 shader 阶段允许通过接口读取 |
| `VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT` | barrier/semaphore 等同步范围 | 哪个执行阶段参与依赖 |

前者不是等待指令，后者不是 shader 资源授权。两者名称相近，不能混用。

### 3.5 Draw：工作量，而不是三个颜色

```cpp
vkCmdDraw(commandBuffer, 3, 1, 0, 0);
// vertexCount=3, instanceCount=1, firstVertex=0, firstInstance=0
```

这是非索引绘制，三个顶点索引为 0、1、2。当前 shader 的 `POSITIONS[gl_VertexIndex]` 依赖这个范围。如果只把 `vertexCount` 改成 6，原数组就越界；不是自动变出另一个全屏三角形。[vkCmdDraw](https://docs.vulkan.org/refpages/latest/refpages/source/vkCmdDraw.html)

draw 使用之前建立的 pipeline 与参数状态。在一个 render pass 内可以写：

```text
bind pipeline A
push 参数 A
draw A
push 参数 B
draw B
```

两次 draw 使用各自前面的 push constant 值。普通 buffer/descriptor 内容却不是这种按 draw 自动快照的机制：若两个 draw 都指向同一 buffer 范围，CPU 改这段内存并不生成两份历史数据。需要不同范围、不同帧槽或合适的资源更新与同步设计。

### 3.6 End Render Pass / End Command Buffer

```cpp
vkCmdEndRenderPass(commandBuffer);
vkEndCommandBuffer(commandBuffer);
```

前者记录结束渲染实例，attachment 的 store/final layout 等按已声明规则执行；后者结束录制，使 command buffer 可供提交。二者都不是“等待 GPU”。

本例 render pass 的外部→subpass dependency 和 acquire wait 一起约束第一次颜色附件访问及其布局转换。`srcAccessMask=0` 不代表任何旧 GPU 写入都已自动可见，而是此例丢弃旧内容、配合 WSI 获取依赖的特定设计。将图像改作上一 pass 输出的采样纹理后，需要重新设计 stage/access/layout；不能原样沿用 present 方案。

## 4. 一个输出槽如何准确落到一张图

把源码中的索引按顺序连接：

```text
fragment: layout(location=0) out vec4 outColor
    -> subpass.pColorAttachments[0]
    -> colorReference.attachment = 0
    -> framebuffer 创建时 pAttachments[0]
    -> swapChainImageViews_[imageIndex]
    -> swapChainImages_[imageIndex]
```

MRT（多个颜色目标）会让 `location=1` 对应 subpass 颜色槽 1，再由该槽的 attachment reference 选择实际附件。location 并不是任何情况下都直接等于 framebuffer 数组索引。接口规则见 [Shader Interfaces](https://docs.vulkan.org/spec/latest/chapters/interfaces.html)。

## 5. 扩展到真实 Vertex Buffer：完整接口，明确集成边界

下面是**接入片段**，不是一个独立程序。Buffer 的 create/allocate/bind/upload 按[第 3 章](03_memory_resources.md)实现；这里展示字节如何变成 vertex shader 输入。用两个 float 的位置加三个 float 的颜色，避开矩阵库和隐式对齐。

```cpp
#include <cstddef>
#include <type_traits>

struct Vertex {
    float position[2];
    float color[3];
};
static_assert(std::is_standard_layout_v<Vertex>);
static_assert(sizeof(float) == 4);
static_assert(offsetof(Vertex, position) == 0);
static_assert(offsetof(Vertex, color) == 8);
static_assert(sizeof(Vertex) == 20);

const Vertex vertices[] = {
    {{-0.6f, -0.5f}, {1.0f, 0.0f, 0.0f}},
    {{ 0.6f, -0.5f}, {0.0f, 1.0f, 0.0f}},
    {{ 0.0f,  0.6f}, {0.0f, 0.0f, 1.0f}},
};

VkVertexInputBindingDescription binding{};
binding.binding = 0;
binding.stride = sizeof(Vertex);
binding.inputRate = VK_VERTEX_INPUT_RATE_VERTEX;

VkVertexInputAttributeDescription attributes[2]{};
attributes[0].location = 0;
attributes[0].binding = 0;
attributes[0].format = VK_FORMAT_R32G32_SFLOAT;
attributes[0].offset = offsetof(Vertex, position);
attributes[1].location = 1;
attributes[1].binding = 0;
attributes[1].format = VK_FORMAT_R32G32B32_SFLOAT;
attributes[1].offset = offsetof(Vertex, color);

// Set these in createGraphicsPipeline() before vkCreateGraphicsPipelines().
vertexInput.vertexBindingDescriptionCount = 1;
vertexInput.pVertexBindingDescriptions = &binding;
vertexInput.vertexAttributeDescriptionCount = 2;
vertexInput.pVertexAttributeDescriptions = attributes;
```

每个顶点步长为 20 字节；位置从该顶点 offset 0 读取 8 字节，颜色从 offset 8 读取 12 字节。这是 vertex input 排布，**不是 UBO 的 std140**。别给所有 vertex `vec3` 盲目补成 16 字节。

配套 vertex shader：

```glsl
#version 450
layout(location=0) in vec2 inPosition;
layout(location=1) in vec3 inColor;
layout(location=0) out vec3 colorToFragment;
void main() {
    gl_Position = vec4(inPosition, 0.0, 1.0);
    colorToFragment = inColor;
}
```

配套 fragment shader：

```glsl
#version 450
layout(location=0) in vec3 colorToFragment;
layout(location=0) out vec4 outColor;
void main() { outColor = vec4(colorToFragment, 1.0); }
```

shader 的 `inColor location=1` 属于**顶点输入接口**，`colorToFragment location=0` 属于**阶段间接口**；它们不是同一个编号空间。Vertex input 的 `binding=0` 也不是 descriptor `set=0,binding=0`。

在 draw 前绑定已准备好的 buffer：

```cpp
// Preconditions: vertexBuffer owns sizeof(vertices) initialized bytes,
// usage includes VERTEX_BUFFER, memory is bound, upload dependency is satisfied.
VkDeviceSize byteOffset = 0;
vkCmdBindVertexBuffers(commandBuffer, 0, 1, &vertexBuffer, &byteOffset);
vkCmdDraw(commandBuffer, 3, 1, 0, 0);
```

最小接入检查：创建/上传放在初始化阶段；只读顶点可跨帧共享；GPU 最后一次读取完成后先 destroy buffer 再 free memory；更换两个 shader 与 vertexInput 后重建 pipeline。旧 push constant range 可以保留为未使用的额外接口，但保留 push 调用时仍须满足其有效使用规则。

如果用 staging copy 上传，在**同一个 graphics queue**中以 legacy barrier 建立 `TRANSFER/TRANSFER_WRITE → VERTEX_INPUT/VERTEX_ATTRIBUTE_READ` 依赖，并在 transfer 使用完成前保留 staging；完整访问链见第 3 章。不要仅因 CPU 的 `memcpy` 完成就销毁 staging。

## 6. 再增加 Index、Uniform、Texture 时各增加什么

| 需求 | 初始化增加 | draw 前增加 | shader 变化 |
|---|---|---|---|
| 索引复用顶点 | 带 `INDEX_BUFFER` usage 的索引 buffer + 上传 | `vkCmdBindIndexBuffer`，换为 `vkCmdDrawIndexed` | 顶点输入可以不变 |
| 每帧相机矩阵 | 每帧 UBO/range、descriptor layout/pool/sets | fence 后写本帧 UBO，bind 本帧 descriptor | `layout(set=0,binding=0) uniform`，乘 MVP |
| 材质纹理 | image/memory/view/sampler、上传及布局、descriptor | bind 材质 descriptor | `sampler2D` + `texture()` |
| 真实遮挡 | depth image/view、render pass/framebuffer/pipeline 状态 | 清理并正确使用深度附件 | 通常无需写 `gl_FragDepth` |
| 离屏后处理 | 离屏颜色图 + 采样接口 + 第二个 pass/pipeline | pass A 写，依赖/布局转换，pass B 采样 | 第二个 fragment 读取 A 的图 |

索引 draw 的 `vertexOffset` 与 `firstIndex`、buffer 绑定字节偏移各有不同单位，按接口确认，不能混作一个 offset。

## 7. 正确记录一次 Draw 的自检问题

1. Command buffer 属于可提交到这个 queue family 的 pool 吗？当前是否 recording？
2. 当前 render pass/subpass 与 pipeline 是否兼容？目标图尺寸、格式、采样数是否匹配？
3. shader 的每个实际输入从哪个 binding/location/offset 得到？
4. 所需 buffer/image 是否已绑定内存、初始化、可见，并在正确 layout/ownership？
5. 必需的 pipeline、descriptor、vertex/index、dynamic state、push constants 是否建立？
6. draw 计数和偏移会不会越界？
7. 从现在到相关 GPU 使用完成，谁保证这些对象与内容不会被改写或销毁？

这七问可以直接用来评审一个新的 draw，不必等出错后再随机改变同步参数。

下一章：[窗口变化与生命周期](05_resize_lifetime.md)。
