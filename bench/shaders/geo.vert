#version 450
// Geometria das cenas S1 e S2: posição, normal e UV; matriz por draw lida de um buffer (o jogo sobe constantes por draw).
layout(location = 0) in vec3 in_pos;
layout(location = 1) in vec3 in_nrm;
layout(location = 2) in vec2 in_uv;

layout(push_constant) uniform PC { uint draw_id; uint tex_id; uint pad0; uint pad1; } pc;
layout(std430, set = 0, binding = 0) readonly buffer Draws { mat4 mvp[]; } draws;

layout(location = 0) out vec3 v_nrm;
layout(location = 1) out vec2 v_uv;
layout(location = 2) flat out uint v_tex;

void main()
{
    mat4 m = draws.mvp[pc.draw_id];
    gl_Position = m * vec4(in_pos, 1.0);
    v_nrm = mat3(m) * in_nrm;
    v_uv = in_uv;
    v_tex = pc.tex_id;
}
