#version 450

layout(location = 0) in vec2 vUV;
layout(location = 0) out vec4 outColor;

// Same 32-byte interface as src/main.cpp; unused members are intentional.
layout(push_constant, std430) uniform PushConstants {
    layout(offset = 0) vec4 timeResolution;
    layout(offset = 16) vec4 mouse;
} pc;

void main() {
    // With the project's positive viewport height, green increases DOWNWARD.
    // Top-left: black; top-right: red; bottom-left: green; bottom-right: yellow.
    outColor = vec4(vUV.x, vUV.y, 0.0, 1.0);

    // Exercise: replace the line above with this to see CPU time reach the GPU.
    // outColor = vec4(vUV, 0.5 + 0.5 * sin(pc.timeResolution.x), 1.0);
}
