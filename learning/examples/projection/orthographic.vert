#version 450

layout(push_constant) uniform PushConstants {
    vec4 timeResolution;
    vec4 mouse;
} pc;

layout(location = 0) out vec2 perspectiveUV;
layout(location = 1) noperspective out vec2 screenLinearUV;

const vec3 POSITIONS[3] = vec3[](
    vec3(-1.0, -0.7, -2.0),
    vec3( 1.0, -0.7, -2.0),
    vec3( 0.0,  1.5, -5.0)
);
const vec2 UVS[3] = vec2[](
    vec2(0.0, 0.0), vec2(1.0, 0.0), vec2(0.5, 1.0)
);

void main() {
    vec3 p = POSITIONS[gl_VertexIndex];
    float nearPlane = 0.1;
    float farPlane = 10.0;
    float halfHeight = 2.0;
    float aspect = pc.timeResolution.w;

    gl_Position = vec4(
        p.x / (aspect * halfHeight),
        -p.y / halfHeight,
        (-p.z - nearPlane) / (farPlane - nearPlane),
        1.0
    );
    perspectiveUV = UVS[gl_VertexIndex];
    screenLinearUV = UVS[gl_VertexIndex];
}
