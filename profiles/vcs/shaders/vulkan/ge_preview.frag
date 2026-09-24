#version 450

layout(location = 0) in vec4 vertex_color;
layout(location = 3) in float vertex_fog;
layout(location = 0) out vec4 fragment_color;

layout(push_constant) uniform AlphaState {
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
    if (state.alpha.x != 0u) {
        uint value = uint(floor(clamp(vertex_color.a, 0.0, 1.0) * 255.0 + 0.5));
        if (!alpha_pass(state.alpha.y, value & state.alpha.w,
                        state.alpha.z & state.alpha.w)) discard;
    }
    vec4 result = vertex_color;
    if (state.fog.a != 0.0)
        result.rgb = mix(state.fog.rgb, result.rgb, clamp(vertex_fog, 0.0, 1.0));
    fragment_color = result;
}
