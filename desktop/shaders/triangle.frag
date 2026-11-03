#version 460
// Pass: host sanity triangle fragment stage - writes the interpolated vertex color.

layout(location = 0) in vec3 v_color;
layout(location = 0) out vec4 o_color;

void main() {
    o_color = vec4(v_color, 1.0);
}
