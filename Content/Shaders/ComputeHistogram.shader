defines: []
glslCommon: |
  #version 460
  #extension GL_ARB_separate_shader_objects : enable
glslCompute: |
  
  // The code below taken from the next source: https://bruop.github.io/exposure/
  layout(std430, set = 0, binding = 0) writeonly buffer DataSSBO
  {	
  	 uint data[];
  } histogram;  
  
  layout(set = 0, binding = 1, rgba16f) uniform readonly image2D s_texColor;
  
  layout(push_constant) uniform Constants
  {
  	float minLog2Luminance;
    float invLog2LuminanceRange;
    float centerWeight;
    float minimumMeteredLuminance;
  } PushConstants;
  
  #define GROUP_SIZE 256
  
  // Taken from RTR vol 4 pg. 278
  #define RGB_TO_LUM vec3(0.2125, 0.7154, 0.0721)
  
  // Shared histogram buffer used for storing intermediate sums for each work group
  shared uint histogramShared[GROUP_SIZE];
  
  // For a given color and luminance range, return the histogram bin index
  uint colorToBin(
    vec3 hdrColor,
    float minLogLum,
    float inverseLogLumRange,
    float minimumMeteredLuminance)
  {
    if(any(isnan(hdrColor)))
    {
        return 0;
    }
    hdrColor = max(hdrColor, vec3(0.0f));
    if(any(isinf(hdrColor)))
    {
        return 255;
    }

    // Convert our RGB value to Luminance, see note for RGB_TO_LUM macro above
    float lum = dot(hdrColor, RGB_TO_LUM);
    
    // Samples darker than the exposure floor cannot lower the target exposure
    // any further. Keep them out of the percentile population so a black
    // background does not make the visible subject clip to white.
    if (lum <= minimumMeteredLuminance)
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
        uint binIndex = colorToBin(
          hdrColor,
          PushConstants.minLog2Luminance,
          PushConstants.invLog2LuminanceRange,
          PushConstants.minimumMeteredLuminance);

        // A smooth center-weighted mask prevents bright objects entering at the
        // viewport edges from steering exposure for the whole image. Keep a
        // weight of one at the edges so every part of the frame is represented.
        const vec2 screenPosition =
          ((vec2(gl_GlobalInvocationID.xy) + vec2(0.5f)) / vec2(dim)) * 2.0f - 1.0f;
        const float center = max(1.0f - dot(screenPosition, screenPosition), 0.0f);
        const uint sampleWeight = max(
          uint(round(1.0f + PushConstants.centerWeight * center * center)),
          1u);

        // We use an atomic add to ensure we don't write to the same bin in our
        // histogram from two different threads at the same time.
        atomicAdd(histogramShared[binIndex], sampleWeight);
    }
    
    // Wait for all threads in the work group to reach this point before adding our
    // local histogram to the global one
    barrier();
    
    // Technically there's no chance that two threads write to the same bin here,
    // but different work groups might! So we still need the atomic add.
    atomicAdd(histogram.data[gl_LocalInvocationIndex], histogramShared[gl_LocalInvocationIndex]);
  } 
