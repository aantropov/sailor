{
"includes": [] ,
"glslCommon" :
BEGIN_CODE
#version 460
#extension GL_ARB_separate_shader_objects : enable
END_CODE,

"glslCompute":
BEGIN_CODE

layout(local_size_x = 1, local_size_y = 1) in;
 
void main() 
{
  vec4 pixel = vec4(0.0, 0.0, 0.0, 1.0);
  ivec2 pixel_coords = ivec2(gl_GlobalInvocationID.xy);
   //imageStore(img_output, pixel_coords, pixel);
}

END_CODE,

"defines":[]
}