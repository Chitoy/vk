#version 450

// Full replacement vertex shader. See README for the TWO host stage-mask edits.
layout(push_constant) uniform PushConstants {
    vec4 timeResolution;
    vec4 mouse;
} pc;

layout(location = 0) out vec2 perspectiveUV;
layout(location = 1) noperspective out vec2 screenLinearUV;

// Positions already in view space: right-handed, camera looking toward -Z.
const vec3 POSITIONS[3] = vec3[](
    vec3(-1.0, -0.7, -2.0),
    vec3( 1.0, -0.7, -2.0),
    vec3( 0.0,  1.5, -5.0)
);
const vec2 UVS[3] = vec2[](
    vec2(0.0, 0.0), vec2(1.0, 0.0), vec2(0.5, 1.0)
);

void main() {
    vec3 p = POSITIONS[gl_VertexIndex]; // Host must keep draw(3, 1, 0, 0).
    float nearPlane = 0.1;
    float farPlane = 10.0;
    float scale = 1.0 / tan(radians(60.0) * 0.5);
    float aspect = pc.timeResolution.w;

    // Vulkan default depth: NDC z in [0,1]. Flip Y here exactly once.
    // Keep w=-z for clipping and perspective-correct interpolation.
    gl_Position = vec4(
        scale * p.x / aspect,
        -scale * p.y,
        (farPlane * p.z + farPlane * nearPlane) / (nearPlane - farPlane),
        -p.z
    );
    perspectiveUV = UVS[gl_VertexIndex];
    screenLinearUV = UVS[gl_VertexIndex];
}
