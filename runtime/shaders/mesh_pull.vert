#version 450
// Mesh shader emulation (docs/frente-M.md): the vertex stage of the indexed draw after the mesh compute. Reads vertex
// gl_VertexIndex from the vertices the compute wrote (set 3 binding 0) as the control block (binding 2) lays it out:
// Position at float 0, Location i at c[17 + i] or (0, 0, 0, 1) when the mesh shader does not write it. A plain copy,
// so the position is the compute's bit for bit (the depth-EQUAL passes of foliage depend on it).
layout(set = 3, binding = 0, std430) readonly buffer Vtx { float v[]; };
layout(set = 3, binding = 2, std430) readonly buffer Ctl { uint c[]; };
invariant gl_Position;
layout(location = 0) out vec4 o0;
layout(location = 1) out vec4 o1;
layout(location = 2) out vec4 o2;
layout(location = 3) out vec4 o3;
layout(location = 4) out vec4 o4;
layout(location = 5) out vec4 o5;
layout(location = 6) out vec4 o6;
layout(location = 7) out vec4 o7;
layout(location = 8) out vec4 o8;
layout(location = 9) out vec4 o9;
layout(location = 10) out vec4 o10;
layout(location = 11) out vec4 o11;
layout(location = 12) out vec4 o12;
layout(location = 13) out vec4 o13;
layout(location = 14) out vec4 o14;
layout(location = 15) out vec4 o15;
layout(location = 16) out vec4 o16;
layout(location = 17) out vec4 o17;
layout(location = 18) out vec4 o18;
layout(location = 19) out vec4 o19;
layout(location = 20) out vec4 o20;
layout(location = 21) out vec4 o21;
layout(location = 22) out vec4 o22;
layout(location = 23) out vec4 o23;
layout(location = 24) out vec4 o24;
layout(location = 25) out vec4 o25;
layout(location = 26) out vec4 o26;
layout(location = 27) out vec4 o27;
vec4 at(uint b, uint off) {
    if (off == 0xffffffffu) return vec4(0.0, 0.0, 0.0, 1.0);
    return vec4(v[b + off], v[b + off + 1u], v[b + off + 2u], v[b + off + 3u]);
}
void main() {
    uint b = uint(gl_VertexIndex) * c[16];
    gl_Position = at(b, 0u);
    o0 = at(b, c[17]);
    o1 = at(b, c[18]);
    o2 = at(b, c[19]);
    o3 = at(b, c[20]);
    o4 = at(b, c[21]);
    o5 = at(b, c[22]);
    o6 = at(b, c[23]);
    o7 = at(b, c[24]);
    o8 = at(b, c[25]);
    o9 = at(b, c[26]);
    o10 = at(b, c[27]);
    o11 = at(b, c[28]);
    o12 = at(b, c[29]);
    o13 = at(b, c[30]);
    o14 = at(b, c[31]);
    o15 = at(b, c[32]);
    o16 = at(b, c[33]);
    o17 = at(b, c[34]);
    o18 = at(b, c[35]);
    o19 = at(b, c[36]);
    o20 = at(b, c[37]);
    o21 = at(b, c[38]);
    o22 = at(b, c[39]);
    o23 = at(b, c[40]);
    o24 = at(b, c[41]);
    o25 = at(b, c[42]);
    o26 = at(b, c[43]);
    o27 = at(b, c[44]);
}
