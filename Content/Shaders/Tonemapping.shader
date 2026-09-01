--- 
includes:
- Shaders/Constants.glsl
- Shaders/Formats.glsl

defines:
- ACES
- AGX
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
    vec4 exposureCompensation;
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

  float interleavedGradientNoise(vec2 pixel)
  {
      return fract(
        52.9829189f * fract(dot(
          pixel,
          vec2(0.06711056f, 0.00583715f))));
  }

  #ifdef AGX
  // AgX maps a wide scene-linear range into SDR while keeping saturated
  // highlights inside the display gamut. The constants and default contrast
  // approximation follow the reference implementation used by Filament.
  const mat3 AGX_inset_matrix = mat3(
      0.842479062253094, 0.0423282422610123, 0.0423756549057051,
      0.0784335999999992, 0.878468636469772, 0.0784336,
      0.0792237451477643, 0.0791661274605434, 0.879142973793104
  );

  const mat3 AGX_outset_matrix = mat3(
      1.19687900512017, -0.0528968517574562, -0.0529716355144438,
      -0.0980208811401368, 1.15190312990417, -0.0980434501171241,
      -0.0990297440797205, -0.0989611768448433, 1.15107367264116
  );

  vec3 AGX_default_contrast(vec3 x)
  {
      vec3 x2 = x * x;
      vec3 x4 = x2 * x2;
      vec3 x6 = x4 * x2;
      return -17.86f * x6 * x +
        78.01f * x6 -
        126.7f * x4 * x +
        92.06f * x4 -
        28.72f * x2 * x +
        4.361f * x2 -
        0.1718f * x +
        0.002857f;
  }

  vec3 AGX_tonemap(vec3 color, float displayDither)
  {
      const float minEV = -12.47393f;
      const float maxEV = 4.026069f;

      color = AGX_inset_matrix * max(color, vec3(0.0f));
      color = log2(max(color, vec3(0.0000000001f)));
      color = clamp((color - minEV) / (maxEV - minEV), 0.0f, 1.0f);
      color = AGX_default_contrast(color);
      color = AGX_outset_matrix * color;

      // Dither in AgX display space before the final 8-bit conversion. This
      // reuses the existing display-to-linear power operation below.
      color = clamp(
        color + vec3(displayDither),
        vec3(0.0f),
        vec3(1.0f));

      // The AgX outset is display encoded. Return to linear Rec.709 because
      // Sailor performs the final sRGB conversion when writing the viewport.
      return pow(max(color, vec3(0.0f)), vec3(2.2f));
  }
  #endif //AGX
  
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
  
  vec3 uncharted2_filmic(vec3 v, vec3 whitePoint)
  {
      vec3 curr = uncharted2_tonemap_partial(v);
      if(any(isnan(whitePoint)) || any(isinf(whitePoint)))
      {
          whitePoint = vec3(11.2f);
      }
      vec3 white_scale = vec3(1.0f) / max(
        uncharted2_tonemap_partial(max(whitePoint, vec3(0.001f))),
        vec3(0.000001f));
      return curr * white_scale;
  }

  #endif //UNCHARTED2

  void main() 
  {
      outColor = texture(colorSampler, fragTexcoord);
      vec3 hdrColor = outColor.xyz;
      hdrColor = mix(hdrColor, vec3(0.0f), isnan(hdrColor));
      hdrColor = mix(hdrColor, vec3(0.0f), isinf(hdrColor));
      hdrColor = min(max(hdrColor, vec3(0.0f)), vec3(65504.0f));

      float avgLum = texture(averageLuminanceSampler, vec2(0,0)).x;
      if(isnan(avgLum) || isinf(avgLum) || avgLum <= 0.0f)
      {
          avgLum = 1.0f;
      }
      // Exposure compensation is expressed in EV stops: +1 doubles exposure,
      // -1 halves it. Zero keeps the metered exposure unchanged.
      float exposureEV = data.exposureCompensation.x;
      if(isnan(exposureEV) || isinf(exposureEV))
      {
          exposureEV = 0.0f;
      }
      const float exposureScale = exp2(clamp(exposureEV, -16.0f, 16.0f));
      // The meter uses EV100 = log2(8 * L). The matching photographic camera
      // exposure is 1 / (1.2 * 2^EV100), so a metered luminance L must be
      // scaled by 1 / (9.6 * L). Using 18% gray here applies a second,
      // incompatible calibration and overexposes the frame by about 0.8 EV.
      const float photographicExposureKey = 1.0f / 9.6f;
      const float meteredExposure =
        photographicExposureKey / max(avgLum, 0.000001f);
      vec3 color = hdrColor * exposureScale * meteredExposure;
      
  #if defined(LUMINANCE)
      // Yxy.x is Y, the luminance
      const float sourceLuminance = dot(
        hdrColor,
        vec3(0.2126729f, 0.7151522f, 0.0721750f));
      vec3 Yxy = sourceLuminance > 0.000001f
        ? convertRGB2Yxy(hdrColor)
        : vec3(0.0f, 0.3127f, 0.3290f);
      float lp = Yxy.x * exposureScale * meteredExposure;
      
      color = vec3(lp);
  #endif

  #if defined(AGX)
      const float displayDither =
        (interleavedGradientNoise(gl_FragCoord.xy) - 0.5f) / 255.0f;
      color = AGX_tonemap(color, displayDither);
  #elif defined(ACES)
      color = ACES_tonemap(color);
  #elif defined(UNCHARTED2)
      color = uncharted2_filmic(color, data.whitePoint.xyz);
  #endif 
  
  #if defined(LUMINANCE)
      outColor.xyz = color.x > 0.0f
        ? convertYxy2RGB(vec3(color.x, Yxy.y, Yxy.z))
        : vec3(0.0f);
  #else
      outColor.xyz = color;
  #endif

      if(any(isnan(outColor.xyz)) || any(isinf(outColor.xyz)))
      {
          outColor.xyz = vec3(0.0f);
      }
      outColor.xyz = clamp(outColor.xyz, vec3(0.0f), vec3(1.0f));
      outColor.a = 1.0f;
  }
