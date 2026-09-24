#version 450

layout(set = 0, binding = 0) uniform sampler2D image_texture;
layout(location = 0) in vec4 vertex_color;
layout(location = 1) in vec2 vertex_uv;
layout(location = 2) in float vertex_q;
layout(location = 3) in float vertex_fog;
layout(location = 0) out vec4 fragment_color;

layout(push_constant) uniform TextureState {
    layout(offset = 16) uvec4 control;
    layout(offset = 32) vec4 environment;
    layout(offset = 48) uvec4 alpha;
    layout(offset = 64) vec4 fog;
} state;

bool alpha_pass(uint function, uint value, uint reference) {
    switch (function & 7u) {
    case 0u: return false;
    case 1u: return true;
    case 2u: return value == reference;
    case 3u: return value != reference;
    case 4u: return value < reference;
    case 5u: return value <= reference;
    case 6u: return value > reference;
    default: return value >= reference;
    }
}

void main() {
    float q = abs(vertex_q) < 1.0e-20 ? 1.0 : vertex_q;
    vec4 texel = texture(image_texture, vertex_uv / q);
    bool use_alpha = state.control.y != 0u;
    vec4 result;
    switch (state.control.x) {
    case 0u: // MODULATE
        result = vec4(vertex_color.rgb * texel.rgb,
                      use_alpha ? vertex_color.a * texel.a : vertex_color.a);
        break;
    case 1u: // DECAL
        result = vec4(mix(vertex_color.rgb, texel.rgb,
                          use_alpha ? texel.a : 1.0), vertex_color.a);
        break;
    case 2u: // BLEND
        result = vec4(mix(vertex_color.rgb, state.environment.rgb, texel.rgb),
                      use_alpha ? vertex_color.a * texel.a : vertex_color.a);
        break;
    case 3u: // REPLACE
        result = vec4(texel.rgb, use_alpha ? texel.a : vertex_color.a);
        break;
    case 4u: // ADD
        result = vec4(min(vec3(1.0), vertex_color.rgb + texel.rgb),
                      use_alpha ? vertex_color.a * texel.a : vertex_color.a);
        break;
    default:
        result = texel;
    }
    if (state.control.z != 0u) result.rgb = min(vec3(1.0), result.rgb * 2.0);
    if (state.fog.a != 0.0)
        result.rgb = mix(state.fog.rgb, result.rgb, clamp(vertex_fog, 0.0, 1.0));
    if (state.alpha.x != 0u) {
        uint value = uint(floor(clamp(result.a, 0.0, 1.0) * 255.0 + 0.5));
        if (!alpha_pass(state.alpha.y, value & state.alpha.w,
                        state.alpha.z & state.alpha.w)) discard;
    }
    fragment_color = result;
}
