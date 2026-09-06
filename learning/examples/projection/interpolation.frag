#version 450

layout(location = 0) in vec2 perspectiveUV;
layout(location = 1) noperspective in vec2 screenLinearUV;
layout(location = 0) out vec4 outColor;

layout(push_constant) uniform PushConstants {
    vec4 timeResolution;
    vec4 mouse;
} pc;

// 0: whole triangle uses perspective-correct interpolation.
// 1: whole triangle uses screen-linear interpolation.
// 2: left/right comparison with a thin separator (default).
const int MODE = 2;

void main() {
    bool useScreenLinear = MODE == 1 ||
        (MODE == 2 && gl_FragCoord.x >= pc.timeResolution.y * 0.5);
    vec2 uv = useScreenLinear ? screenLinearUV : perspectiveUV;
    vec2 tile = floor(uv * 12.0);
    float checker = mod(tile.x + tile.y, 2.0);
    vec3 color = mix(vec3(0.035, 0.06, 0.10), vec3(0.12, 0.75, 0.90), checker);
    if (MODE == 2 && abs(gl_FragCoord.x - pc.timeResolution.y * 0.5) < 1.0) {
        color = vec3(1.0, 0.25, 0.04);
    }
    outColor = vec4(color, 1.0);
}
