#version 450
// S2: passe de luz do deferred. Lê os 4 alvos e a profundidade e soma uma luz direcional e 32 pontuais.
layout(location = 0) in vec2 v_uv;
layout(set = 0, binding = 0) uniform sampler smp;
layout(set = 0, binding = 1) uniform texture2D g_albedo;
layout(set = 0, binding = 2) uniform texture2D g_normal;
layout(set = 0, binding = 3) uniform texture2D g_params;
layout(set = 0, binding = 4) uniform texture2D g_emis;
layout(set = 0, binding = 5) uniform texture2D g_depth;
layout(location = 0) out vec4 o_hdr;

void main()
{
    vec3 a = texture(sampler2D(g_albedo, smp), v_uv).rgb;
    vec3 n = texture(sampler2D(g_normal, smp), v_uv).xyz * 2.0 - 1.0;
    vec4 p = texture(sampler2D(g_params, smp), v_uv);
    vec2 e = texture(sampler2D(g_emis, smp), v_uv).xy;
    float z = texture(sampler2D(g_depth, smp), v_uv).r;
    vec3 pos = vec3(v_uv * 2.0 - 1.0, z);
    vec3 col = a * max(dot(n, normalize(vec3(0.4, 0.8, 0.3))), 0.0);
    for (int i = 0; i < 32; i++) {
        float f = float(i);
        vec3 lp = vec3(sin(f * 1.7), cos(f * 2.3), fract(f * 0.37));
        vec3 d = lp - pos;
        float att = 1.0 / (1.0 + dot(d, d) * 8.0);
        col += a * max(dot(n, normalize(d)), 0.0) * att * vec3(fract(f * 0.13), fract(f * 0.71), fract(f * 0.29));
    }
    col = col * (1.0 - p.g * 0.5) + e.x * 2.0;
    o_hdr = vec4(col, 1.0);
}
