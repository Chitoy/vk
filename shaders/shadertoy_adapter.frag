#version 450

// Copy a LICENSE-COMPATIBLE ShaderToy mainImage() below, then compile this file.
// Multi-pass shaders and iChannel textures need extra Vulkan images/descriptors.

layout(location = 0) in vec2 vUV;
layout(location = 0) out vec4 outColor;

layout(push_constant) uniform PushConstants {
    vec4 timeResolution;
    vec4 mouse;
} pc;

#define iTime       (pc.timeResolution.x)
#define iResolution vec3(pc.timeResolution.yz, 1.0)
#define iMouse      vec4(pc.mouse.xy * pc.timeResolution.yz, 0.0, 0.0)

// Replace this demo body with the source shader's mainImage implementation.
void mainImage(out vec4 fragColor, in vec2 fragCoord) {
    vec2 uv = (2.0 * fragCoord - iResolution.xy) / iResolution.y;
    float d = length(uv) - 0.45 - 0.04 * sin(iTime * 2.0);
    float edge = 0.012 / max(abs(d), 0.001);
    fragColor = vec4(edge * vec3(0.2, 0.8, 1.0), 1.0);
}

void main() {
    mainImage(outColor, vUV * pc.timeResolution.yz);
}

