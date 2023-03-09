includes:
- Shaders/Math.glsl
- Shaders/Lighting.glsl 
defines: []
glslCommon: |
  #version 450 core
  #extension GL_ARB_separate_shader_objects : enable
  #extension GL_EXT_shader_atomic_float : enable
glslCompute: |
  // Pre-integrates Cook-Torrance specular BRDF for varying roughness and viewing directions.
  // Results are saved into 2D LUT texture in the form of DFG1 and DFG2 split-sum approximation terms,
  // which act as a scale and bias to F0 (Fresnel reflectance at normal incidence) during rendering.
  
  const float Epsilon = 0.001; // This program needs larger eps.
  
  const uint NumSamples = 1024;
  const float InvNumSamples = 1.0 / float(NumSamples);

  layout(set=0, binding=0, rg16f) restrict writeonly uniform image2D dst;
  
  // Sample i-th point from Hammersley point set of NumSamples points total.
  vec2 SampleHammersley(uint i)
  {
    return vec2(i * InvNumSamples, RadicalInverse_VdC(i));
  }
  
  layout(local_size_x=32, local_size_y=32, local_size_z=1) in;
  void main(void)
  {
    float cosLo = gl_GlobalInvocationID.x / float(imageSize(dst).x);
    float roughness = gl_GlobalInvocationID.y / float(imageSize(dst).y);
  
    // Make sure viewing angle is non-zero to avoid divisions by zero (and subsequently NaNs).
    cosLo = max(cosLo, Epsilon);
  
    // Derive tangent-space viewing vector from angle to normal (pointing towards +Z in this reference frame).
    vec3 Lo = vec3(sqrt(1.0 - cosLo*cosLo), 0.0, cosLo);
  
    // We will now pre-integrate Cook-Torrance BRDF for a solid white environment and save results into a 2D LUT.
    // DFG1 & DFG2 are terms of split-sum approximation of the reflectance integral.
    // For derivation see: "Moving Frostbite to Physically Based Rendering 3.0", SIGGRAPH 2014, section 4.9.2.
    float DFG1 = 0;
    float DFG2 = 0;
  
    for(uint i = 0; i < NumSamples; i++) 
    {
      vec2 u  = SampleHammersley(i);
  
      // Sample directly in tangent/shading space since we don't care about reference frame as long as it's consistent.
      vec3 Lh = SampleGGX(u.x, u.y, roughness);
  
      // Compute incident direction (Li) by reflecting viewing direction (Lo) around half-vector (Lh).
      vec3 Li = 2.0 * dot(Lo, Lh) * Lh - Lo;
  
      float cosLi   = Li.z;
      float cosLh   = Lh.z;
      float cosLoLh = max(dot(Lo, Lh), 0.0);
  
      if(cosLi > 0.0) 
        {
        float G  = GeometrySchlickGGX_IBL(cosLi, cosLo, roughness);
        float Gv = G * cosLoLh / (cosLh * cosLo);
        float Fc = pow(1.0 - cosLoLh, 5);
  
        DFG1 += (1 - Fc) * Gv;
        DFG2 += Fc * Gv;
      }
    }
  
    imageStore(dst, ivec2(gl_GlobalInvocationID), vec4(DFG1, DFG2, 0, 0) * InvNumSamples);
  }
