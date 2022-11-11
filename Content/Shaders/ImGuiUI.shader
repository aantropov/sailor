---
glslCommon: |
  #version 450 core
  #extension GL_ARB_separate_shader_objects : enable
   
glslVertex: |
  layout(location = 0) in vec2 aPos;
  layout(location = 2) in vec2 aUV;
  layout(location = 3) in vec4 aColor;
  layout(push_constant) uniform uPushConstant { vec2 uScale; vec2 uTranslate; } pc;
  
  out gl_PerVertex { vec4 gl_Position; };
  layout(location = 0) out vec4 outColor; 
  layout(location = 1) out vec2 outUV;
  
  void main()
  {
  	  outColor = aColor;
  	  outUV = aUV;
  	  gl_Position = vec4(aPos * pc.uScale + pc.uTranslate, 0, 1);
  }

glslFragment: |
  layout(location = 0) out vec4 fColor;
  layout(set=0, binding=0) uniform sampler2D sTexture;
  layout(location = 0) in vec4 inColor; 
  layout(location = 1) in vec2 inUV;
  void main()
  {
  	  fColor = inColor * texture(sTexture, inUV.st);
  }
