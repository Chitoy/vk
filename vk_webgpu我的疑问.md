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
