{
"includes": [] ,
"glslCommon" :
BEGIN_CODE
#version 460
#extension GL_ARB_separate_shader_objects : enable
END_CODE,

"glslCompute":
BEGIN_CODE

layout (local_size_x = 16, local_size_y = 16, local_size_z = 1) in;
layout (set = 0, binding = 0, r32f) writeonly uniform image2D destTex;

void main() 
{
  float pixel = 1;
  ivec2 coords = ivec2(gl_GlobalInvocationID.xy);
  
  if(1 == coords.x % 2 || 1 == coords.y % 2)
  {
	pixel = 0;
  }
  
  imageStore(destTex, coords, vec4(pixel));
}

END_CODE,

"defines":[]
}