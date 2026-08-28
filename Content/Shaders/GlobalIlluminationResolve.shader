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
  layout(set = 2, binding = 1, rgba32f) uniform writeonly image2D probeCellIndices;

  const uint ResolveGroupSize = 8u;
  layout(local_size_x = ResolveGroupSize, local_size_y = ResolveGroupSize) in;
  shared uvec4 resolveRepresentativeEncodedBrickIndices;
  shared uint resolveRepresentativeBrickCount;

  uint ResolveRepresentativeInvocation()
  {
    const uint representativeX = 3u;
    const uint representativeY = 3u;
    return representativeY * ResolveGroupSize + representativeX;
  }

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
    out uint selectedBrickIndex)
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
        floatBitsToUint(brick.minAndSubdivision.w) &
        GLOBAL_ILLUMINATION_BRICK_SUBDIVISION_MASK;
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
        selectedContainmentRank = containmentRank;
        selectedSubdivision = subdivision;
        selectedDistanceSquared = distanceSquared;
      }
    }
    return foundBrick;
  }

  bool FindProbeCellCandidates(
    vec3 tracePosition,
    out uvec4 encodedBrickIndices,
    out uint brickCount)
  {
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
    return foundBrickCandidates;
  }

  bool ProbeCellContainsStrictInterior(
    vec3 tracePosition,
    vec3 brickMin,
    vec3 brickMax)
  {
    const float tolerance =
      GlobalIlluminationBoundsTolerance(brickMin, brickMax);
    // Adaptive-layout bricks are non-overlapping leaves. Strict interior
    // containment therefore proves that the local traversal selects this
    // same brick. Shared boundaries stay on the exact per-pixel fallback.
    return all(greaterThan(tracePosition, brickMin + vec3(tolerance))) &&
      all(lessThan(tracePosition, brickMax - vec3(tolerance)));
  }

  bool TrySelectProbeCellCandidate(
    vec3 tracePosition,
    uint encodedBrickIndex,
    out uint selectedEncodedBrickIndex)
  {
    selectedEncodedBrickIndex = 0u;
    if(encodedBrickIndex == 0u)
    {
      return false;
    }

    const uint brickIndex = encodedBrickIndex - 1u;
    if(brickIndex >= globalIlluminationHeader.counts.z ||
      brickIndex >= uint(globalIlluminationBricks.instance.length()))
    {
      return false;
    }

    const GlobalIlluminationBrick brick =
      globalIlluminationBricks.instance[brickIndex];
    if(brick.probeCountsAndValidCount.w == 0u ||
      !ProbeCellContainsStrictInterior(
        tracePosition,
        brick.minAndSubdivision.xyz,
        brick.maxAndFirstProbe.xyz))
    {
      return false;
    }

    selectedEncodedBrickIndex = encodedBrickIndex;
    return true;
  }

  ivec2 ResolveDepthPixel(
    ivec2 pixel,
    uint subpixelIndex,
    ivec2 outputExtent,
    ivec2 depthExtent)
  {
    const ivec2 subpixelOffset = ivec2(
      int(subpixelIndex & 1u),
      int(subpixelIndex >> 1u));
    const vec2 uv =
      (vec2(pixel) + (vec2(subpixelOffset) + vec2(0.5)) * 0.5) *
      rcp(vec2(outputExtent));
    return clamp(
      ivec2(floor(uv * vec2(depthExtent))),
      ivec2(0),
      depthExtent - ivec2(1));
  }

  bool ResolveTracePosition(
    ivec2 depthPixel,
    ivec2 depthExtent,
    out vec3 tracePosition)
  {
    tracePosition = vec3(0.0);
    if(any(lessThan(depthPixel, ivec2(0))) ||
      any(greaterThanEqual(depthPixel, depthExtent)))
    {
      return false;
    }

    const bool bResolveEnabled = GlobalIlluminationDebugUsesProbeData() ||
      (globalIlluminationHeader.settings.x != 0u &&
        globalIlluminationHeader.settings.y !=
          GLOBAL_ILLUMINATION_MODE_REALTIME);
    if(!bResolveEnabled)
    {
      return false;
    }

    const float depth = texelFetch(depthSampler, depthPixel, 0).r;
    if(depth <= 0.000001)
    {
      return false;
    }

    const vec2 uv = (vec2(depthPixel) + vec2(0.5)) *
      rcp(vec2(depthExtent));
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
    const vec3 surfaceToCamera =
      frame.cameraPosition.xyz - worldPosition;
    const float surfaceToCameraLengthSquared =
      dot(surfaceToCamera, surfaceToCamera);
    const vec3 viewBiasDirection =
      surfaceToCameraLengthSquared > 0.000001
        ? surfaceToCamera * inversesqrt(surfaceToCameraLengthSquared)
        : vec3(0.0);
    tracePosition = worldPosition +
      viewBiasDirection * globalIlluminationHeader.volumeMax.w;
    return true;
  }

  void main()
  {
    const ivec2 outputExtent = imageSize(probeCellIndices);
    const ivec2 depthExtent = textureSize(depthSampler, 0);
    const ivec2 pixel = ivec2(gl_GlobalInvocationID.xy);
    const bool bPixelInBounds = all(lessThan(pixel, outputExtent));
    const bool bRepresentative =
      gl_LocalInvocationIndex == ResolveRepresentativeInvocation();

    if(bRepresentative)
    {
      uvec4 encodedBrickIndices = uvec4(0u);
      uint brickCount = 0u;
      if(bPixelInBounds)
      {
        // Seed the workgroup from the first visible subpixel. Other subpixels
        // and invocations reuse these candidates when strict containment
        // proves that the same adaptive brick applies.
        for(uint subpixelIndex = 0u; subpixelIndex < 4u; ++subpixelIndex)
        {
          vec3 tracePosition = vec3(0.0);
          const ivec2 depthPixel = ResolveDepthPixel(
            pixel,
            subpixelIndex,
            outputExtent,
            depthExtent);
          if(ResolveTracePosition(
            depthPixel,
            depthExtent,
            tracePosition) && FindProbeCellCandidates(
              tracePosition,
              encodedBrickIndices,
              brickCount))
          {
            break;
          }
        }
      }
      resolveRepresentativeEncodedBrickIndices = encodedBrickIndices;
      resolveRepresentativeBrickCount = brickCount;
    }
    barrier();

    if(!bPixelInBounds)
    {
      return;
    }

    uvec4 resolvedEncodedBrickIndices = uvec4(0u);
    uvec4 localEncodedBrickIndices =
      resolveRepresentativeEncodedBrickIndices;
    uint localBrickCount = resolveRepresentativeBrickCount;
    for(uint subpixelIndex = 0u; subpixelIndex < 4u; ++subpixelIndex)
    {
      vec3 tracePosition = vec3(0.0);
      const ivec2 depthPixel = ResolveDepthPixel(
        pixel,
        subpixelIndex,
        outputExtent,
        depthExtent);
      if(!ResolveTracePosition(depthPixel, depthExtent, tracePosition))
      {
        continue;
      }

      uint selectedEncodedBrickIndex = 0u;
      bool bFoundContainedCandidate = false;
      for(uint candidateIndex = 0u;
        candidateIndex < localBrickCount;
        ++candidateIndex)
      {
        if(TrySelectProbeCellCandidate(
          tracePosition,
          localEncodedBrickIndices[candidateIndex],
          selectedEncodedBrickIndex))
        {
          bFoundContainedCandidate = true;
          break;
        }
      }

      if(!bFoundContainedCandidate && FindProbeCellCandidates(
        tracePosition,
        localEncodedBrickIndices,
        localBrickCount))
      {
        uint selectedBrickIndex = 0u;
        if(SelectProbeCell(
          tracePosition,
          localEncodedBrickIndices,
          localBrickCount,
          selectedBrickIndex))
        {
          selectedEncodedBrickIndex = selectedBrickIndex + 1u;
        }
      }
      resolvedEncodedBrickIndices[subpixelIndex] =
        selectedEncodedBrickIndex;
    }

    // R32 floats preserve every practical brick index exactly. Four channels
    // map directly to the four full-resolution quadrants of this half-res
    // texel, so the material pass never upscales across depth layers.
    imageStore(
      probeCellIndices,
      pixel,
      vec4(resolvedEncodedBrickIndices));
  }
