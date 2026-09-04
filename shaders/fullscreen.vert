#version 450

layout(location = 0) out vec2 vUV;

// A single oversized triangle covers the screen. No vertex buffer is needed.
const vec2 POSITIONS[3] = vec2[](
    vec2(-1.0, -1.0),
    vec2( 3.0, -1.0),
    vec2(-1.0,  3.0)
);

void main() {
    vec2 position = POSITIONS[gl_VertexIndex];
    gl_Position = vec4(position, 0.0, 1.0);
    vUV = position * 0.5 + 0.5;
}

