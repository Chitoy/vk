vulkan gpu流程
Queue
  │
  ▼
Command Processor
  │
  ├─ Bind Pipeline
  │
  ├─ Bind Descriptor
  │
  ├─ Bind Vertex Buffer
  │
  └─ DRAW
        │
        ▼
     Vertex Fetch
        │
        ▼
     Vertex Shader
        │
        ▼
     Primitive
        │
        ▼
     Rasterizer
        │
        ▼
     Fragment Shader
        │
        ▼
     Depth
        │
        ▼
      Blend
        │
        ▼
     Render Target

图形引擎流程
Frame
│
├── Shadow Pass
│
│    ├── Pipeline
│
│    └── Draws
│
├── GBuffer / Opaque Pass
│
│    ├── Pipeline A
│
│    │    ├── Material 1
│
│    │    │    ├── Mesh
│
│    │    │    └── Mesh
│
│    │    └── Material 2
│
│    └── Pipeline B
│
├── Transparent Pass
│
├── Post Process Pass
│
└── UI Pass


webgpu
┌────────────────────────────────────────────┐
│            图形引擎层 / Renderer           │
│                                            │
│ Frame                                      │
│ ├── Shadow Pass                            │
│ ├── GBuffer Pass                           │
│ ├── Lighting Pass                          │
│ ├── Transparent Pass                       │
│ ├── PostProcess Pass                       │
│ └── UI Pass                                │
└──────────────────────┬─────────────────────┘
                       │
                       ▼
┌────────────────────────────────────────────┐
│                 WebGPU API                 │
│                                            │
│ GPUDevice                                  │
│   │                                        │
│   ├── GPUBuffer / GPUTexture               │
│   ├── GPUBindGroup                         │
│   ├── GPURenderPipeline                    │
│   │                                        │
│   └── GPUCommandEncoder                    │
│         │                                  │
│         ├── RenderPassEncoder              │
│         │    ├── setPipeline               │
│         │    ├── setBindGroup              │
│         │    ├── setVertexBuffer           │
│         │    └── draw                      │
│         │                                  │
│         └── ComputePassEncoder             │
│              ├── setPipeline               │
│              ├── setBindGroup              │
│              └── dispatchWorkgroups        │
│                                            │
│ GPUCommandBuffer                           │
│        │                                   │
│ GPUQueue.submit()                          │
└──────────────────────┬─────────────────────┘
                       │
                       ▼
┌────────────────────────────────────────────┐
│             Vulkan / Metal / D3D12         │
│                      │                     │
│                      ▼                     │
│                    GPU                     │
│                                            │
│ Queue                                      │
│ ↓                                          │
│ Command Processor                          │
│ ↓                                          │
│ Vertex Fetch                               │
│ ↓                                          │
│ Vertex Shader                              │
│ ↓                                          │
│ Rasterizer                                 │
│ ↓                                          │
│ Fragment Shader                            │
│ ↓                                          │
│ Depth / Blend                              │
└────────────────────────────────────────────┘


| Vulkan                          | WebGPU                 |
| ------------------------------- | ---------------------- |
| `VkPhysicalDevice`              | `GPUAdapter`           |
| `VkDevice`                      | `GPUDevice`            |
| `VkQueue`                       | `GPUQueue`             |
| `VkBuffer`                      | `GPUBuffer`            |
| `VkImage`                       | `GPUTexture`           |
| `VkImageView`                   | `GPUTextureView`       |
| `VkSampler`                     | `GPUSampler`           |
| `VkDescriptorSetLayout`         | `GPUBindGroupLayout`   |
| `VkDescriptorSet`               | `GPUBindGroup`         |
| `VkPipelineLayout`              | `GPUPipelineLayout`    |
| `VkPipeline`                    | `GPURenderPipeline`    |
| `VkShaderModule`                | `GPUShaderModule`      |
| `VkCommandBuffer` 录制过程          | `GPUCommandEncoder`    |
| Dynamic Rendering / Render Pass | `GPURenderPassEncoder` |
| `vkCmdBindPipeline`             | `setPipeline()`        |
| `vkCmdBindDescriptorSets`       | `setBindGroup()`       |
| `vkCmdBindVertexBuffers`        | `setVertexBuffer()`    |
| `vkCmdBindIndexBuffer`          | `setIndexBuffer()`     |
| `vkCmdDraw`                     | `draw()`               |
| `vkCmdDrawIndexed`              | `drawIndexed()`        |
| Compute Dispatch                | `dispatchWorkgroups()` |
| `vkQueueSubmit`                 | `queue.submit()`       |


VMA 
你的 Renderer / Engine
        │
        ▼
 VkBuffer / VkImage
        │
        ▼
       VMA
        │
        ├── 选择 Memory Type
        ├── 分配 VkDeviceMemory
        ├── Suballocation // 再分配
        ├── bindMemory
        ├── map/unmap
        ├── 内存池
        ├── Budget
        └── Defragmentation
        │
        ▼
 Vulkan Memory API
        │
        ▼
 VkDeviceMemory
        │
        ▼
 VRAM / System RAM

 VMA 管理：

哪些区域用了
哪些区域空闲
哪些 allocation 属于哪个 resource
怎么对齐
怎么重用空洞


VMA 四个核心对象
VmaAllocator // 整个 VMA 的总管理器

VmaAllocation // 内存分配执行类

VmaAllocationInfo // 内存分配执行类 所需的信息数据

VmaPool


例如：
VmaAllocator allocator;

VmaAllocatorCreateInfo info{};
info.instance = instance;
info.physicalDevice = physicalDevice;
info.device = device;

vmaCreateAllocator(
    &info,
    &allocator
); // 这里有个疑问 这个allocator是在CPU内存还是GPU内存上 应该是在CPU内存上


不用VMA创建内存
vkCreateBuffer

vkGetBufferMemoryRequirements

findMemoryType

vkAllocateMemory

vkBindBufferMemory


// 使用VMA
VkBufferCreateInfo bufferInfo{};

VmaAllocationCreateInfo allocInfo{};

VkBuffer buffer;
VmaAllocation allocation;

vmaCreateBuffer(
    allocator,
    &bufferInfo,
    &allocInfo,
    &buffer,
    &allocation,
    nullptr
);
vkCreateBuffer
      ↓
vkGetBufferMemoryRequirements
      ↓
选择 memoryType
      ↓
寻找已有 Memory Block
      ↓
找到空闲区域
      ↓
Suballocate
      ↓
vkBindBufferMemory

8. Image 同样

不用 VMA：

vkCreateImage
↓
vkGetImageMemoryRequirements
↓
memoryType
↓
vkAllocateMemory
↓
vkBindImageMemory

VMA：

vmaCreateImage(
    allocator,
    &imageInfo,
    &allocInfo,
    &image,
    &allocation,
    nullptr
);

所以通常：

VkBuffer   + VmaAllocation

VkImage    + VmaAllocation

成对出现。


正因为原生 Vulkan 编写子分配算法极其繁琐（需要自己管理内存对齐、碎片整理、多线程分配等），AMD 才开发了 VMA 库。VMA 内部替你调用了 vkAllocateMemory 申请大块内存（称为 VmaBlock），并在 CPU 端算法中将其切分成无数个小块。当你调用 VMA 的接口时：VMA 的 VmaAllocation 对象，在底层其实就对应着原生 Vulkan 的 VkDeviceMemory + Offset + Size 的组合。

10. Memory Heap 和 Memory Type

这部分其实和你之前 Vulkan Memory 的知识直接衔接。

GPU 可能有：

Heap 0
DEVICE_LOCAL
8GB VRAM

Heap 1
HOST RAM
32GB

然后每个 Heap 可能暴露多个：

Memory Type

例如：

MemoryType 0

DEVICE_LOCAL

适合：

Vertex Buffer
Index Buffer
Texture
Render Target

例如：

MemoryType 1

HOST_VISIBLE
HOST_COHERENT

适合：

Staging Buffer
Uniform Upload
CPU → GPU

VMA 就是在帮你做：

Resource Requirements
        +
Allocation Requirements
        ↓
选择最佳 Memory Type

所以你很多时候不用自己：

findMemoryType()

                    VmaAllocator
                         │
          ┌──────────────┼──────────────┐
          ▼              ▼              ▼
      MemoryType 0   MemoryType 1   MemoryType 2
          │              │              │
          ▼              ▼              ▼
      Block List      Block List      Block List
          │
      ┌───┼───────────────┐
      ▼   ▼               ▼
    Block A             Block B
  VkDeviceMemory       VkDeviceMemory
      │                  │
      │                  │
 ┌────┼─────┐       ┌────┼─────┐
 ▼    ▼     ▼       ▼    ▼     ▼
Alloc Alloc Free    Alloc Free Alloc


？？？？？也就是说VMA的VmaAllocator 可以同时拥有好几个VMAPOOL是的，完全正确！一个 VmaAllocator 内部不仅可以同时拥有好几个 VmaPool，而且在默认情况下，它一出生就自带了对应所有 Vulkan 内存类型的默认 Pool。这里可以用两个层面来彻底理解 VMA 的 Pool 架构：1. 默认自带的多个 Pool（按硬件内存类型划分）当你调用 vmaCreateAllocator 时，VMA 会查询当前 GPU 硬件支持的所有内存类型（Memory Types）。不同的显卡硬件会报告不同的类型数量（通常在 3 到 10 个之间）。VMA 会为每一个硬件内存类型，都在内部自动创建一个默认的 Pool：Pool 0：专门针对 DEVICE_LOCAL_BIT（纯 GPU 显存，不映射）。Pool 1：专门针对 HOST_VISIBLE_BIT | HOST_COHERENT_BIT（CPU 可见且自动同步的内存，大块持久映射）。Pool 2：专门针对 HOST_VISIBLE_BIT | HOST_CACHED_BIT（带 CPU 缓存的内存，常用于将 GPU 数据读回 CPU，大块持久映射）。当你调用 vmaCreateBuffer 且没有指定特殊 Pool 时，VMA 会根据你对内存的需求（比如要快、还是要 CPU 能写），自动把你的请求分流到这几个默认 Pool 中的某一个去。2. 用户自定义的多个 Pool（VmaPool）除了默认的 Pool，VMA 还允许开发者通过 vmaCreatePool 手动创建无数个自定义的 VmaPool。在实际的自研引擎开发中，这是一个非常高级且常用的优化手段。你可以让一个 Allocator 同时管理以下这些你亲手创建的 Pool：transient_images_pool：用于管理每帧渲染完就丢弃的临时 Render Target（如延迟渲染的 G-Buffer）。设置标志让它永远不向系统申请真正的物理显存（使用 LAZILY_ALLOCATED），纯粹靠 GPU 缓存运转。uniform_buffer_pool：专门用来放 Uniform Buffer。你可以限制这个 Pool 的 blockSize（比如每个大块只有 4MB），并且强制它创建出来的所有 Block 全部开启持久映射。streaming_textures_pool：专门用来做大纹理流式加载的 Pool。为了防止小资源产生碎片，你可以限制这个 Pool 只能分配固定大小（如 64KB）的内存块。
这里是不是可以作为webgpu内存优化的点


？？？？17. Dedicated Allocation

有些很大的 Resource 不适合和别人挤一个 Block。

例如：

4K HDR Render Target

500MB

这时 VMA 可能：

Dedicated Allocation

也就是：

VkImage
   │
   ▼
VkDeviceMemory

整块内存专门属于它。

而不是：

VkDeviceMemory
├── Texture A
├── Buffer B
└── Render Target

VMA 会根据：

allocation size
driver requirements
resource hints

决定。 这个是全自动的吗还是写的时候手动去做



VMA 还能做 Budget

比如你 GPU：

VRAM 24GB

并不代表你应该一直分配到：

23.99GB

VMA 可以获取：

heap usage
heap budget

VMA Defragmentation  可以通过接口进行碎片化整理但本身比较复杂，WEBGPU内存这里可能有优化点 ？？？？？


VMA与Barrier 区别
VMA
=
Memory Allocation

Barrier
=
Memory Dependency