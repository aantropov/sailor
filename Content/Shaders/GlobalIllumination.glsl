#if defined(FRAGMENT) || defined(COMPUTE)

const uint GLOBAL_ILLUMINATION_LEAF_BIT = 0x80000000u;
const uint GLOBAL_ILLUMINATION_MAX_TRAVERSAL_STEPS = 128u;
const uint GLOBAL_ILLUMINATION_MAX_BVH_STACK = 32u;

const uint GLOBAL_ILLUMINATION_DEBUG_LIT = 0u;
const uint GLOBAL_ILLUMINATION_DEBUG_INDIRECT_ONLY = 1u;
const uint GLOBAL_ILLUMINATION_DEBUG_PROBES = 2u;
const uint GLOBAL_ILLUMINATION_DEBUG_BRICKS = 3u;
const uint GLOBAL_ILLUMINATION_DEBUG_VALIDITY = 4u;
const uint GLOBAL_ILLUMINATION_DEBUG_VISIBILITY = 5u;
const uint GLOBAL_ILLUMINATION_DEBUG_RESIDENCY = 6u;
const uint GLOBAL_ILLUMINATION_DEBUG_ASSET_IDENTITY = 7u;
const uint GLOBAL_ILLUMINATION_DEBUG_FALLBACK = 8u;

const uint GLOBAL_ILLUMINATION_MODE_REALTIME = 0u;
const uint GLOBAL_ILLUMINATION_MODE_REALTIME_AND_BAKED = 1u;
const uint GLOBAL_ILLUMINATION_MODE_BAKED_ONLY = 2u;
const float GLOBAL_ILLUMINATION_SURFACE_PLANE_TOLERANCE = 0.0001;
const float GLOBAL_ILLUMINATION_NEIGHBOR_SUPPORT_SCALE = 0.5;

struct GlobalIlluminationBvhNode
{
  vec4 minAndLeft;
  vec4 maxAndRight;
};

struct GlobalIlluminationBrick
{
  vec4 minAndSubdivision;
  vec4 maxAndFirstProbe;
  uvec4 probeCountsAndValidCount;
};

struct GlobalIlluminationProbe
{
  vec4 positionAndValidity;
  vec4 environmentVisibility0123;
  vec4 environmentVisibility45;
};

struct GlobalIlluminationCoefficients
{
  uvec4 packed[4];
};

struct GlobalIlluminationShBasis
{
  vec4 values0123;
  vec4 values4567;
  float value8;
};

struct GlobalIlluminationState
{
  vec4 parameters;
  uvec4 identity;
};

layout(std430, set = 1, binding = 12) readonly buffer GlobalIlluminationHeaderSSBO
{
  uvec4 counts;
  uvec4 stateAndDebug;
  uvec4 settings;
  vec4 volumeMin;
  vec4 volumeMax;
  uvec4 identity;
} globalIlluminationHeader;

#ifdef COMPUTE
layout(std430, set = 1, binding = 13) readonly buffer GlobalIlluminationBvhSSBO
{
  GlobalIlluminationBvhNode instance[];
} globalIlluminationBvh;
#endif

#if defined(COMPUTE) || defined(FRAGMENT)
layout(std430, set = 1, binding = 14) readonly buffer GlobalIlluminationBricksSSBO
{
  GlobalIlluminationBrick instance[];
} globalIlluminationBricks;
#endif

#ifdef FRAGMENT
layout(std430, set = 1, binding = 15) readonly buffer GlobalIlluminationProbesSSBO
{
  GlobalIlluminationProbe instance[];
} globalIlluminationProbes;

layout(std430, set = 1, binding = 16) readonly buffer GlobalIlluminationCoefficientsSSBO
{
  GlobalIlluminationCoefficients instance[];
} globalIlluminationCoefficients;

layout(std430, set = 1, binding = 17) readonly buffer GlobalIlluminationStatesSSBO
{
  GlobalIlluminationState instance[];
} globalIlluminationStates;
#endif

#ifdef FRAGMENT
layout(set = 1, binding = 18) uniform sampler2D g_globalIlluminationProbeCellIndicesSampler;
#endif

struct GlobalIlluminationProbeCell
{
  vec3 boundsMin;
  vec3 boundsMax;
  uvec3 probeCounts;
  uint firstProbeIndex;
  uint brickIndex;
  uint subdivision;
};

struct GlobalIlluminationSampleDebug
{
  bool usedProbeVolume;
  uint brickIndex;
  uint dominantProbeIndex;
  float averageValidity;
  float averageVisibility;
  float dominantStateHue;
};

void InitializeGlobalIlluminationSampleDebug(
  out GlobalIlluminationSampleDebug debugInfo)
{
  debugInfo.usedProbeVolume = false;
  debugInfo.brickIndex = 0u;
  debugInfo.dominantProbeIndex = 0u;
  debugInfo.averageValidity = 0.0;
  debugInfo.averageVisibility = 0.0;
  debugInfo.dominantStateHue = 0.0;
}

bool GlobalIlluminationDebugUsesProbeData()
{
  return globalIlluminationHeader.stateAndDebug.z >
    GLOBAL_ILLUMINATION_DEBUG_INDIRECT_ONLY;
}

float GlobalIlluminationNeighborSupportRadius()
{
  const float minProbeSpacing =
    uintBitsToFloat(globalIlluminationHeader.settings.z);
  if(isnan(minProbeSpacing) || isinf(minProbeSpacing) ||
    minProbeSpacing <= 0.0)
  {
    return 0.0;
  }
  return minProbeSpacing * GLOBAL_ILLUMINATION_NEIGHBOR_SUPPORT_SCALE +
    max(globalIlluminationHeader.volumeMin.w, 0.0) +
    max(globalIlluminationHeader.volumeMax.w, 0.0);
}

float GlobalIlluminationBoundsTolerance(vec3 boundsMin, vec3 boundsMax)
{
  const vec3 maximumAbsoluteCoordinate = max(abs(boundsMin), abs(boundsMax));
  const float coordinateScale = max(
    1.0,
    max(
      maximumAbsoluteCoordinate.x,
      max(maximumAbsoluteCoordinate.y, maximumAbsoluteCoordinate.z)));
  return 0.00001 + coordinateScale * 0.000001;
}

bool GlobalIlluminationContains(vec3 boundsMin, vec3 boundsMax, vec3 position)
{
  const float tolerance =
    GlobalIlluminationBoundsTolerance(boundsMin, boundsMax);
  return all(greaterThanEqual(position, boundsMin - vec3(tolerance))) &&
    all(lessThanEqual(position, boundsMax + vec3(tolerance)));
}

bool GlobalIlluminationContainsExact(
  vec3 boundsMin,
  vec3 boundsMax,
  vec3 position)
{
  return all(greaterThanEqual(position, boundsMin)) &&
    all(lessThanEqual(position, boundsMax));
}

float GlobalIlluminationDistanceSquaredToBounds(
  vec3 position,
  vec3 boundsMin,
  vec3 boundsMax)
{
  const vec3 delta = max(
    max(boundsMin - position, position - boundsMax),
    vec3(0.0));
  return dot(delta, delta);
}

uint GlobalIlluminationFlattenIndex(uvec3 coordinate, uvec3 counts)
{
  return coordinate.x + counts.x * (coordinate.y + counts.y * coordinate.z);
}

float GlobalIlluminationEnvironmentVisibility(
  vec3 positiveDirectionVisibility,
  vec3 negativeDirectionVisibility,
  vec3 sourceDirection)
{
  const vec3 direction = length(sourceDirection) > 0.000001
    ? normalize(sourceDirection)
    : vec3(0.0, 1.0, 0.0);
  const vec3 axisWeights = abs(direction);
  const float totalAxisWeight =
    axisWeights.x + axisWeights.y + axisWeights.z;
  const vec3 directionalVisibility = vec3(
    direction.x >= 0.0
      ? positiveDirectionVisibility.x
      : negativeDirectionVisibility.x,
    direction.y >= 0.0
      ? positiveDirectionVisibility.y
      : negativeDirectionVisibility.y,
    direction.z >= 0.0
      ? positiveDirectionVisibility.z
      : negativeDirectionVisibility.z);
  return clamp(dot(
    axisWeights,
    directionalVisibility) / totalAxisWeight,
    0.0,
    1.0);
}

float GlobalIlluminationSurfaceFacingWeight(
  GlobalIlluminationProbe probe,
  vec3 worldPosition,
  vec3 worldNormal)
{
  const vec3 surfaceToProbe = probe.positionAndValidity.xyz - worldPosition;
  const float signedPlaneDistance = dot(worldNormal, surfaceToProbe);
  const float transitionWidth = max(
    globalIlluminationHeader.volumeMin.w,
    GLOBAL_ILLUMINATION_SURFACE_PLANE_TOLERANCE);
  return smoothstep(
    -transitionWidth,
    transitionWidth,
    signedPlaneDistance);
}

#ifdef COMPUTE
bool GlobalIlluminationFindBrickCandidates(
  vec3 worldPosition,
  float queryRadius,
  out uvec4 encodedBrickIndices,
  out uint selectedBrickCount)
{
  encodedBrickIndices = uvec4(0u);
  selectedBrickCount = 0u;
  if(globalIlluminationHeader.counts.x == 0u ||
    globalIlluminationHeader.counts.y == 0u ||
    globalIlluminationHeader.counts.z == 0u)
  {
    return false;
  }

  uint stack[GLOBAL_ILLUMINATION_MAX_BVH_STACK];
  uint stackSize = 1u;
  stack[0] = globalIlluminationHeader.stateAndDebug.y;
  uint steps = 0u;
  const float queryTolerance = GlobalIlluminationBoundsTolerance(
    globalIlluminationHeader.volumeMin.xyz,
    globalIlluminationHeader.volumeMax.xyz);
  const float expandedQueryRadius = queryRadius + queryTolerance;
  const float queryRadiusSquared =
    expandedQueryRadius * expandedQueryRadius;
  vec4 selectedDistanceSquared = vec4(3.402823466e+38);
  uvec4 selectedSubdivisions = uvec4(0u);
  while(stackSize > 0u && steps < GLOBAL_ILLUMINATION_MAX_TRAVERSAL_STEPS)
  {
    const uint nodeIndex = stack[--stackSize];
    ++steps;
    if(nodeIndex >= globalIlluminationHeader.counts.y ||
      nodeIndex >= uint(globalIlluminationBvh.instance.length()))
    {
      continue;
    }
    const GlobalIlluminationBvhNode node =
      globalIlluminationBvh.instance[nodeIndex];
    if(GlobalIlluminationDistanceSquaredToBounds(
      worldPosition,
      node.minAndLeft.xyz,
      node.maxAndRight.xyz) > queryRadiusSquared)
    {
      continue;
    }

    const uint leftOrLeaf = floatBitsToUint(node.minAndLeft.w);
    if((leftOrLeaf & GLOBAL_ILLUMINATION_LEAF_BIT) != 0u)
    {
      const uint brickIndex = leftOrLeaf & ~GLOBAL_ILLUMINATION_LEAF_BIT;
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
      const uint subdivision = floatBitsToUint(brick.minAndSubdivision.w);
      const float distanceSquared =
        GlobalIlluminationDistanceSquaredToBounds(
          worldPosition,
          brick.minAndSubdivision.xyz,
          brick.maxAndFirstProbe.xyz);
      if(distanceSquared > queryRadiusSquared)
      {
        continue;
      }

      uint insertionIndex = 0u;
      if(selectedBrickCount < 4u)
      {
        insertionIndex = selectedBrickCount++;
      }
      else
      {
        uint worstIndex = 0u;
        for(uint candidateIndex = 1u; candidateIndex < 4u; ++candidateIndex)
        {
          const bool bCandidateIsWorse =
            selectedDistanceSquared[candidateIndex] >
              selectedDistanceSquared[worstIndex] ||
            (abs(
              selectedDistanceSquared[candidateIndex] -
                selectedDistanceSquared[worstIndex]) <= 0.0000001 &&
              selectedSubdivisions[candidateIndex] <
                selectedSubdivisions[worstIndex]);
          if(bCandidateIsWorse)
          {
            worstIndex = candidateIndex;
          }
        }
        const bool bCandidateIsBetter =
          distanceSquared < selectedDistanceSquared[worstIndex] ||
          (abs(distanceSquared - selectedDistanceSquared[worstIndex]) <=
              0.0000001 &&
            subdivision > selectedSubdivisions[worstIndex]);
        if(!bCandidateIsBetter)
        {
          continue;
        }
        insertionIndex = worstIndex;
      }
      encodedBrickIndices[insertionIndex] = brickIndex + 1u;
      selectedDistanceSquared[insertionIndex] = distanceSquared;
      selectedSubdivisions[insertionIndex] = subdivision;
      continue;
    }

    if(stackSize + 2u > GLOBAL_ILLUMINATION_MAX_BVH_STACK)
    {
      break;
    }
    stack[stackSize++] = floatBitsToUint(node.maxAndRight.w);
    stack[stackSize++] = leftOrLeaf;
  }
  return selectedBrickCount > 0u;
}
#endif

#ifdef FRAGMENT
GlobalIlluminationShBasis GlobalIlluminationBuildShBasis(vec3 normal)
{
  const float x = normal.x;
  const float y = normal.y;
  const float z = normal.z;
  GlobalIlluminationShBasis basis;
  basis.values0123 = vec4(
    0.2820947918,
    0.4886025119 * y,
    0.4886025119 * z,
    0.4886025119 * x);
  basis.values4567 = vec4(
    1.0925484306 * x * y,
    1.0925484306 * y * z,
    0.3153915653 * (3.0 * z * z - 1.0),
    1.0925484306 * x * z);
  basis.value8 = 0.5462742153 * (x * x - y * y);
  return basis;
}

vec3 GlobalIlluminationEvaluateProbe(
  uint stateIndex,
  uint probeIndex,
  GlobalIlluminationShBasis basis)
{
  const uint coefficientArrayIndex =
    stateIndex * globalIlluminationHeader.counts.w + probeIndex;
  if(coefficientArrayIndex >=
    uint(globalIlluminationCoefficients.instance.length()))
  {
    return vec3(0.0);
  }
  // Load the complete packed record once. The previous per-component helper
  // could issue 27 dynamic SSBO reads for the same probe/state pair.
  const GlobalIlluminationCoefficients coefficients =
    globalIlluminationCoefficients.instance[coefficientArrayIndex];

  vec3 irradiance = vec3(0.0);
  const vec2 pair0 = unpackHalf2x16(coefficients.packed[0].x);
  const vec2 pair1 = unpackHalf2x16(coefficients.packed[0].y);
  irradiance += vec3(pair0, pair1.x) * basis.values0123.x;
  const vec2 pair2 = unpackHalf2x16(coefficients.packed[0].z);
  irradiance += vec3(pair1.y, pair2) * basis.values0123.y;
  const vec2 pair3 = unpackHalf2x16(coefficients.packed[0].w);
  const vec2 pair4 = unpackHalf2x16(coefficients.packed[1].x);
  irradiance += vec3(pair3, pair4.x) * basis.values0123.z;
  const vec2 pair5 = unpackHalf2x16(coefficients.packed[1].y);
  irradiance += vec3(pair4.y, pair5) * basis.values0123.w;
  const vec2 pair6 = unpackHalf2x16(coefficients.packed[1].z);
  const vec2 pair7 = unpackHalf2x16(coefficients.packed[1].w);
  irradiance += vec3(pair6, pair7.x) * basis.values4567.x;
  const vec2 pair8 = unpackHalf2x16(coefficients.packed[2].x);
  irradiance += vec3(pair7.y, pair8) * basis.values4567.y;
  const vec2 pair9 = unpackHalf2x16(coefficients.packed[2].y);
  const vec2 pair10 = unpackHalf2x16(coefficients.packed[2].z);
  irradiance += vec3(pair9, pair10.x) * basis.values4567.z;
  const vec2 pair11 = unpackHalf2x16(coefficients.packed[2].w);
  irradiance += vec3(pair10.y, pair11) * basis.values4567.w;
  const vec2 pair12 = unpackHalf2x16(coefficients.packed[3].x);
  const vec2 pair13 = unpackHalf2x16(coefficients.packed[3].y);
  irradiance += vec3(pair12, pair13.x) * basis.value8;
  return irradiance;
}

vec3 GlobalIlluminationEvaluateProbeStates(
  uint probeIndex,
  GlobalIlluminationShBasis basis,
  uint stateCount,
  float singleStateWeight)
{
  if(stateCount == 1u)
  {
    return GlobalIlluminationEvaluateProbe(
      0u,
      probeIndex,
      basis) * singleStateWeight;
  }

  vec3 irradiance = vec3(0.0);
  for(uint stateIndex = 0u; stateIndex < stateCount; ++stateIndex)
  {
    const float stateWeight =
      globalIlluminationStates.instance[stateIndex].parameters.x;
    irradiance += GlobalIlluminationEvaluateProbe(
      stateIndex,
      probeIndex,
      basis) * stateWeight;
  }
  return irradiance;
}

bool SampleGlobalIlluminationFromProbeCell(
  GlobalIlluminationProbeCell probeCell,
  vec3 worldPosition,
  vec3 shadingNormal,
  vec3 geometricNormal,
  vec3 samplingPosition,
  vec3 environmentDirection,
  out vec3 irradiance,
  out float environmentVisibility,
  out GlobalIlluminationSampleDebug debugInfo)
{
  irradiance = vec3(0.0);
  environmentVisibility = 1.0;
  vec3 positiveDirectionVisibility = vec3(0.0);
  vec3 negativeDirectionVisibility = vec3(0.0);
  vec3 interpolationPositiveDirectionVisibility = vec3(0.0);
  vec3 interpolationNegativeDirectionVisibility = vec3(0.0);
  InitializeGlobalIlluminationSampleDebug(debugInfo);

  if(globalIlluminationHeader.counts.x == 0u ||
    globalIlluminationHeader.stateAndDebug.x == 0u ||
    probeCell.brickIndex >= globalIlluminationHeader.counts.z ||
    probeCell.firstProbeIndex >= globalIlluminationHeader.counts.w)
  {
    return false;
  }

  const GlobalIlluminationShBasis shBasis =
    GlobalIlluminationBuildShBasis(shadingNormal);
  const uint debugMode = globalIlluminationHeader.stateAndDebug.z;
  const bool bTrackProbe =
    debugMode == GLOBAL_ILLUMINATION_DEBUG_PROBES;
  const bool bTrackValidity =
    debugMode == GLOBAL_ILLUMINATION_DEBUG_VALIDITY;
  const bool bTrackVisibility =
    debugMode == GLOBAL_ILLUMINATION_DEBUG_VISIBILITY;
  const bool bTrackState =
    debugMode == GLOBAL_ILLUMINATION_DEBUG_ASSET_IDENTITY;
  if(debugMode == GLOBAL_ILLUMINATION_DEBUG_BRICKS)
  {
    debugInfo.brickIndex = probeCell.brickIndex;
  }
  const vec3 extent = probeCell.boundsMax - probeCell.boundsMin;
  const uvec3 probeCounts = probeCell.probeCounts;

  const vec3 normalizedPosition = clamp(
    (samplingPosition - probeCell.boundsMin) / extent,
    vec3(0.0),
    vec3(1.0));
  const vec3 cell = normalizedPosition * vec3(max(probeCounts, uvec3(1u)) - uvec3(1u));
  const uvec3 lower = uvec3(floor(cell));
  const uvec3 upper = min(lower + uvec3(1u), probeCounts - uvec3(1u));
  const vec3 fraction = fract(cell);
  const uint firstProbeIndex = probeCell.firstProbeIndex;

  float totalInterpolationWeight = 0.0;
  float totalVisibleWeight = 0.0;
  float dominantProbeWeight = -1.0;
  const uint stateCount = min(
    globalIlluminationHeader.stateAndDebug.x,
    uint(globalIlluminationStates.instance.length()));
  const float singleStateWeight = stateCount == 1u
    ? globalIlluminationStates.instance[0].parameters.x
    : 0.0;
  if(bTrackState)
  {
    float dominantStateWeight = -1.0;
    for(uint stateIndex = 0u; stateIndex < stateCount; ++stateIndex)
    {
      const float stateWeight =
        globalIlluminationStates.instance[stateIndex].parameters.x;
      if(stateWeight > dominantStateWeight)
      {
        dominantStateWeight = stateWeight;
        debugInfo.dominantStateHue =
          globalIlluminationStates.instance[stateIndex].parameters.z;
      }
    }
  }

  for(uint z = 0u; z < 2u; ++z)
  {
    for(uint y = 0u; y < 2u; ++y)
    {
      for(uint x = 0u; x < 2u; ++x)
      {
        const uvec3 coordinate = uvec3(
          x != 0u ? upper.x : lower.x,
          y != 0u ? upper.y : lower.y,
          z != 0u ? upper.z : lower.z);
        const uint probeIndex = firstProbeIndex +
          GlobalIlluminationFlattenIndex(coordinate, probeCounts);
        if(probeIndex >= globalIlluminationHeader.counts.w ||
          probeIndex >= uint(globalIlluminationProbes.instance.length()))
        {
          return false;
        }
        const GlobalIlluminationProbe probe =
          globalIlluminationProbes.instance[probeIndex];
        const float trilinearWeight =
          (x != 0u ? fraction.x : 1.0 - fraction.x) *
          (y != 0u ? fraction.y : 1.0 - fraction.y) *
          (z != 0u ? fraction.z : 1.0 - fraction.z);
        const float validity = clamp(probe.positionAndValidity.w, 0.0, 1.0);
        const float interpolationWeight = trilinearWeight * validity;
        const float surfaceFacingWeight =
          GlobalIlluminationSurfaceFacingWeight(
            probe,
            worldPosition,
            geometricNormal);
        // Six signed-axis clearance rays cannot describe the finite extent of
        // scene occluders. Extending any hit into a runtime rejection plane
        // creates camera-moving rectangular holes. The baked SH already carries
        // diffuse occlusion, so runtime visibility is receiver-side selection.
        const float visibility = surfaceFacingWeight;
        const float spatialWeight = interpolationWeight * visibility;
        const vec3 probePositiveDirectionVisibility = vec3(
          probe.environmentVisibility0123.x,
          probe.environmentVisibility0123.z,
          probe.environmentVisibility45.x);
        const vec3 probeNegativeDirectionVisibility = vec3(
          probe.environmentVisibility0123.y,
          probe.environmentVisibility0123.w,
          probe.environmentVisibility45.y);
        totalInterpolationWeight += interpolationWeight;
        totalVisibleWeight += spatialWeight;
        positiveDirectionVisibility +=
          probePositiveDirectionVisibility * spatialWeight;
        negativeDirectionVisibility +=
          probeNegativeDirectionVisibility * spatialWeight;
        interpolationPositiveDirectionVisibility +=
          probePositiveDirectionVisibility * interpolationWeight;
        interpolationNegativeDirectionVisibility +=
          probeNegativeDirectionVisibility * interpolationWeight;
        if(bTrackProbe && spatialWeight > dominantProbeWeight)
        {
          dominantProbeWeight = spatialWeight;
          debugInfo.dominantProbeIndex = probeIndex;
        }
        if(bTrackValidity)
        {
          debugInfo.averageValidity += trilinearWeight * validity;
        }
        if(bTrackVisibility)
        {
          debugInfo.averageVisibility += trilinearWeight * visibility;
        }
        if(spatialWeight > 0.0)
        {
          irradiance += GlobalIlluminationEvaluateProbeStates(
            probeIndex,
            shBasis,
            stateCount,
            singleStateWeight) * spatialWeight;
        }
      }
    }
  }

  if(totalInterpolationWeight <= 0.000001)
  {
    return false;
  }
  const float receiverCoverage = clamp(
    totalVisibleWeight / totalInterpolationWeight,
    0.0,
    1.0);
  const float receiverBlend = smoothstep(0.0, 0.05, receiverCoverage);
  vec3 unfilteredIrradiance = vec3(0.0);
  if(receiverBlend < 1.0 || totalVisibleWeight <= 0.000001)
  {
    for(uint z = 0u; z < 2u; ++z)
    {
      for(uint y = 0u; y < 2u; ++y)
      {
        for(uint x = 0u; x < 2u; ++x)
        {
          const uvec3 coordinate = uvec3(
            x != 0u ? upper.x : lower.x,
            y != 0u ? upper.y : lower.y,
            z != 0u ? upper.z : lower.z);
          const uint probeIndex = firstProbeIndex +
            GlobalIlluminationFlattenIndex(coordinate, probeCounts);
          const float trilinearWeight =
            (x != 0u ? fraction.x : 1.0 - fraction.x) *
            (y != 0u ? fraction.y : 1.0 - fraction.y) *
            (z != 0u ? fraction.z : 1.0 - fraction.z);
          const float validity = clamp(
            globalIlluminationProbes.instance[probeIndex]
              .positionAndValidity.w,
            0.0,
            1.0);
          const float interpolationWeight = trilinearWeight * validity;
          if(interpolationWeight > 0.0)
          {
            unfilteredIrradiance += GlobalIlluminationEvaluateProbeStates(
              probeIndex,
              shBasis,
              stateCount,
              singleStateWeight) * interpolationWeight;
          }
        }
      }
    }
    unfilteredIrradiance /= totalInterpolationWeight;
  }
  const vec3 unfilteredPositiveDirectionVisibility =
    interpolationPositiveDirectionVisibility / totalInterpolationWeight;
  const vec3 unfilteredNegativeDirectionVisibility =
    interpolationNegativeDirectionVisibility / totalInterpolationWeight;
  vec3 receiverIrradiance = unfilteredIrradiance;
  vec3 receiverPositiveDirectionVisibility =
    unfilteredPositiveDirectionVisibility;
  vec3 receiverNegativeDirectionVisibility =
    unfilteredNegativeDirectionVisibility;
  if(totalVisibleWeight > 0.000001)
  {
    receiverIrradiance = irradiance / totalVisibleWeight;
    receiverPositiveDirectionVisibility =
      positiveDirectionVisibility / totalVisibleWeight;
    receiverNegativeDirectionVisibility =
      negativeDirectionVisibility / totalVisibleWeight;
  }
  // Surface-facing weights select probes on the receiver side. Validity-only
  // irradiance is evaluated lazily only inside the narrow low-coverage fade;
  // ordinary receivers avoid loading and decoding probes with zero spatial
  // weight while retaining the exact boundary fallback.
  irradiance = max(mix(
    unfilteredIrradiance,
    receiverIrradiance,
    receiverBlend), vec3(0.0));
  positiveDirectionVisibility = clamp(mix(
    unfilteredPositiveDirectionVisibility,
    receiverPositiveDirectionVisibility,
    receiverBlend),
    vec3(0.0),
    vec3(1.0));
  negativeDirectionVisibility = clamp(mix(
    unfilteredNegativeDirectionVisibility,
    receiverNegativeDirectionVisibility,
    receiverBlend),
    vec3(0.0),
    vec3(1.0));
  environmentVisibility = GlobalIlluminationEnvironmentVisibility(
    positiveDirectionVisibility,
    negativeDirectionVisibility,
    environmentDirection);
  debugInfo.usedProbeVolume = true;
  return true;
}
#endif

vec3 GlobalIlluminationHueToRgb(float hue)
{
  const vec3 shifted = fract(hue + vec3(0.0, 0.6666667, 0.3333333));
  return clamp(abs(shifted * 6.0 - 3.0) - 1.0, 0.0, 1.0);
}

vec3 ApplyGlobalIlluminationDebug(
  vec3 indirectLighting,
  GlobalIlluminationSampleDebug debugInfo)
{
  const uint mode = globalIlluminationHeader.stateAndDebug.z;
  if(mode == GLOBAL_ILLUMINATION_DEBUG_LIT ||
    mode == GLOBAL_ILLUMINATION_DEBUG_INDIRECT_ONLY)
  {
    return indirectLighting;
  }
  if(mode == GLOBAL_ILLUMINATION_DEBUG_PROBES)
  {
    const float hue = fract(float(debugInfo.dominantProbeIndex) * 0.61803398875);
    return debugInfo.usedProbeVolume ? GlobalIlluminationHueToRgb(hue) : vec3(0.02);
  }
  if(mode == GLOBAL_ILLUMINATION_DEBUG_BRICKS)
  {
    const float hue = fract(float(debugInfo.brickIndex) * 0.754877666);
    return debugInfo.usedProbeVolume ? GlobalIlluminationHueToRgb(hue) : vec3(0.02);
  }
  if(mode == GLOBAL_ILLUMINATION_DEBUG_VALIDITY)
  {
    return vec3(clamp(debugInfo.averageValidity, 0.0, 1.0));
  }
  if(mode == GLOBAL_ILLUMINATION_DEBUG_VISIBILITY)
  {
    return vec3(clamp(debugInfo.averageVisibility, 0.0, 1.0));
  }
  if(mode == GLOBAL_ILLUMINATION_DEBUG_RESIDENCY)
  {
    return globalIlluminationHeader.counts.x != 0u
      ? vec3(0.1, 0.8, 0.2)
      : vec3(0.8, 0.1, 0.1);
  }
  if(mode == GLOBAL_ILLUMINATION_DEBUG_ASSET_IDENTITY)
  {
    return debugInfo.usedProbeVolume
      ? GlobalIlluminationHueToRgb(debugInfo.dominantStateHue)
      : vec3(0.02);
  }
  if(mode == GLOBAL_ILLUMINATION_DEBUG_FALLBACK)
  {
    return debugInfo.usedProbeVolume
      ? vec3(0.1, 0.8, 0.2)
      : vec3(0.9, 0.05, 0.7);
  }
  return indirectLighting;
}

#ifdef FRAGMENT
bool GlobalIlluminationProbeCellCandidateIsBetter(
  uint containmentRank,
  uint subdivision,
  float distanceSquared,
  float score,
  uint otherContainmentRank,
  uint otherSubdivision,
  float otherDistanceSquared,
  float otherScore)
{
  if(containmentRank != otherContainmentRank)
  {
    return containmentRank > otherContainmentRank;
  }
  if(abs(distanceSquared - otherDistanceSquared) > 0.000000000001)
  {
    return distanceSquared < otherDistanceSquared;
  }
  if(subdivision != otherSubdivision)
  {
    return subdivision > otherSubdivision;
  }
  return score > otherScore;
}

bool GlobalIlluminationDecodeProbeCell(
  uint encodedBrickIndex,
  out GlobalIlluminationProbeCell probeCell)
{
  probeCell.boundsMin = vec3(0.0);
  probeCell.boundsMax = vec3(0.0);
  probeCell.probeCounts = uvec3(0u);
  probeCell.firstProbeIndex = 0u;
  probeCell.brickIndex = 0u;
  probeCell.subdivision = 0u;
  const uint totalProbeCount = globalIlluminationHeader.counts.w;
  if(totalProbeCount == 0u || encodedBrickIndex == 0u ||
    encodedBrickIndex > globalIlluminationHeader.counts.z)
  {
    return false;
  }

  const uint brickIndex = encodedBrickIndex - 1u;
  if(brickIndex >= uint(globalIlluminationBricks.instance.length()))
  {
    return false;
  }
  const GlobalIlluminationBrick brick =
    globalIlluminationBricks.instance[brickIndex];
  const uint firstProbeIndex = floatBitsToUint(brick.maxAndFirstProbe.w);
  const uvec3 probeCounts = brick.probeCountsAndValidCount.xyz;
  if(brick.probeCountsAndValidCount.w == 0u ||
    any(lessThanEqual(
      brick.maxAndFirstProbe.xyz,
      brick.minAndSubdivision.xyz)) ||
    any(equal(probeCounts, uvec3(0u))) ||
    firstProbeIndex >= totalProbeCount ||
    probeCounts.x > totalProbeCount / probeCounts.y)
  {
    return false;
  }
  const uint probeCountXY = probeCounts.x * probeCounts.y;
  if(probeCountXY > totalProbeCount / probeCounts.z)
  {
    return false;
  }
  const uint cellProbeCount = probeCountXY * probeCounts.z;
  if(cellProbeCount > totalProbeCount - firstProbeIndex)
  {
    return false;
  }

  probeCell.boundsMin = brick.minAndSubdivision.xyz;
  probeCell.boundsMax = brick.maxAndFirstProbe.xyz;
  probeCell.probeCounts = probeCounts;
  probeCell.firstProbeIndex = firstProbeIndex;
  probeCell.brickIndex = brickIndex;
  probeCell.subdivision = floatBitsToUint(brick.minAndSubdivision.w);
  return true;
}

bool GlobalIlluminationReadSupportedProbeCell(
  uint encodedBrickIndex,
  vec3 samplingPosition,
  out GlobalIlluminationProbeCell probeCell,
  out uint containmentRank,
  out float distanceSquared)
{
  containmentRank = 0u;
  distanceSquared = 3.402823466e+38;
  if(!GlobalIlluminationDecodeProbeCell(
    encodedBrickIndex,
    probeCell))
  {
    return false;
  }

  distanceSquared = GlobalIlluminationDistanceSquaredToBounds(
    samplingPosition,
    probeCell.boundsMin,
    probeCell.boundsMax);
  const float neighborSupportRadius =
    GlobalIlluminationNeighborSupportRadius() +
    GlobalIlluminationBoundsTolerance(
      probeCell.boundsMin,
      probeCell.boundsMax);
  if(distanceSquared > neighborSupportRadius * neighborSupportRadius)
  {
    return false;
  }

  containmentRank = GlobalIlluminationContainsExact(
    probeCell.boundsMin,
    probeCell.boundsMax,
    samplingPosition)
    ? 2u
    : (GlobalIlluminationContains(
        probeCell.boundsMin,
        probeCell.boundsMax,
        samplingPosition) ? 1u : 0u);
  return true;
}

bool SelectGlobalIlluminationProbeCell(
  vec2 screenUv,
  vec3 samplingPosition,
  out GlobalIlluminationProbeCell selectedProbeCell)
{
  selectedProbeCell.boundsMin = vec3(0.0);
  selectedProbeCell.boundsMax = vec3(0.0);
  selectedProbeCell.probeCounts = uvec3(0u);
  selectedProbeCell.firstProbeIndex = 0u;
  selectedProbeCell.brickIndex = 0u;
  selectedProbeCell.subdivision = 0u;
  const ivec2 cellExtent =
    textureSize(g_globalIlluminationProbeCellIndicesSampler, 0);
  if(globalIlluminationHeader.counts.z == 0u ||
    any(lessThan(cellExtent, ivec2(2))))
  {
    // Passes without the authoritative probe-cell set bind one-pixel defaults
    // and preserve environment IBL.
    return false;
  }

  const vec2 clampedUv = clamp(
    screenUv,
    vec2(0.0),
    vec2(0.99999994));
  const vec2 texelPosition = clampedUv * vec2(cellExtent);
  const ivec2 primaryPixel = clamp(
    ivec2(floor(texelPosition)),
    ivec2(0),
    cellExtent - ivec2(1));
  const vec2 subpixelPosition = fract(texelPosition);
  const uint primaryChannel =
    (subpixelPosition.x >= 0.5 ? 1u : 0u) +
    (subpixelPosition.y >= 0.5 ? 2u : 0u);
  const uvec4 primaryEncodedBrickIndices = uvec4(
    max(texelFetch(
      g_globalIlluminationProbeCellIndicesSampler,
      primaryPixel,
      0), vec4(0.0)) + vec4(0.5));
  bool foundProbeCell = false;
  uint selectedContainmentRank = 0u;
  uint selectedSubdivision = 0u;
  float selectedDistanceSquared = 3.402823466e+38;
  float selectedScore = -1.0;

  GlobalIlluminationProbeCell primaryProbeCell;
  uint primaryContainmentRank = 0u;
  float primaryDistanceSquared = 3.402823466e+38;
  if(GlobalIlluminationReadSupportedProbeCell(
    primaryEncodedBrickIndices[primaryChannel],
    samplingPosition,
    primaryProbeCell,
    primaryContainmentRank,
    primaryDistanceSquared))
  {
    // The common path is the exact brick selected from this fragment's own
    // full-resolution depth sample, packed into one channel of a half-res
    // texel. It never interpolates selection across depth layers.
    if(primaryContainmentRank == 2u)
    {
      selectedProbeCell = primaryProbeCell;
      return true;
    }
    foundProbeCell = true;
    selectedProbeCell = primaryProbeCell;
    selectedContainmentRank = primaryContainmentRank;
    selectedSubdivision = primaryProbeCell.subdivision;
    selectedDistanceSquared = primaryDistanceSquared;
    selectedScore = 1.0;
  }

  // Geometric-normal bias can move a receiver over an adaptive brick boundary.
  // The other subpixel channels are already resident in the same cache line,
  // so prefer a containing neighbor before accepting a supported near cell.
  for(uint candidateChannel = 0u; candidateChannel < 4u; ++candidateChannel)
  {
    if(candidateChannel == primaryChannel ||
      primaryEncodedBrickIndices[candidateChannel] ==
        primaryEncodedBrickIndices[primaryChannel])
    {
      continue;
    }
    GlobalIlluminationProbeCell probeCell;
    uint containmentRank = 0u;
    float distanceSquared = 3.402823466e+38;
    if(!GlobalIlluminationReadSupportedProbeCell(
      primaryEncodedBrickIndices[candidateChannel],
      samplingPosition,
      probeCell,
      containmentRank,
      distanceSquared))
    {
      continue;
    }
    if(containmentRank == 2u)
    {
      selectedProbeCell = probeCell;
      return true;
    }
    if(!foundProbeCell || GlobalIlluminationProbeCellCandidateIsBetter(
      containmentRank,
      probeCell.subdivision,
      distanceSquared,
      0.5,
      selectedContainmentRank,
      selectedSubdivision,
      selectedDistanceSquared,
      selectedScore))
    {
      foundProbeCell = true;
      selectedProbeCell = probeCell;
      selectedContainmentRank = containmentRank;
      selectedSubdivision = probeCell.subdivision;
      selectedDistanceSquared = distanceSquared;
      selectedScore = 0.5;
    }
  }

  if(foundProbeCell && selectedContainmentRank > 0u)
  {
    return true;
  }

  // A masked depth/main-pass mismatch can still leave this exact channel
  // empty. Search one packed texel around it only on that exceptional path.
  for(int y = -1; y <= 1; ++y)
  {
    for(int x = -1; x <= 1; ++x)
    {
      if(x == 0 && y == 0)
      {
        continue;
      }
      const ivec2 pixel = clamp(
        primaryPixel + ivec2(x, y),
        ivec2(0),
        cellExtent - ivec2(1));
      const uvec4 encodedBrickIndices = uvec4(
        max(texelFetch(
          g_globalIlluminationProbeCellIndicesSampler,
          pixel,
          0), vec4(0.0)) + vec4(0.5));
      for(uint candidateChannel = 0u;
        candidateChannel < 4u;
        ++candidateChannel)
      {
        GlobalIlluminationProbeCell probeCell;
        uint containmentRank = 0u;
        float distanceSquared = 3.402823466e+38;
        if(!GlobalIlluminationReadSupportedProbeCell(
          encodedBrickIndices[candidateChannel],
          samplingPosition,
          probeCell,
          containmentRank,
          distanceSquared))
        {
          continue;
        }
        if(containmentRank == 2u)
        {
          selectedProbeCell = probeCell;
          return true;
        }
        if(!foundProbeCell || GlobalIlluminationProbeCellCandidateIsBetter(
          containmentRank,
          probeCell.subdivision,
          distanceSquared,
          0.0,
          selectedContainmentRank,
          selectedSubdivision,
          selectedDistanceSquared,
          selectedScore))
        {
          foundProbeCell = true;
          selectedProbeCell = probeCell;
          selectedContainmentRank = containmentRank;
          selectedSubdivision = probeCell.subdivision;
          selectedDistanceSquared = distanceSquared;
          selectedScore = 0.0;
        }
      }
    }
  }
  return foundProbeCell;
}

vec3 ResolveGlobalIlluminationDiffuseIrradiance(
  vec2 screenUv,
  vec3 worldPosition,
  vec3 worldShadingNormal,
  vec3 worldGeometricNormal,
  vec3 surfaceToCamera,
  vec3 environmentDirection,
  vec3 environmentIrradiance,
  out float environmentVisibility,
  out GlobalIlluminationSampleDebug debugInfo)
{
  environmentVisibility = 1.0;
  InitializeGlobalIlluminationSampleDebug(debugInfo);
  const bool bDebugUsesProbeData =
    GlobalIlluminationDebugUsesProbeData();
  if(!bDebugUsesProbeData &&
    globalIlluminationHeader.settings.x == 0u)
  {
    return environmentIrradiance;
  }
  const uint mode = globalIlluminationHeader.settings.y;
  if(!bDebugUsesProbeData &&
    mode == GLOBAL_ILLUMINATION_MODE_REALTIME)
  {
    return environmentIrradiance;
  }
  const float shadingNormalLengthSquared =
    dot(worldShadingNormal, worldShadingNormal);
  const vec3 shadingNormal = shadingNormalLengthSquared > 0.000001
    ? worldShadingNormal * inversesqrt(shadingNormalLengthSquared)
    : vec3(0.0, 1.0, 0.0);
  const float geometricNormalLengthSquared =
    dot(worldGeometricNormal, worldGeometricNormal);
  const vec3 geometricNormal = geometricNormalLengthSquared > 0.000001
    ? worldGeometricNormal * inversesqrt(geometricNormalLengthSquared)
    : shadingNormal;
  const float surfaceToCameraLengthSquared =
    dot(surfaceToCamera, surfaceToCamera);
  const vec3 viewBiasDirection = surfaceToCameraLengthSquared > 0.000001
    ? surfaceToCamera * inversesqrt(surfaceToCameraLengthSquared)
    : vec3(0.0);
  const vec3 samplingPosition = worldPosition +
    geometricNormal * globalIlluminationHeader.volumeMin.w +
    viewBiasDirection * globalIlluminationHeader.volumeMax.w;
  if(!GlobalIlluminationContains(
    globalIlluminationHeader.volumeMin.xyz,
    globalIlluminationHeader.volumeMax.xyz,
    samplingPosition))
  {
    return bDebugUsesProbeData
      ? ApplyGlobalIlluminationDebug(vec3(0.0), debugInfo)
      : environmentIrradiance;
  }
  GlobalIlluminationProbeCell probeCell;
  if(SelectGlobalIlluminationProbeCell(
    screenUv,
    samplingPosition,
    probeCell))
  {
    vec3 probeIrradiance = vec3(0.0);
    if(SampleGlobalIlluminationFromProbeCell(
      probeCell,
      worldPosition,
      shadingNormal,
      geometricNormal,
      samplingPosition,
      environmentDirection,
      probeIrradiance,
      environmentVisibility,
      debugInfo))
    {
      return bDebugUsesProbeData
        ? ApplyGlobalIlluminationDebug(probeIrradiance, debugInfo)
        : probeIrradiance;
    }
  }
  // Missing coarse selection preserves the pre-GI SkyComponent cubemap path.
  // No BVH traversal is performed by a material shader.
  environmentVisibility = 1.0;
  return bDebugUsesProbeData
    ? ApplyGlobalIlluminationDebug(vec3(0.0), debugInfo)
    : environmentIrradiance;
}

bool GlobalIlluminationDebugSuppressesDirectLighting()
{
  return globalIlluminationHeader.stateAndDebug.z !=
    GLOBAL_ILLUMINATION_DEBUG_LIT;
}

#endif
#endif
