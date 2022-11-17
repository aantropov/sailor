includes:
  - Shaders/Lighting.glsl
defines: []
glslCommon: |
  #version 460
  #extension GL_ARB_separate_shader_objects : enable
  #extension GL_EXT_shader_atomic_float : enable
glslCompute: |
  
  // The code below taken from the next source: https://bruop.github.io/exposure/
  layout(set = 0, binding = 0) uniform FrameData
  {
      mat4 view;
      mat4 projection;	
      mat4 invProjection;
      vec4 cameraPosition;
      ivec2 viewportSize;
      float currentTime;
      float deltaTime;
  } frame;

  layout(std430, set = 1, binding = 0) writeonly buffer DataSSBO
  {	
  	 float histogram[];
  } data;  
  
  layout(set = 1, binding = 1, rgba16f) uniform image2D s_texColor;
  
  layout(push_constant) uniform Constants
  {
  	float minLog2Luminance;
    float invMinLog2Luminance;
  } PushConstants;
  
  #define GROUP_SIZE 256
  
  #define EPSILON 0.005
  // Taken from RTR vol 4 pg. 278
  #define RGB_TO_LUM vec3(0.2125, 0.7154, 0.0721)
  
  // Shared histogram buffer used for storing intermediate sums for each work group
  shared uint histogramShared[GROUP_SIZE];
  
  // For a given color and luminance range, return the histogram bin index
  uint colorToBin(vec3 hdrColor, float minLogLum, float inverseLogLumRange) 
  {
    // Convert our RGB value to Luminance, see note for RGB_TO_LUM macro above
    float lum = dot(hdrColor, RGB_TO_LUM);
    
    // Avoid taking the log of zero
    if (lum < EPSILON) 
    {
        return 0;
    }
    
    // Calculate the log_2 luminance and express it as a value in [0.0, 1.0]
    // where 0.0 represents the minimum luminance, and 1.0 represents the max.
    float logLum = clamp((log2(lum) - minLogLum) * inverseLogLumRange, 0.0, 1.0);
    
    // Map [0, 1] to [1, 255]. The zeroth bin is handled by the epsilon check above.
    return uint(logLum * 254.0 + 1.0);
  }
 
  layout(local_size_x = 16, local_size_y = 16, local_size_z = 1) in; 
  void main() 
  {
    // Initialize the bin for this thread to 0
    histogramShared[gl_LocalInvocationIndex] = 0;
    barrier();
    
    uvec2 dim = imageSize(s_texColor).xy;
    
    // Ignore threads that map to areas beyond the bounds of our HDR image
    if (gl_GlobalInvocationID.x < dim.x && gl_GlobalInvocationID.y < dim.y) 
    {
        vec3 hdrColor = imageLoad(s_texColor, ivec2(gl_GlobalInvocationID.xy)).xyz;
        uint binIndex = colorToBin(hdrColor, PushConstants.minLog2Luminance, PushConstants.invMinLog2Luminance);
        
        // We use an atomic add to ensure we don't write to the same bin in our
        // histogram from two different threads at the same time.
        atomicAdd(histogramShared[binIndex], 1);
    }
    
    // Wait for all threads in the work group to reach this point before adding our
    // local histogram to the global one
    barrier();
    
    // Technically there's no chance that two threads write to the same bin here,
    // but different work groups might! So we still need the atomic add.
    atomicAdd(data.histogram[gl_LocalInvocationIndex], histogramShared[gl_LocalInvocationIndex]);
  } 
