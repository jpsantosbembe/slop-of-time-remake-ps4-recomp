#version 450
#extension GL_EXT_nonuniform_qualifier : require
// S2: G-buffer com 4 render targets e três texturas por material (cor, normal, parâmetros), como um deferred do agl.
layout(location = 0) in vec3 v_nrm;
layout(location = 1) in vec2 v_uv;
layout(location = 2) flat in uint v_tex;

layout(set = 0, binding = 1) uniform sampler smp;
layout(set = 0, binding = 2) uniform texture2D tex[];

layout(location = 0) out vec4 o_albedo;
layout(location = 1) out vec4 o_normal;
layout(location = 2) out vec4 o_params;
layout(location = 3) out vec2 o_emis;

void main()
{
    vec4 c = texture(sampler2D(tex[nonuniformEXT(v_tex)], smp), v_uv);
    vec4 n = texture(sampler2D(tex[nonuniformEXT(v_tex + 1u)], smp), v_uv * 2.0);
    vec4 p = texture(sampler2D(tex[nonuniformEXT(v_tex + 2u)], smp), v_uv * 0.5);
    vec3 nn = normalize(v_nrm + (n.xyz * 2.0 - 1.0) * 0.3);
    o_albedo = vec4(c.rgb, 1.0);
    o_normal = vec4(nn * 0.5 + 0.5, p.a);
    o_params = p;
    o_emis = vec2(c.a * p.r, n.a);
}
