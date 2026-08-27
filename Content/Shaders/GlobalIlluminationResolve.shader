---
includes:
- Shaders/Constants.glsl
- Shaders/Math.glsl
- Shaders/GlobalIllumination.glsl
defines: []
glslCommon: |
  #version 460
  #extension GL_ARB_separate_shader_objects : enable
  #extension GL_EXT_control_flow_attributes : enable
glslCompute: |
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

  layout(set = 2, binding = 0) uniform sampler2D depthSampler;
  layout(set = 2, binding = 1, rgba32f) uniform writeonly image2D probeCellMin;
  layout(set = 2, binding = 2, rgba32f) uniform writeonly image2D probeCellMax;
  layout(set = 2, binding = 3, rgba32f) uniform writeonly image2D probeCellMetadata;

  const uint ResolveGroupSize = 8u;
  layout(local_size_x = ResolveGroupSize, local_size_y = ResolveGroupSize) in;

  bool ProbeCellCandidateIsBetter(
    uint containmentRank,
    uint subdivision,
    float distanceSquared,
    uint otherContainmentRank,
    uint otherSubdivision,
    float otherDistanceSquared)
  {
    if(containmentRank != otherContainmentRank)
    {
      return containmentRank > otherContainmentRank;
    }
    if(abs(distanceSquared - otherDistanceSquared) > 0.000000000001)
    {
      return distanceSquared < otherDistanceSquared;
    }
    return subdivision > otherSubdivision;
  }

  bool SelectProbeCell(
    vec3 tracePosition,
    uvec4 encodedBrickIndices,
    uint brickCount,
    out uint selectedBrickIndex,
    out GlobalIlluminationBrick selectedBrick)
  {
    bool foundBrick = false;
    uint selectedContainmentRank = 0u;
    uint selectedSubdivision = 0u;
    float selectedDistanceSquared = 3.402823466e+38;
    for(uint candidateIndex = 0u; candidateIndex < brickCount; ++candidateIndex)
    {
      const uint encodedBrickIndex = encodedBrickIndices[candidateIndex];
      if(encodedBrickIndex == 0u)
      {
        continue;
      }
      const uint brickIndex = encodedBrickIndex - 1u;
      if(brickIndex >= globalIlluminationHeader.counts.z ||
        brickIndex >= uint(globalIlluminationBricks.instance.length()))
      {
        continue;
      }
      const GlobalIlluminationBrick brick =
        globalIlluminationBricks.instance[brickIndex];
      if(brick.probeCountsAndValidCount.w == 0u)
      {
        continue;
      }
      const float distanceSquared =
        GlobalIlluminationDistanceSquaredToBounds(
          tracePosition,
          brick.minAndSubdivision.xyz,
          brick.maxAndFirstProbe.xyz);
      const uint containmentRank = GlobalIlluminationContainsExact(
        brick.minAndSubdivision.xyz,
        brick.maxAndFirstProbe.xyz,
        tracePosition)
        ? 2u
        : (GlobalIlluminationContains(
            brick.minAndSubdivision.xyz,
            brick.maxAndFirstProbe.xyz,
            tracePosition) ? 1u : 0u);
      const uint subdivision =
        floatBitsToUint(brick.minAndSubdivision.w);
      if(!foundBrick || ProbeCellCandidateIsBetter(
        containmentRank,
        subdivision,
        distanceSquared,
        selectedContainmentRank,
        selectedSubdivision,
        selectedDistanceSquared))
      {
        foundBrick = true;
        selectedBrickIndex = brickIndex;
        selectedBrick = brick;
        selectedContainmentRank = containmentRank;
        selectedSubdivision = subdivision;
        selectedDistanceSquared = distanceSquared;
      }
    }
    return foundBrick;
  }

  void main()
  {
    const ivec2 pixel = ivec2(gl_GlobalInvocationID.xy);
    const ivec2 outputExtent = imageSize(probeCellMin);
    if(any(greaterThanEqual(pixel, outputExtent)))
    {
      return;
    }

    // Zero probe counts mark an invalid cell. The three images contain the
    // complete interpolation-cell metadata so material shaders never touch the
    // BVH or brick SSBOs.
    imageStore(probeCellMin, pixel, vec4(0.0));
    imageStore(probeCellMax, pixel, vec4(0.0));
    imageStore(probeCellMetadata, pixel, vec4(0.0));
    if(!GlobalIlluminationDebugUsesProbeData() &&
      (globalIlluminationHeader.settings.x == 0u ||
        globalIlluminationHeader.settings.y == GLOBAL_ILLUMINATION_MODE_REALTIME))
    {
      return;
    }

    const float depth = texelFetch(depthSampler, pixel, 0).r;
    if(depth <= 0.000001)
    {
      return;
    }

    const vec2 uv = (vec2(pixel) + vec2(0.5)) * rcp(vec2(outputExtent));
    const vec3 reconstructedViewPosition = ScreenSpaceToViewSpace(
      FramebufferUvToSceneProjectionUv(uv),
      depth,
      frame.invProjection).xyz;
    // ScreenSpaceToViewSpace follows the positive-Z convention used by the
    // screen-space passes. Convert to the camera's right-handed view space.
    const vec3 cameraViewPosition =
      reconstructedViewPosition * vec3(1.0, 1.0, -1.0);
    const mat3 viewToWorld = transpose(mat3(frame.view));
    const vec3 worldPosition =
      viewToWorld * cameraViewPosition + frame.cameraPosition.xyz;
    const vec3 surfaceToCamera = frame.cameraPosition.xyz - worldPosition;
    const float surfaceToCameraLengthSquared =
      dot(surfaceToCamera, surfaceToCamera);
    const vec3 viewBiasDirection = surfaceToCameraLengthSquared > 0.000001
      ? surfaceToCamera * inversesqrt(surfaceToCameraLengthSquared)
      : vec3(0.0);
    // The coarse pass performs the BVH/brick lookup and packs one complete
    // interpolation cell. Material-normal bias, trilinear weights, visibility
    // and SH evaluation remain full resolution. Query locally first; only a
    // hole with no valid brick expands to bounded neighboring probe support.
    const vec3 tracePosition = worldPosition +
      viewBiasDirection * globalIlluminationHeader.volumeMax.w;
    uvec4 encodedBrickIndices = uvec4(0u);
    uint brickCount = 0u;
    bool foundBrickCandidates = GlobalIlluminationFindBrickCandidates(
      tracePosition,
      globalIlluminationHeader.volumeMin.w,
      encodedBrickIndices,
      brickCount);
    if(!foundBrickCandidates)
    {
      foundBrickCandidates = GlobalIlluminationFindBrickCandidates(
        tracePosition,
        GlobalIlluminationNeighborSupportRadius(),
        encodedBrickIndices,
        brickCount);
    }
    if(!foundBrickCandidates)
    {
      return;
    }

    uint selectedBrickIndex = 0u;
    GlobalIlluminationBrick selectedBrick;
    if(!SelectProbeCell(
      tracePosition,
      encodedBrickIndices,
      brickCount,
      selectedBrickIndex,
      selectedBrick))
    {
      return;
    }
    imageStore(
      probeCellMin,
      pixel,
      vec4(
        selectedBrick.minAndSubdivision.xyz,
        float(floatBitsToUint(selectedBrick.maxAndFirstProbe.w))));
    imageStore(
      probeCellMax,
      pixel,
      vec4(
        selectedBrick.maxAndFirstProbe.xyz,
        float(selectedBrickIndex + 1u)));
    imageStore(
      probeCellMetadata,
      pixel,
      vec4(
        vec3(selectedBrick.probeCountsAndValidCount.xyz),
        float(floatBitsToUint(selectedBrick.minAndSubdivision.w))));
  }
