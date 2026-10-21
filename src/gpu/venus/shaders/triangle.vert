#version 450
layout(location = 0) out vec3 color;
void main() {
    const vec2 positions[3] = vec2[3](vec2(-0.75, -0.75), vec2(0.75, -0.75), vec2(0.0, 0.75));
    const vec3 colors[3] = vec3[3](vec3(1.0, 0.0, 0.0), vec3(0.0, 1.0, 0.0), vec3(0.0, 0.0, 1.0));
    gl_Position = vec4(positions[gl_VertexIndex], 0.0, 1.0);
    color = colors[gl_VertexIndex];
}
