#version 450

layout(location = 0) in vec2 vUV;
layout(location = 0) out vec4 outColor;

layout(push_constant, std430) uniform PushConstants {
    layout(offset = 0) vec4 timeResolution;
    layout(offset = 16) vec4 mouse;
} pc;

void main() {
    vec2 p = vUV * 2.0 - 1.0;
    p.x *= pc.timeResolution.w; // Equal coordinate distance per screen pixel.

    float radius = 0.45;
    float signedDistance = length(p) - radius;
    float distanceToRing = abs(signedDistance);

    // fwidth estimates how rapidly this value changes across nearby fragments.
    // Both edges of smoothstep are ordered, and aa is strictly positive.
    float aa = max(fwidth(signedDistance), 2.0 / max(pc.timeResolution.z, 1.0));
    float halfWidth = 0.008;
    float coverage = 1.0 - smoothstep(halfWidth, halfWidth + aa, distanceToRing);

    vec3 background = vec3(0.006, 0.009, 0.025);
    vec3 ringColor = vec3(0.1, 0.75, 1.0);
    outColor = vec4(mix(background, ringColor, coverage), 1.0);

    // Exercise A: radius = 0.45 + 0.04 * sin(pc.timeResolution.x * 2.0);
    // Change the radius definition above to make the ring breathe.
    // Exercise B: replace the output above with the next line to show a disk.
    // outColor = vec4(vec3(1.0 - smoothstep(-aa, aa, signedDistance)), 1.0);
}
