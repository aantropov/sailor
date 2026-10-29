---
includes:
- Shaders/Constants.glsl

colorAttachments:
- R16G16B16A16_SFLOAT

glslCommon: |
  #version 460
  #extension GL_ARB_separate_shader_objects : enable

glslVertex: |
  layout(location=DefaultPositionBinding) in vec3 inPosition;
  void main()
  {
      gl_Position = vec4(inPosition, 1.0);
  }

glslFragment: |
  layout(set=1, binding=0) uniform sampler2D colorSampler;
  layout(location=0) out vec4 outColor;
  void main()
  {
      // Keep normal scene winding while converting camera images to cube addressing.
      ivec2 size = textureSize(colorSampler, 0);
      ivec2 pixel = ivec2(gl_FragCoord.xy);
      pixel.x = size.x - 1 - pixel.x;
      outColor = texelFetch(colorSampler, pixel, 0);
  }
