glslCommon: |
  #version 450
  #extension GL_ARB_separate_shader_objects : enable
glslCompute: |
  // The code below taken from the next source: https://github.com/Shot511/RapidGL#bloom
  
  layout(set = 0, binding = 0, rgba16f) uniform readonly image2D u_input_texture;
  layout(set = 0, binding = 1, rgba16f) uniform image2D u_output_image;
  layout(set = 0, binding = 2) uniform sampler2D u_dirt_texture;
  layout(set = 0, binding = 3) uniform sampler2D u_average_luminance;
  
  layout(std430, push_constant) uniform Constants
  {  	
    int   u_mip_level;
    float u_bloom_intensity;
    float u_dirt_intensity;
    float u_scatter;
  } PushConstants;
  
  #define GROUP_SIZE         8
  #define GROUP_THREAD_COUNT (GROUP_SIZE * GROUP_SIZE)
  #define FILTER_RADIUS      1
  #define TILE_SIZE          (GROUP_SIZE + 2 * FILTER_RADIUS)
  #define TILE_PIXEL_COUNT   (TILE_SIZE * TILE_SIZE)
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

  float get_inverse_metering_scale()
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
      // Inverse of the EV100 camera exposure used during first downscale.
      const float photographicExposureKey = 1.0f / 9.6f;
      return max(averageLuminance, 0.000001f) /
        photographicExposureKey;
  }
  
  shared float sm_r[TILE_PIXEL_COUNT];
  shared float sm_g[TILE_PIXEL_COUNT];
  shared float sm_b[TILE_PIXEL_COUNT];
  
  void store_lds(int idx, vec4 c)
  {
      c = sanitize_hdr(c);
      sm_r[idx] = c.r;
      sm_g[idx] = c.g;
      sm_b[idx] = c.b;
  }
  
  vec4 load_lds(uint idx)
  {
      return vec4(sm_r[idx], sm_g[idx], sm_b[idx], 1.0);
  }
  
  layout(local_size_x = GROUP_SIZE, local_size_y = GROUP_SIZE) in;
  void main()
  {
      ivec2 pixel_coords = ivec2(gl_GlobalInvocationID);
      ivec2 base_index   = ivec2(gl_WorkGroupID) * GROUP_SIZE - FILTER_RADIUS;
  
      uvec2 readDim        = imageSize(u_input_texture).xy;
      uvec2 writeDim       = imageSize(u_output_image).xy;
      vec2  texel_size     = 1.0f / writeDim;
      
      vec2 uv              = (vec2(base_index) + 0.5) * texel_size;
      // The first (TILE_PIXEL_COUNT - GROUP_THREAD_COUNT) threads load at most 2 texel values
      for (int i = int(gl_LocalInvocationIndex); i < TILE_PIXEL_COUNT; i += GROUP_THREAD_COUNT)
      {         
          vec2 uv_offset = vec2(i % TILE_SIZE, i / TILE_SIZE) * texel_size;
      
          ivec2 read_coords = clamp(
            ivec2(vec2(readDim) * (uv + uv_offset)),
            ivec2(0),
            ivec2(readDim) - ivec2(1));
          vec4 color = imageLoad(u_input_texture, read_coords);
          store_lds(i, color);
      }
  
      memoryBarrierShared();
      barrier();

      if (any(greaterThanEqual(uvec2(pixel_coords), writeDim)))
      {
          return;
      }
  
      // center texel
      uint sm_idx = (gl_LocalInvocationID.x + FILTER_RADIUS) + (gl_LocalInvocationID.y + FILTER_RADIUS) * TILE_SIZE;
  
      // Based on [Jimenez14] http://goo.gl/eomGso
      vec4 s;
      s =  load_lds(sm_idx - TILE_SIZE - 1);
      s += load_lds(sm_idx - TILE_SIZE    ) * 2.0;
      s += load_lds(sm_idx - TILE_SIZE + 1);
      
      s += load_lds(sm_idx - 1) * 2.0;
      s += load_lds(sm_idx    ) * 4.0;
      s += load_lds(sm_idx + 1) * 2.0;
      
      s += load_lds(sm_idx + TILE_SIZE - 1);
      s += load_lds(sm_idx + TILE_SIZE    ) * 2.0;
      s += load_lds(sm_idx + TILE_SIZE + 1);
      
      vec4 bloom = s * (1.0 / 16.0);
      const bool isFinalComposite = PushConstants.u_mip_level == 1;
      const float bloomToHdrScale = isFinalComposite
        ? get_inverse_metering_scale()
        : 1.0f;
      // Bloom intensity controls the final HDR contribution. Intermediate
      // reconstruction uses a separate spread so distant mip energy is not
      // attenuated by the final intensity at every level.
      const float reconstructionWeight = isFinalComposite
        ? PushConstants.u_bloom_intensity * bloomToHdrScale
        : clamp(PushConstants.u_scatter, 0.0f, 1.0f);
  
      vec4 out_pixel = sanitize_hdr(
        imageLoad(u_output_image, pixel_coords));
      out_pixel += bloom * reconstructionWeight;
  
      if (isFinalComposite)
      {
          vec2  uv  = (vec2(pixel_coords) + vec2(0.5, 0.5)) * texel_size;
          out_pixel += texture(u_dirt_texture, uv) *
            PushConstants.u_dirt_intensity * bloom *
            PushConstants.u_bloom_intensity * bloomToHdrScale;
      }
      
      imageStore(
        u_output_image,
        pixel_coords,
        sanitize_hdr(out_pixel));
  }
