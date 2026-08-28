$input a_position, a_color0
$output v_color0

#include <bgfx_shader.sh>

void main()
{
    vec3 blockPos = floor(a_position * 255.0 + 0.5);
    gl_Position = mul(u_modelViewProj, vec4(blockPos, 1.0));
    v_color0 = a_color0;
}
