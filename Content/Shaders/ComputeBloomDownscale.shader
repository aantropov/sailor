includes:
- Shaders/Constants.glsl
defines: []
glslCommon: |
  #version 450
  #extension GL_ARB_separate_shader_objects : enable
  #extension GL_EXT_shader_atomic_float : enable
glslCompute: |
  // The code below taken from the next source: https://github.com/Shot511/RapidGL#bloom
  
  layout(set = 0, binding = 0, rgba16f) uniform readonly image2D u_input_texture;
  layout(set = 0, binding = 1, rgba16f) uniform writeonly image2D u_output_image;
  
  layout(std430, push_constant) uniform Constants
  {
  	vec4 u_threshold; // x -> threshold, yzw -> (threshold - knee, 2.0 * knee, 0.25 * knee)
    bool  u_use_threshold;
  } PushConstants;
  
  const float epsilon = 1.0e-4;
  
  // Curve = (threshold - knee, knee * 2.0, knee * 0.25)
  vec4 quadratic_threshold(vec4 color, float threshold, vec3 curve)
  {
      // Pixel brightness
      float br = max(color.r, max(color.g, color.b));
  
      // Under-threshold part: quadratic curve
      float rq = clamp(br - curve.x, 0.0, curve.y);
      rq = curve.z * rq * rq;
  
      // Combine and apply the brightness response curve.
      color *= max(rq, br - threshold) / max(br, epsilon);
  
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
  
  #define GROUP_SIZE         8
  #define GROUP_THREAD_COUNT (GROUP_SIZE * GROUP_SIZE)
  #define FILTER_RADIUS      1
  #define TILE_SIZE          (GROUP_SIZE + 2 * FILTER_RADIUS)
  #define TILE_PIXEL_COUNT   (TILE_SIZE * TILE_SIZE)
  
  shared float sm_r[TILE_PIXEL_COUNT];
  shared float sm_g[TILE_PIXEL_COUNT];
  shared float sm_b[TILE_PIXEL_COUNT];
  
  void store_lds(int idx, vec4 c)
  {
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
      
      // The first (TILE_PIXEL_COUNT - GROUP_THREAD_COUNT) threads load at most 2 texel values
      for (int i = int(gl_LocalInvocationIndex); i < TILE_PIXEL_COUNT; i += GROUP_THREAD_COUNT)
      {
          vec2 uv        = (vec2(base_index) + 0.5) * texel_size;
          vec2 uv_offset = vec2(i % TILE_SIZE, i / TILE_SIZE) * texel_size;          

          vec4 color = imageLoad(u_input_texture, ivec2(readDim * (uv + uv_offset)));
          store_lds(i, color);
      }
  
      memoryBarrierShared();
      barrier();
  
      // Based on [Jimenez14] http://goo.gl/eomGso
      // center texel
      uint sm_idx = (gl_LocalInvocationID.x + FILTER_RADIUS) + (gl_LocalInvocationID.y + FILTER_RADIUS) * TILE_SIZE;
  
      vec4 A = load_lds(sm_idx - TILE_SIZE - 1);
      vec4 B = load_lds(sm_idx - TILE_SIZE    );
      vec4 C = load_lds(sm_idx - TILE_SIZE + 1);
      vec4 F = load_lds(sm_idx - 1            );
      vec4 G = load_lds(sm_idx                );
      vec4 H = load_lds(sm_idx + 1            );
      vec4 K = load_lds(sm_idx + TILE_SIZE - 1);
      vec4 L = load_lds(sm_idx + TILE_SIZE    );
      vec4 M = load_lds(sm_idx + TILE_SIZE + 1);
  
      vec4 D = (A + B + G + F) * 0.25;
      vec4 E = (B + C + H + G) * 0.25;
      vec4 I = (F + G + L + K) * 0.25;
      vec4 J = (G + H + M + L) * 0.25;
  
      vec2 div = (1.0 / 4.0) * vec2(0.5, 0.125);
  
      vec4 c =  karis_avg((D + E + I + J) * div.x);
           c += karis_avg((A + B + G + F) * div.y);
           c += karis_avg((B + C + H + G) * div.y);
           c += karis_avg((F + G + L + K) * div.y);
           c += karis_avg((G + H + M + L) * div.y);
  
  	if (PushConstants.u_use_threshold)
    {
          c = quadratic_threshold(c, PushConstants.u_threshold.x, PushConstants.u_threshold.yzw);
    }
  
  	imageStore(u_output_image, pixel_coords, c);
  }
