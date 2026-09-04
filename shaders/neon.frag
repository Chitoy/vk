#version 450

layout(location = 0) in vec2 vUV;
layout(location = 0) out vec4 outColor;

layout(push_constant) uniform PushConstants {
    // x = time, y = width, z = height, w = aspect ratio
    vec4 timeResolution;
    // xy = normalized mouse position
    vec4 mouse;
} pc;

mat2 rotate2D(float angle) {
    float s = sin(angle);
    float c = cos(angle);
    return mat2(c, -s, s, c);
}

float glow(float distanceToLine, float radius, float intensity) {
    return pow(radius / max(distanceToLine, 0.0008), intensity);
}

void main() {
    float time = pc.timeResolution.x;
    float aspect = pc.timeResolution.w;

    vec2 p = vUV * 2.0 - 1.0;
    p.x *= aspect;
    p += (pc.mouse.xy - 0.5) * 0.16;

    vec3 color = vec3(0.006, 0.009, 0.025);

    // Domain-warped polar rings: deliberately compact so it is easy to edit.
    for (int i = 0; i < 6; ++i) {
        float fi = float(i);
        vec2 q = rotate2D(time * (0.08 + fi * 0.017)) * p;
        q += 0.10 * vec2(
            sin(q.y * 3.0 + time + fi),
            cos(q.x * 2.6 - time * 0.8 + fi)
        );

        float radius = 0.20 + fi * 0.12;
        float ring = abs(length(q) - radius);
        float wave = sin(atan(q.y, q.x) * (5.0 + fi) - time * (1.2 + fi * 0.08));
        ring = abs(ring + wave * 0.018);

        vec3 tint = 0.5 + 0.5 * cos(vec3(0.0, 2.1, 4.2) + fi * 0.7 + time * 0.35);
        color += tint * glow(ring, 0.006, 1.22) * 0.085;
    }

    float vignette = smoothstep(1.45, 0.20, length(p));
    color *= vignette;
    color = 1.0 - exp(-color * 1.5); // simple tone mapping

    outColor = vec4(color, 1.0);
}
