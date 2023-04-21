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
  layout(set = 0, binding = 1, rgba16f) uniform image2D u_output_image;
  layout(set = 0, binding = 2) uniform sampler2D u_dirt_texture;
  
  layout(std430, push_constant) uniform Constants
  {  	
    int   u_mip_level;
    float u_bloom_intensity;
    float u_dirt_intensity;
  } PushConstants;
  
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
      
      vec2 uv              = (vec2(base_index) + 0.5) * texel_size;
      // The first (TILE_PIXEL_COUNT - GROUP_THREAD_COUNT) threads load at most 2 texel values
      for (int i = int(gl_LocalInvocationIndex); i < TILE_PIXEL_COUNT; i += GROUP_THREAD_COUNT)
      {         
          vec2 uv_offset = vec2(i % TILE_SIZE, i / TILE_SIZE) * texel_size;
      
          vec4 color = imageLoad(u_input_texture, ivec2(readDim * (uv + uv_offset)));
          store_lds(i, color);
      }
  
      memoryBarrierShared();
      barrier();
  
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
  
      vec4 out_pixel = imageLoad(u_output_image, pixel_coords);
           out_pixel += bloom * PushConstants.u_bloom_intensity;
  
      if (PushConstants.u_mip_level == 1)
      {
          vec2  uv  = (vec2(pixel_coords) + vec2(0.5, 0.5)) * texel_size;
          out_pixel += texture(u_dirt_texture, uv) * PushConstants.u_dirt_intensity * bloom * PushConstants.u_bloom_intensity;
      }
      
  
      imageStore(u_output_image, pixel_coords, out_pixel);
  }
