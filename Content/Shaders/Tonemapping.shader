--- 
includes:
- Shaders/Constants.glsl
- Shaders/Formats.glsl

defines:
- ACES
- UNCHARTED2
- LUMINANCE

glslCommon: |
  #version 460
  #extension GL_ARB_separate_shader_objects : enable

glslVertex: |
  layout(location=DefaultPositionBinding) in vec3 inPosition;
  layout(location=DefaultTexcoordBinding) in vec2 inTexcoord;
  
  layout(location=0) out vec2 fragTexcoord;
  
  void main() 
  {
      gl_Position = vec4(inPosition, 1);
      fragTexcoord = inTexcoord;
  }
  
glslFragment: |
  layout(set = 0, binding = 0) uniform FrameData
  {
      mat4 view;
      mat4 projection;
      mat4 invProjection;
      vec4 cameraPosition;
      ivec2 viewportSize;
      vec2 cameraZNearZFar;
      float currentTime;
      float deltaTime;
  } frame;
  
  layout(set = 0, binding = 1) uniform PreviousFrameData
  {
      mat4 view;
      mat4 projection;
      mat4 invProjection;
      vec4 cameraPosition;
      ivec2 viewportSize;
      vec2 cameraZNearZFar;
      float currentTime;
      float deltaTime;
  } previousFrame;
  
  layout(set=1, binding=0) uniform PostProcessDataUBO
  {
    vec4 whitePoint;
    vec4 exposure;
  } data;
  
  layout(set=1, binding=1) uniform sampler2D colorSampler;
  layout(set=1, binding=2) uniform sampler2D averageLuminanceSampler;
  
  layout(location=0) in vec2 fragTexcoord;
  layout(location=0) out vec4 outColor;
  
  #ifdef ACES
  //
  // This code is modified from 'Baking Lab' by MJP and David Neubelt (licensed under the MIT license):
  // https://github.com/TheRealMJP/BakingLab/blob/master/BakingLab/ACES.hlsl
  //
  // which states
  //
  // "The code in this file was originally written by Stephen Hill (@self_shadow), who deserves all
  // credit for coming up with this fit and implementing it. Buy him a beer next time you see him. :)"
  //
  
  // sRGB => XYZ => D65_2_D60 => AP1 => RRT_SAT
  const mat3 ACES_input_matrix = mat3(
      0.59719, 0.07600, 0.02840,
      0.35458, 0.90834, 0.13383,
      0.04823, 0.01566, 0.83777
  );
  
  // ODT_SAT => XYZ => D60_2_D65 => sRGB
  const mat3 ACES_output_matrix = mat3(
      1.60475, -0.10208, -0.00327,
      -0.53108, 1.10813, -0.07276,
      -0.07367, -0.00605, 1.07602
  );
  
  vec3 ACES_RRTAndODTFit(vec3 v)
  {
      vec3 a = v * (v + 0.0245786) - 0.000090537;
      vec3 b = v * (0.983729 * v + 0.4329510) + 0.238081;
      return a / b;
  }
  
  vec3 ACES_tonemap(vec3 color)
  {
      color = ACES_input_matrix * color;
  
      // Apply RRT and ODT
      color = ACES_RRTAndODTFit(color);
  
      color = ACES_output_matrix * color;
  
      // Clamp to [0, 1]
      color = clamp(color, vec3(0.0), vec3(1.0));
  
      return color;
  }
  #endif //ACES
  
  #ifdef UNCHARTED2
  // This code is taken from http://filmicworlds.com/blog/filmic-tonemapping-with-piecewise-power-curves/
  
  vec3 uncharted2_tonemap_partial(vec3 x)
  {
      float A = 0.15f;
      float B = 0.50f;
      float C = 0.10f;
      float D = 0.20f;
      float E = 0.02f;
      float F = 0.30f;
      return ((x*(A*x+C*B)+D*E)/(x*(A*x+B)+D*F))-E/F;
  }
  
  vec3 uncharted2_filmic(vec3 v, vec3 whitePoint, float exposure)
  {
      vec3 curr = uncharted2_tonemap_partial(v * exposure);
      vec3 white_scale = vec3(1.0f) / uncharted2_tonemap_partial(whitePoint);
      return curr * white_scale;
  }

  #endif //UNCHARTED2

  void main() 
  {
      outColor = texture(colorSampler, fragTexcoord);
      float avgLum = texture(averageLuminanceSampler, vec2(0,0)).x;
      
      vec3 color = outColor.xyz / (9.6 * avgLum + 0.0001);
      
  #if defined(LUMINANCE)
      // Yxy.x is Y, the luminance
      vec3 Yxy = convertRGB2Yxy(outColor.xyz);
      float lp = Yxy.x / (9.6 * avgLum + 0.0001);
      
      color = vec3(lp);
  #endif

  #if defined(ACES)
      color = ACES_tonemap(color);
  #elif defined(UNCHARTED2)
      color = uncharted2_filmic(color, data.whitePoint.xyz, data.exposure.x);
  #endif 
  
  #if defined(LUMINANCE)
      outColor.xyz = convertYxy2RGB(vec3(color.x, Yxy.y, Yxy.z));
  #else
      outColor.xyz = color;
  #endif
  }
