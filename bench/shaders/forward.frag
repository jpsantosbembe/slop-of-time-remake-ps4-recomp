#version 450
#extension GL_EXT_nonuniform_qualifier : require
// S1: uma textura por draw, vinda de um array bindless (o OoT tem ~3.600 texturas no pool), e luz simples.
layout(location = 0) in vec3 v_nrm;
layout(location = 1) in vec2 v_uv;
layout(location = 2) flat in uint v_tex;

layout(set = 0, binding = 1) uniform sampler smp;
layout(set = 0, binding = 2) uniform texture2D tex[];

layout(location = 0) out vec4 o_col;

void main()
{
    vec4 a = texture(sampler2D(tex[nonuniformEXT(v_tex)], smp), v_uv);
    float l = max(dot(normalize(v_nrm), normalize(vec3(0.4, 0.8, 0.3))), 0.15);
    o_col = vec4(a.rgb * l, 1.0);
}
