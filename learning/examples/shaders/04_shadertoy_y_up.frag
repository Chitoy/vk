#version 450

layout(location = 0) in vec2 vUV;
layout(location = 0) out vec4 outColor;

layout(push_constant, std430) uniform PushConstants {
    layout(offset = 0) vec4 timeResolution;
    layout(offset = 16) vec4 mouse;
} pc;

#define iTime (pc.timeResolution.x)
#define iResolution vec3(pc.timeResolution.yz, 1.0)
// XY only. ZW remain zero: button/click-origin state is NOT supplied by the host.
#define iMouse vec4(pc.mouse.xy * pc.timeResolution.yz, 0.0, 0.0)

void mainImage(out vec4 fragColor, in vec2 fragCoord) {
    vec2 uv = fragCoord / iResolution.xy;
    vec2 p = (2.0 * fragCoord - iResolution.xy) / iResolution.y;
    float d = abs(length(p) - (0.45 + 0.04 * sin(iTime * 2.0)));
    float aa = max(fwidth(d), 2.0 / max(iResolution.y, 1.0));
    float ring = 1.0 - smoothstep(0.008, 0.008 + aa, d);

    // Background green increases UPWARD, visibly testing the Y conversion.
    vec3 color = vec3(0.08 * uv.x, 0.15 * uv.y, 0.015);
    color = mix(color, vec3(0.1, 0.75, 1.0), ring);

    // Existing CPU mouse normalization assumes matching window/framebuffer sizes.
    // With that assumption, this marker follows the actual cursor in both axes.
    float cursorDistance = length(fragCoord - iMouse.xy);
    float cursor = 1.0 - smoothstep(5.0, 7.0, cursorDistance);
    color = mix(color, vec3(1.0, 0.35, 0.05), cursor);
    fragColor = vec4(color, 1.0);
}

void main() {
    // Current fullscreen.vert + positive viewport height -> input UV Y-down.
    vec2 fragCoordYUp = vec2(vUV.x, 1.0 - vUV.y) * pc.timeResolution.yz;
    mainImage(outColor, fragCoordYUp);
}
