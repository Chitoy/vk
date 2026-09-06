# 01 · 从一个二维点到屏幕上的三维场景

你问是否需要补世界坐标、相机、clip、正交与透视。答案是需要，但可以分层补：**当前霓虹先学二维函数、坐标和插值；三维模型再学矩阵、相机、投影、深度与光照。** 本章把这些概念接到当前代码，而不是另开一门脱离项目的数学课。

## 1. 先把 shader 看成函数

当前片元 shader 可以写成数学形式：

```text
颜色 = F(屏幕位置, 时间, 鼠标)
```

窗口上不同位置执行同一段程序，输入位置不同，结果颜色不同。动画通常是下一帧重新代入不同的时间，重新算出颜色。当前 shader 没有保存“上一帧圆环的位置”。

最简单的例子是 `color=vec3(uv.x,uv.y,0)`：越向右红分量越大，越向下绿分量越大。再把“到圆心距离接近半径”映射成高亮，就得到圆环。

三维渲染则首先回答“哪些模型表面投影到哪些屏幕位置”，再在这些片元上计算颜色。两个流程共享图形 pipeline，但几何意义不同。

## 2. 二维运算必须会手算

| 运算 | 几何意义 | 示例 |
|---|---|---|
| `p+offset` | 改变用于计算的位置 | `p+(0.1,0)`；注意函数图案的视觉移动方向往往相反 |
| `p*scale` | 缩放采样坐标 | `length(2*p)<1` 画出半径 0.5 的圆 |
| `length(p)` | 到原点距离 | `(0.3,0.4)` 的长度为 0.5 |
| `dot(a,b)` | 投影/方向相似度的基础 | 单位向量夹角余弦 |
| `normalize(v)` | 只保留方向 | `v/length(v)`；零向量需要另外处理 |
| `abs(length(p)-r)` | 到理想圆周的距离 | `r=0.5`，`p=(0.5,0)` 时为 0 |
| `mix(a,b,t)` | 线性插值 | `(1-t)*a+t*b`，`t=0.25` 更靠近 a |
| `sin(t)` / `cos(t)` | 周期变化 | GLSL 三角函数参数是弧度 |

对于隐式函数 `f(p)`，若把代码改为 `f(p-offset)`，图案中心会出现在 `p=offset`；因此 `p += mouseOffset` 不代表圆环中心跟鼠标同向移动。先解方程比靠直觉更可靠。

### 宽高比为什么只乘 X？

UV 的横纵范围都是 0～1，但 1280×720 的屏幕中一个单位横跨 1280 像素、纵跨 720 像素。

```glsl
vec2 p = uv * 2.0 - 1.0;
p.x *= width / height;
```

这样每移动一个像素，横向 `p` 增量约为 `2*(width/height)/width=2/height`，纵向也为 `2/height`。两个轴的度量一致，`length(p)` 得到的圆在屏幕上才圆。还可以用：

```glsl
vec2 p = (2.0 * pixelPosition - resolution) / resolution.y;
```

这是同一种按屏幕高度统一单位的约定。

## 3. 六种坐标空间分别解决什么

假设你在桌子上放一个立方体，相机位于房间内。

| 空间 | 原点/轴是谁规定的 | 典型数据 | 为什么需要 |
|---|---|---|---|
| 模型/局部 model | 模型作者 | 立方体中心附近的顶点 | 同一个网格可以放在多个地方 |
| 世界 world | 场景设计者 | 房间里物体的位置 | 统一描述物体、灯光和相机 |
| 观察/相机 view | 相机成为参考系 | 本章相机朝 -Z 看 | 投影公式可固定写在相机原点 |
| 裁剪 clip | 投影后的齐次坐标 | `gl_Position=(x,y,z,w)` | 在除以 w 前裁剪图元 |
| 归一化设备 NDC | clip.xyz / clip.w | 默认可见 x/y 为 -1～1，z 为 0～1 | 与像素尺寸分离 |
| framebuffer | viewport 的像素范围 | 例如 `(640.5,360.5)` | 光栅化、采样位置和附件访问 |

texture UV 又是一种独立坐标：用来访问纹理或参数化表面，一般约定 0～1，但 Vulkan 没有规定所有 UV 必须落在这个范围。当前全屏三角形的顶点 UV 包含 2。

Vulkan 固定功能主要从 **clip 坐标** 开始理解几何；model/world/view 是你自己在 shader 中组织的数学过程。默认裁剪和 viewport 定义见 [Fixed-Function Vertex Post-Processing](https://docs.vulkan.org/spec/latest/chapters/vertexpostproc.html)。

```mermaid
flowchart LR
    A["模型坐标 p"] -->|M| B["世界坐标"]
    B -->|V| C["相机坐标"]
    C -->|P| D["clip xyzw<br/>gl_Position"]
    D --> E["裁剪"]
    E -->|"xyz / w"| F["NDC"]
    F --> G["viewport 与光栅化"]
    G --> H["片元输入/颜色"]
```

## 4. 齐次坐标与矩阵：先会用，再扩展推导

三维点写成 `vec4(x,y,z,1)`，方向常写成 `vec4(dx,dy,dz,0)`。仿射变换中的平移列乘以 w，因此会平移点而不会平移方向。

采用 GLSL 常用的矩阵乘列向量约定：

```glsl
gl_Position = projection * view * model * vec4(inPosition, 1.0);
```

右侧的 model 先作用。`M=T*R*S` 表示先缩放、再旋转、再平移。矩阵乘法一般不交换：把物体绕自身原点转再搬到右侧，与先搬到右侧再绕世界原点转，结果不同。

世界空间中的相机变换若是 `C`，view 矩阵是 `inverse(C)`。相机向右移动，相对于相机的静止场景就向左移动。

GLSL `mat4` 默认按列组织，构造器每四个标量组成一列。CPU 侧的存储排布、数学中的乘法约定和 shader block 的布局是三个需要同时明确的约定；别仅凭“都是 4×4”就直接复制任何矩阵库的 64 字节。GLSL 类型与构造规则见 [Variables and Types](https://docs.vulkan.org/glsl/latest/chapters/variables.html) 和 [Operators and Expressions](https://docs.vulkan.org/glsl/latest/chapters/operators.html)。

## 5. 透视的核心是除以距离

本章选择：右手相机空间，X 向右、Y 向上、相机朝 -Z；near `n>0`，far `f>n`；正高度 Vulkan viewport，所以在投影中翻转一次 Y。

设垂直视角 `fovY`，`s=1/tan(fovY/2)`，宽高比 `a=width/height`。对相机点 `(x,y,z)`，构造：

```text
x_clip = s*x/a
y_clip = -s*y                 本章选择的 Y 翻转
z_clip = f*z/(n-f) + f*n/(n-f)
w_clip = -z
```

这个投影由以下 GLSL 矩阵表达，参数按列填写：

```glsl
mat4 P = mat4(
    s/a, 0, 0, 0,
    0, -s, 0, 0,
    0, 0, f/(n-f), -1,
    0, 0, f*n/(n-f), 0
);
```

随后固定功能阶段做 `NDC=clip.xyz/clip.w`。于是 `x_ndc=s*x/(a*(-z))`：距离增大两倍，同样横向长度投影成一半，这就是近大远小。

### 完整数值链

选择 `fovY=90°`、`a=1`、`n=1`、`f=10`、相机点 `(1,0,-2)`：

```text
s = 1
clip = (1, 0, 10/9, 2)
NDC  = (0.5, 0, 5/9)
```

800×800 viewport、`x=y=0`、深度范围 `[0,1]` 时，连续 framebuffer 坐标是 `(600,400)`，深度约 0.556。这是投影点的位置，不是说光栅化一定生成坐标恰好为 `(600,400)` 的像素中心。

同样横向位置、深度换成 `z=-4`，`x_ndc=0.25`，屏幕 X 变为 500。离屏幕中心更近了。

检查 near/far：`z=-n` 时 `z_clip=0`，除法后为 0；`z=-f` 时 `z_clip=f`、`w_clip=f`，除法后为 1。near 不能取零。

## 6. 正交投影不做近大远小

正交相机保持 `w_clip=1`。在以原点为中心、半高 `h`、半宽 `a*h` 的观察盒中：

```text
x_clip = x/(a*h)
y_clip = -y/h
z_clip = (-z-n)/(f-n)
w_clip = 1
```

两个横向尺寸相同但深度不同的物体，屏幕大小相同。它适合工程视图、某些二维界面和等距风格，但仍有深度与裁剪，并不等于“没有 Z”。

Vulkan 不需要一个“启用透视”开关。正交与透视都是 vertex shader 如何产生 `gl_Position` 的差别。

## 7. Clip 不是 NDC：为什么原三角形可以写到 3？

原 [`fullscreen.vert`](../shaders/fullscreen.vert) 输出：

```text
(-1,-1,0,1), (3,-1,0,1), (-1,3,0,1)
```

默认 clip 体积满足 `-w≤x≤w`、`-w≤y≤w`、`0≤z≤w`。图元可以部分在这个体积外；固定功能裁剪后保留内部部分。不是某个顶点越界就把整个三角形丢掉。

所有 w 都为 1，所以这个例子里 clip.xyz 与 NDC 数值一样。这个巧合容易让人误以为 `gl_Position` 永远要求直接写 -1～1。透视例子里的 clip 值 `(1,0,10/9,2)` 就证明不是。

不要在一般 vertex shader 中先做 `position.xyz /= position.w`、再强行把 w 设为 1 来“帮 GPU 除法”；你改变了裁剪和插值需要的信息。

## 8. Viewport、Y 轴、像素中心

当前代码设置正宽高 viewport，映射公式为：

```text
x_fb = viewport.x + viewport.width  * (x_ndc+1)/2
y_fb = viewport.y + viewport.height * (y_ndc+1)/2
z_fb = minDepth + (maxDepth-minDepth)*z_ndc
```

于是 NDC 的 y=-1 映射到 framebuffer 顶边，y=+1 映射到底边。原全屏 shader 令 `vUV=position*0.5+0.5`，所以 UV 的 y=0 也在上方。

如果世界/相机 Y 希望向上，本章投影公式选择翻一次 Y；另一种实现是负高度 viewport，需要同步调整 viewport 原点并检查正面朝向。两种方案不要叠加翻转。纹理数据的行顺序和鼠标坐标也应在明确的边界转换。

常规 fragment 坐标采用半整数像素中心：左上第一个像素中心约为 `(0.5,0.5)`。`gl_FragCoord.xy` 是 framebuffer 坐标；`vUV*resolution` 在当前全屏映射下近似与它对应，不是所有模型都如此。内建变量定义见 [GLSL Built-In Variables](https://docs.vulkan.org/glsl/latest/chapters/builtins.html)。

## 9. 从三顶点到许多 fragment：重心坐标与插值

对投影后三角形中的一点，光栅化器可以用重心权重 `λ0,λ1,λ2` 描述它，三者相加为 1。若属性是默认平滑插值，计算形式为：

```text
attribute = Σ(λi * attribute_i / w_i) / Σ(λi / w_i)
```

这叫透视正确插值。屏幕重心权重不是直接等同于三维表面上的参数权重。`noperspective` 使用屏幕线性插值 `Σ(λi*attribute_i)`；`flat` 取 provoking vertex 的值而不插值。规则见 [Rasterization / interpolation](https://docs.vulkan.org/spec/latest/chapters/primsrast.html)。

手算一条边的中点：`λ=(0.5,0.5)`、`w=(1,2)`、属性 `u=(0,1)`。

```text
普通屏幕线性插值：u=0.5
透视正确插值：u=(0+0.25)/(0.5+0.25)=1/3
```

当前全屏三角形的所有 w 相同，公式会退化成普通线性插值，所以看不到两者差别。对应的[投影实验](examples/projection/README.md)刻意让顶点深度不同，同时输出 `smooth` 与 `noperspective` UV 来对照。

fragment 也不应简单等同于最终屏幕像素：存在三角形覆盖重叠、深度测试、discard、多重采样和用于导数的 helper invocation。当前单采样全屏示例可以先近似为“每个覆盖位置算颜色”，但不能据此设计跨 invocation 执行顺序。

## 10. 深度、混合和光照怎样接入

### 深度决定前后遮挡

当前 pipeline 没有深度附件，也没启用深度测试；只画一个全屏三角形不需要它。一般三维场景需要：

1. 选择支持 depth attachment 的格式，创建匹配尺寸和采样数的 image、memory、view。
2. 在 render pass/subpass/framebuffer 接入 depth attachment。
3. pipeline 启用 depth test/write，普通正向深度常用 `LESS` 并 clear 为 1。
4. 给深度图安排访问、布局与生命周期；多帧重叠时避免未同步共享同一可写深度图。

投影的深度通常非线性，near 设得过小会损害远处精度。Reverse-Z 是后续专题，需要投影、clear、compare 和深度格式等一起设计。深度测试可能在 shader 前后以合法方式实现，不必想象固定成一次严格的串行硬件步骤。

### Alpha 不会自动透明

`outColor.a=0.5` 不自动产生透明混合；当前 `blendEnable` 为 false，写的是输出值。要混合须配置 blend state，并让不透明/透明物体的绘制顺序、深度写入和颜色空间共同匹配。常规透明还涉及排序，不能只换一个 alpha 值。

### 光照需要真实几何方向

最小 Lambert 漫反射模型可以写为：

```glsl
vec3 N = normalize(worldNormal);
vec3 L = normalize(lightPosition - worldPosition);
float diffuse = max(dot(N, L), 0.0);
vec3 linearColor = albedo * lightColor * diffuse;
```

N 和 L 必须在同一个坐标空间。模型含非均匀缩放时，法线常用 `transpose(inverse(mat3(model)))` 变换后重新归一化，不能总像位置那样直接乘 model。实际渲染再考虑环境光、材质、阴影、能量关系、BRDF 等；当前霓虹是程序化颜色函数，不是真实场景光照，也没有真正 bloom pass。

深度、blend 等状态定义分别见 [Fragment Operations](https://docs.vulkan.org/spec/latest/chapters/fragops.html) 与 [Framebuffer Operations](https://docs.vulkan.org/spec/latest/chapters/framebuffer.html)。

## 11. 接下来补什么，什么时候补

| 当前任务 | 必备知识 | 后续再补 |
|---|---|---|
| 看懂霓虹 | 向量、长度、三角函数、smoothstep、UV/aspect | 噪声、导数、SDF/ray marching |
| 渲染纹理三角形 | 顶点属性、UV 插值、采样、颜色空间 | mip、过滤、各向异性、压缩纹理 |
| 渲染三维场景 | M/V/P、clip/w、深度、法线 | PBR、阴影、IBL |
| 画面稳定与清晰 | 像素采样、抗锯齿、线性空间运算 | MSAA/TAA、时域历史资源 |
| 性能优化 | 工作量、带宽、缓存、overdraw、测量 | GPU 架构、subgroup、tile-based rendering |

先做[投影实验](examples/projection/README.md)或继续[逐行读 shader](02_shader_dataflow.md)。不需要等数学全部学完才允许自己写下一行 Vulkan。
