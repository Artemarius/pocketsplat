#version 460
// Pass: host sanity triangle. Inputs: none (positions from gl_VertexIndex). Outputs: interpolated
// color to the fragment stage. Proves the build-time SPIR-V embedding and the graphics pipeline.

layout(location = 0) out vec3 v_color;

void main() {
    const vec2 positions[3] = vec2[](vec2(0.0, -0.6), vec2(0.6, 0.6), vec2(-0.6, 0.6));
    const vec3 colors[3] = vec3[](vec3(1.0, 0.2, 0.2), vec3(0.2, 1.0, 0.2), vec3(0.2, 0.4, 1.0));
    gl_Position = vec4(positions[gl_VertexIndex], 0.0, 1.0);
    v_color = colors[gl_VertexIndex];
}
