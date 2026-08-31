defines: []
glslCommon: |
  #version 460
  #extension GL_ARB_separate_shader_objects : enable

glslCompute: |  
  // The code below taken from the next source: https://bruop.github.io/exposure/
  layout(std430, set = 0, binding = 0) buffer DataSSBO
  {	
  	 uint data[];
  } histogram;  
  
  layout(set = 0, binding = 1, r32f) uniform image2D s_texColor;
  
  layout(push_constant) uniform Constants
  {
  	float minLog2Luminance;
    float log2LuminanceRange;
    float lowPercentile;
    float highPercentile;
    float minEV100;
    float maxEV100;
    float deltaTime;
    float speedUp;
    float speedDown;
    float padding0;
    float padding1;
    float padding2;
  } PushConstants;
  
  #define GROUP_SIZE 256
  
  // Shared histogram buffer used for storing intermediate sums for each work group
  shared uint histogramShared[GROUP_SIZE];
  
  layout(local_size_x = GROUP_SIZE, local_size_y = 1, local_size_z = 1) in; 
  void main() 
  {
    uint countForThisBin = histogram.data[gl_LocalInvocationIndex];
    histogramShared[gl_LocalInvocationIndex] = countForThisBin;
    
    barrier();
    
    // Reset the count stored in the buffer in anticipation of the next pass
    histogram.data[gl_LocalInvocationIndex] = 0;
    
    // A tiny number of black pixels or highlights such as the sun disk must not
    // steer exposure for the whole frame. Process the 256-bin histogram once
    // and average only the configured percentile interval.
    if (all(equal(gl_GlobalInvocationID.xy, uvec2(0))))
    {
        uint nonBlackCount = 0;
        for(uint index = 1; index < GROUP_SIZE; ++index)
        {
            nonBlackCount += histogramShared[index];
        }

        if(nonBlackCount == 0)
        {
            return;
        }

        const float lowRank = float(nonBlackCount) *
          clamp(PushConstants.lowPercentile, 0.0f, 1.0f);
        const float highRank = float(nonBlackCount) *
          clamp(PushConstants.highPercentile, 0.0f, 1.0f);
        float cumulative = 0.0f;
        float includedCount = 0.0f;
        float weightedBins = 0.0f;
        for(uint index = 1; index < GROUP_SIZE; ++index)
        {
            const float count = float(histogramShared[index]);
            const float nextCumulative = cumulative + count;
            const float included = max(
              min(nextCumulative, highRank) - max(cumulative, lowRank),
              0.0f);
            includedCount += included;
            weightedBins += included * float(index);
            cumulative = nextCumulative;
        }

        const float weightedLogAverage =
          weightedBins / max(includedCount, 1.0f) - 1.0f;
        float weightedAvgLum = exp2(
          (weightedLogAverage / 254.0f) *
            PushConstants.log2LuminanceRange +
          PushConstants.minLog2Luminance);

        // EV100 = log2(8 * luminance) for ISO 100 reflected-light metering.
        // Clamping the target EV bounds how far a dark view may lift the night
        // sky and keeps physically authored daylight values inside the meter.
        const float minLuminance = exp2(PushConstants.minEV100) / 8.0f;
        const float maxLuminance = exp2(PushConstants.maxEV100) / 8.0f;
        weightedAvgLum = clamp(
          weightedAvgLum,
          min(minLuminance, maxLuminance),
          max(minLuminance, maxLuminance));

        float lumLastFrame = imageLoad(s_texColor, ivec2(0, 0)).x;
        float adaptedLum = weightedAvgLum;
        if(lumLastFrame > 0.0f &&
           !isnan(lumLastFrame) && !isinf(lumLastFrame))
        {
            const float lastLogLuminance = log2(lumLastFrame);
            const float targetLogLuminance = log2(weightedAvgLum);
            const float adaptationSpeed = targetLogLuminance > lastLogLuminance
              ? PushConstants.speedUp
              : PushConstants.speedDown;
            const float timeCoeff = clamp(
              1.0f - exp2(-PushConstants.deltaTime * adaptationSpeed),
              0.0f,
              1.0f);
            adaptedLum = exp2(mix(
              lastLogLuminance,
              targetLogLuminance,
              timeCoeff));
        }
        imageStore(s_texColor, ivec2(0, 0), vec4(adaptedLum, 0.0, 0.0, 0.0));
    }
  } 
