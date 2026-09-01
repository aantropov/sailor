defines: []
glslCommon: |
  #version 450
  #extension GL_ARB_separate_shader_objects : enable
glslCompute: |
  // The code below taken from the next source: https://github.com/Shot511/RapidGL#bloom
  
  layout(set = 0, binding = 0, rgba16f) uniform readonly image2D u_input_texture;
  layout(set = 0, binding = 1, rgba16f) uniform writeonly image2D u_output_image;
  layout(set = 0, binding = 2) uniform sampler2D u_average_luminance;
  
  layout(std430, push_constant) uniform Constants
  {
    vec4 u_threshold; // x -> threshold, yzw -> (threshold - knee, 2.0 * knee, 0.25 / knee)
    bool  u_use_threshold;
  } PushConstants;
  
  const float epsilon = 1.0e-4;
  const float max_half_float = 65504.0f;

  vec4 sanitize_hdr(vec4 color)
  {
      color.rgb = mix(color.rgb, vec3(0.0f), isnan(color.rgb));
      color.rgb = mix(color.rgb, vec3(0.0f), isinf(color.rgb));
      color.rgb = min(
        max(color.rgb, vec3(0.0f)),
        vec3(max_half_float));
      color.a = isnan(color.a) || isinf(color.a) ? 1.0f : color.a;
      return color;
  }

  float get_metering_scale()
  {
      float averageLuminance = texture(
        u_average_luminance,
        vec2(0.5f)).x;
      if (isnan(averageLuminance) ||
          isinf(averageLuminance) ||
          averageLuminance <= 0.0f)
      {
          averageLuminance = 1.0f;
      }
      // Match the EV100 camera exposure used by Tonemapping.shader so that a
      // bloom threshold denotes the same scene-referred brightness on screen.
      const float photographicExposureKey = 1.0f / 9.6f;
      return photographicExposureKey /
        max(averageLuminance, 0.000001f);
  }
  
  // Curve = (threshold - knee, knee * 2.0, 0.25 / knee)
  vec4 quadratic_threshold(vec4 color, float threshold, vec3 curve)
  {
      const float sourceAlpha = color.a;
      float br = max(color.r, max(color.g, color.b));
  
      // Under-threshold part: quadratic curve
      float rq = clamp(br - curve.x, 0.0, curve.y);
      rq = curve.z * rq * rq;
  
      // Combine and apply the brightness response curve.
      color *= max(rq, br - threshold) / max(br, epsilon);
      // Alpha carries non-color metadata for the first downsample and must not
      // be altered by the RGB bloom threshold.
      color.a = sourceAlpha;
  
      return color;
  }
  
  float luma(vec3 c)
  {
      return dot(c, vec3(0.2126729, 0.7151522, 0.0721750));
  }
  
  // [Karis2013] proposed reducing the dynamic range before averaging
  vec4 karis_avg(vec4 c)
  {
      return c / (1.0 + luma(c.rgb));
  }

  float sun_bloom_marker(vec4 c)
  {
      return 1.0f - smoothstep(0.05f, 0.25f, abs(c.a));
  }
  
  #define GROUP_SIZE         8
  #define GROUP_THREAD_COUNT (GROUP_SIZE * GROUP_SIZE)
  #define TILE_SIZE          (GROUP_SIZE * 2 + 3)
  #define TILE_PIXEL_COUNT   (TILE_SIZE * TILE_SIZE)
  
  shared float sm_r[TILE_PIXEL_COUNT];
  shared float sm_g[TILE_PIXEL_COUNT];
  shared float sm_b[TILE_PIXEL_COUNT];
  shared float sm_a[TILE_PIXEL_COUNT];
  shared float sm_metering_scale;
  
  void store_lds(int idx, vec4 c)
  {
      c = sanitize_hdr(c);
      sm_r[idx] = c.r;
      sm_g[idx] = c.g;
      sm_b[idx] = c.b;
      sm_a[idx] = c.a;
  }
  
  vec4 load_lds(uint idx)
  {
      return vec4(sm_r[idx], sm_g[idx], sm_b[idx], sm_a[idx]);
  }
  
  layout(local_size_x = GROUP_SIZE, local_size_y = GROUP_SIZE) in;
  void main()
  {
      const ivec2 pixel_coords = ivec2(gl_GlobalInvocationID);
      const ivec2 base_index =
        ivec2(gl_WorkGroupID) * GROUP_SIZE * 2 - ivec2(1);
  
      const uvec2 readDim = imageSize(u_input_texture).xy;
      const uvec2 writeDim = imageSize(u_output_image).xy;

      if (gl_LocalInvocationIndex == 0u)
      {
          sm_metering_scale = PushConstants.u_use_threshold
            ? get_metering_scale()
            : 1.0f;
      }
      memoryBarrierShared();
      barrier();
      
      // One workgroup covers an 8x8 output tile. A correct 13-tap
      // downsample needs the corresponding 19x19 source footprint, including
      // the half-step samples between the even source texels.
      for (int i = int(gl_LocalInvocationIndex); i < TILE_PIXEL_COUNT; i += GROUP_THREAD_COUNT)
      {
          const ivec2 tile_coords = ivec2(i % TILE_SIZE, i / TILE_SIZE);
          const ivec2 read_coords = clamp(
            base_index + tile_coords,
            ivec2(0),
            ivec2(readDim) - ivec2(1));
          vec4 color = imageLoad(u_input_texture, read_coords);
          if (PushConstants.u_use_threshold)
          {
              // Keep the bloom pyramid in metered scene space. This avoids
              // half-float overflow and keeps its threshold independent of
              // whether authored lights use lux, candela, or emissive nits.
              color.rgb *= sm_metering_scale;
              color = quadratic_threshold(
                color,
                PushConstants.u_threshold.x,
                PushConstants.u_threshold.yzw);
          }
          store_lds(i, color);
      }
  
      memoryBarrierShared();
      barrier();

      if (any(greaterThanEqual(uvec2(pixel_coords), writeDim)))
      {
          return;
      }
  
      // Jimenez' 13-tap pattern: four overlapping corner quads carry half
      // of the energy and the four interleaved center taps carry the other
      // half. The weights sum to exactly one.
      const uint tileStride = uint(TILE_SIZE);
      const uint sm_idx =
        (gl_LocalInvocationID.x * 2u + 2u) +
        (gl_LocalInvocationID.y * 2u + 2u) * tileStride;
      const vec4 A = load_lds(sm_idx - 2u * tileStride - 2u);
      const vec4 B = load_lds(sm_idx - 2u * tileStride);
      const vec4 C = load_lds(sm_idx - 2u * tileStride + 2u);
      const vec4 D = load_lds(sm_idx - tileStride - 1u);
      const vec4 E = load_lds(sm_idx - tileStride + 1u);
      const vec4 F = load_lds(sm_idx - 2u);
      const vec4 G = load_lds(sm_idx);
      const vec4 H = load_lds(sm_idx + 2u);
      const vec4 I = load_lds(sm_idx + tileStride - 1u);
      const vec4 J = load_lds(sm_idx + tileStride + 1u);
      const vec4 K = load_lds(sm_idx + 2u * tileStride - 2u);
      const vec4 L = load_lds(sm_idx + 2u * tileStride);
      const vec4 M = load_lds(sm_idx + 2u * tileStride + 2u);

      const vec4 quadNW = (A + B + F + G) * 0.25f;
      const vec4 quadNE = (B + C + G + H) * 0.25f;
      const vec4 quadSW = (F + G + K + L) * 0.25f;
      const vec4 quadSE = (G + H + L + M) * 0.25f;
      const vec4 quadCenter = (D + E + I + J) * 0.25f;

      const vec4 linearAverage =
        0.125f * (quadNW + quadNE + quadSW + quadSE) +
        0.5f * quadCenter;

      vec4 c;
      if (PushConstants.u_use_threshold)
      {
          // Karis averaging is a first-downsample firefly filter. Repeating
          // this nonlinear compression at every mip destroys wide bloom
          // energy and makes the result depend on the pyramid depth.
          c = 0.125f * (
            karis_avg(quadNW) +
            karis_avg(quadNE) +
            karis_avg(quadSW) +
            karis_avg(quadSE)) +
            0.5f * karis_avg(quadCenter);

          const float sunCoverage = clamp(
            sun_bloom_marker(A) + sun_bloom_marker(B) +
            sun_bloom_marker(C) + sun_bloom_marker(D) +
            sun_bloom_marker(E) + sun_bloom_marker(F) +
            sun_bloom_marker(G) + sun_bloom_marker(H) +
            sun_bloom_marker(I) + sun_bloom_marker(J) +
            sun_bloom_marker(K) + sun_bloom_marker(L) +
            sun_bloom_marker(M),
            0.0f,
            1.0f);
          const float averageLuminance = max(
            luma(linearAverage.rgb),
            0.0f);
          vec4 solarEnergy = linearAverage;
          solarEnergy.rgb *= inversesqrt(1.0f + averageLuminance);
          solarEnergy.a = 1.0f;
          c = mix(c, solarEnergy, sunCoverage);
      }
      else
      {
          c = linearAverage;
      }

      imageStore(u_output_image, pixel_coords, sanitize_hdr(c));
  }
