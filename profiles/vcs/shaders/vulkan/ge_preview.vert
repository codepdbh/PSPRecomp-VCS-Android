#version 450

layout(location = 0) in vec4 in_position;
layout(location = 1) in vec4 in_color;
layout(location = 2) in vec2 in_uv;
layout(location = 3) in float in_q;
layout(location = 4) in float in_fog;
layout(location = 0) out vec4 vertex_color;
layout(location = 1) out vec2 vertex_uv;
layout(location = 2) out float vertex_q;
layout(location = 3) out float vertex_fog;

layout(push_constant) uniform DrawScale {
    vec4 scale_offset;
} draw;

void main() {
    gl_Position = vec4(in_position.xy * draw.scale_offset.xy +
                       draw.scale_offset.zw,
                       clamp(in_position.z / 65535.0, 0.0, 1.0), 1.0);
    vertex_color = in_color;
    vertex_uv = in_uv;
    vertex_q = in_q;
    vertex_fog = in_fog;
}
