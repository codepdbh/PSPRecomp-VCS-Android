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
    // Positions arrive already in PSP screen space, but carry their clip W.
    // Handing W back to the rasterizer (NDC * w, w) is what makes it
    // interpolate UV/Q/colour perspective-correctly - with W fixed at 1 the
    // ground textures were mapped affinely and warped as the camera moved.
    // Same mapping as screen_to_d3d() in the DX12 backend, minus its Y flip.
    float w = in_position.w;
    if (!(abs(w) >= 1.0e-12)) w = 1.0;
    vec2 ndc = in_position.xy * draw.scale_offset.xy + draw.scale_offset.zw;
    gl_Position = vec4(ndc * w, clamp(in_position.z / 65535.0, 0.0, 1.0) * w, w);
    vertex_color = in_color;
    vertex_uv = in_uv;
    vertex_q = in_q;
    vertex_fog = in_fog;
}
