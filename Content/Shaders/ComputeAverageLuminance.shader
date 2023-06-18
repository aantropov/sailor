includes:
- Shaders/Constants.glsl
- Shaders/Math.glsl
- Shaders/Lighting.glsl

defines: []
glslCommon: |
  #version 460
  #extension GL_ARB_separate_shader_objects : enable
  #extension GL_EXT_shader_atomic_float : enable

glslCompute: |  
  // The code below taken from the next source: https://bruop.github.io/exposure/
  layout(std430, set = 0, binding = 0) buffer DataSSBO
  {	
  	 uint data[];
  } histogram;  
  
  layout(set = 0, binding = 1, r16f) uniform image2D s_texColor;
  
  layout(push_constant) uniform Constants
  {
  	float minLog2Luminance;
    float log2LuminanceRange;
    float numPixels;
    float timeCoeff;
  } PushConstants;
  
  #define GROUP_SIZE 256
  
  #define EPSILON 0.005
  // Taken from RTR vol 4 pg. 278
  #define RGB_TO_LUM vec3(0.2125, 0.7154, 0.0721)
  
  // Shared histogram buffer used for storing intermediate sums for each work group
  shared uint histogramShared[GROUP_SIZE];
  
  layout(local_size_x = GROUP_SIZE, local_size_y = 1, local_size_z = 1) in; 
  void main() 
  {
    // Get the count from the histogram buffer
    uint countForThisBin = histogram.data[gl_LocalInvocationIndex];
    histogramShared[gl_LocalInvocationIndex] = countForThisBin * gl_LocalInvocationIndex;
    
    barrier();
    
    // Reset the count stored in the buffer in anticipation of the next pass
    histogram.data[gl_LocalInvocationIndex] = 0;
    
    // This loop will perform a weighted count of the luminance range
    for (uint cutoff = (GROUP_SIZE >> 1); cutoff > 0; cutoff >>= 1) 
    {
        if (uint(gl_LocalInvocationIndex) < cutoff) 
        {
            histogramShared[gl_LocalInvocationIndex] += histogramShared[gl_LocalInvocationIndex + cutoff];
        }
    
        barrier();
    }
    
    // We only need to calculate this once, so only a single thread is needed.
    if (gl_GlobalInvocationID.xy == vec2(0,0)) 
    {
        // Here we take our weighted sum and divide it by the number of pixels
        // that had luminance greater than zero (since the index == 0, we can
        // use countForThisBin to find the number of black pixels)
        float weightedLogAverage = (histogramShared[0] / max(PushConstants.numPixels - float(countForThisBin), 1.0)) - 1.0;
    
        // Map from our histogram space to actual luminance
        float weightedAvgLum = exp2(((weightedLogAverage / 254.0) * PushConstants.log2LuminanceRange) + PushConstants.minLog2Luminance);
    
        // The new stored value will be interpolated using the last frames value
        // to prevent sudden shifts in the exposure.
        float lumLastFrame = imageLoad(s_texColor, ivec2(0, 0)).x;
        float adaptedLum = lumLastFrame + (weightedAvgLum - lumLastFrame) * PushConstants.timeCoeff;
        imageStore(s_texColor, ivec2(0, 0), vec4(adaptedLum, 0.0, 0.0, 0.0));
    }
  } 
