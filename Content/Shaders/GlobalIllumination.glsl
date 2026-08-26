#ifdef FRAGMENT

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
const float GLOBAL_ILLUMINATION_BLOCKING_DISTANCE_TOLERANCE = 0.0001;
const float GLOBAL_ILLUMINATION_SURFACE_PLANE_TOLERANCE = 0.0001;
const uint GLOBAL_ILLUMINATION_BLOCKED_DIRECTION_SHIFT = 8u;
const uint GLOBAL_ILLUMINATION_BLOCKED_DIRECTION_MASK = 0x3f00u;

struct GlobalIlluminationBvhNode
{
  vec4 minAndLeft;
  vec4 maxAndRight;
};

struct GlobalIlluminationBrick
{
  vec4 minAndSubdivision;
  vec4 maxAndFirstProbe;
  uvec4 probeCountsAndCount;
};

struct GlobalIlluminationProbe
{
  vec4 positionAndValidity;
  vec4 visibility01;
  vec4 visibility23;
  vec4 visibility45;
  vec4 environmentVisibility0123;
  vec4 environmentVisibility45;
};

struct GlobalIlluminationCoefficients
{
  uvec4 packed[4];
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

layout(std430, set = 1, binding = 13) readonly buffer GlobalIlluminationBvhSSBO
{
  GlobalIlluminationBvhNode instance[];
} globalIlluminationBvh;

layout(std430, set = 1, binding = 14) readonly buffer GlobalIlluminationBricksSSBO
{
  GlobalIlluminationBrick instance[];
} globalIlluminationBricks;

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

struct GlobalIlluminationSampleDebug
{
  bool usedProbeVolume;
  bool traversalComplete;
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
  debugInfo.traversalComplete = true;
  debugInfo.brickIndex = 0u;
  debugInfo.dominantProbeIndex = 0u;
  debugInfo.averageValidity = 0.0;
  debugInfo.averageVisibility = 0.0;
  debugInfo.dominantStateHue = 0.0;
}

bool GlobalIlluminationContains(vec3 boundsMin, vec3 boundsMax, vec3 position)
{
  return all(greaterThanEqual(position, boundsMin)) &&
    all(lessThanEqual(position, boundsMax));
}

uint GlobalIlluminationFlattenIndex(uvec3 coordinate, uvec3 counts)
{
  return coordinate.x + counts.x * (coordinate.y + counts.y * coordinate.z);
}

vec2 GlobalIlluminationVisibilityMoments(
  GlobalIlluminationProbe probe,
  uint directionIndex)
{
  if(directionIndex == 0u) return probe.visibility01.xy;
  if(directionIndex == 1u) return probe.visibility01.zw;
  if(directionIndex == 2u) return probe.visibility23.xy;
  if(directionIndex == 3u) return probe.visibility23.zw;
  if(directionIndex == 4u) return probe.visibility45.xy;
  return probe.visibility45.zw;
}

bool GlobalIlluminationDirectionBlocked(
  GlobalIlluminationProbe probe,
  uint directionIndex)
{
  const uint blockedDirections =
    floatBitsToUint(probe.environmentVisibility45.z) &
    GLOBAL_ILLUMINATION_BLOCKED_DIRECTION_MASK;
  return (blockedDirections &
    (1u << (GLOBAL_ILLUMINATION_BLOCKED_DIRECTION_SHIFT +
      directionIndex))) != 0u;
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

float GlobalIlluminationEnvironmentVisibility(
  GlobalIlluminationProbe probe,
  vec3 sourceDirection)
{
  return GlobalIlluminationEnvironmentVisibility(
    vec3(
      probe.environmentVisibility0123.x,
      probe.environmentVisibility0123.z,
      probe.environmentVisibility45.x),
    vec3(
      probe.environmentVisibility0123.y,
      probe.environmentVisibility0123.w,
      probe.environmentVisibility45.y),
    sourceDirection);
}

float GlobalIlluminationVisibilityWeight(
  GlobalIlluminationProbe probe,
  vec3 worldPosition)
{
  const vec3 delta = worldPosition - probe.positionAndValidity.xyz;
  const float distanceToProbe = length(delta);
  if(distanceToProbe <= 0.00001)
  {
    return 1.0;
  }
  const uvec3 directionIndices = uvec3(
    delta.x >= 0.0 ? 0u : 1u,
    delta.y >= 0.0 ? 2u : 3u,
    delta.z >= 0.0 ? 4u : 5u);
  for(uint axis = 0u; axis < 3u; ++axis)
  {
    const uint directionIndex = directionIndices[axis];
    const float clearance = GlobalIlluminationVisibilityMoments(
      probe,
      directionIndex).x;
    if(GlobalIlluminationDirectionBlocked(probe, directionIndex) &&
      abs(delta[axis]) > clearance +
        GLOBAL_ILLUMINATION_BLOCKING_DISTANCE_TOLERANCE)
    {
      return 0.0;
    }
  }
  return 1.0;
}

float GlobalIlluminationSurfaceFacingWeight(
  GlobalIlluminationProbe probe,
  vec3 worldPosition,
  vec3 worldNormal)
{
  const vec3 surfaceToProbe = probe.positionAndValidity.xyz - worldPosition;
  return dot(worldNormal, surfaceToProbe) <
    -GLOBAL_ILLUMINATION_SURFACE_PLANE_TOLERANCE ? 0.0 : 1.0;
}

bool GlobalIlluminationFindBrick(
  vec3 worldPosition,
  out uint selectedBrickIndex,
  out bool traversalComplete)
{
  selectedBrickIndex = 0u;
  traversalComplete = true;
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
  bool found = false;
  uint selectedSubdivision = 0u;
  while(stackSize > 0u && steps < GLOBAL_ILLUMINATION_MAX_TRAVERSAL_STEPS)
  {
    const uint nodeIndex = stack[--stackSize];
    ++steps;
    if(nodeIndex >= globalIlluminationHeader.counts.y ||
      nodeIndex >= uint(globalIlluminationBvh.instance.length()))
    {
      traversalComplete = false;
      continue;
    }
    const GlobalIlluminationBvhNode node =
      globalIlluminationBvh.instance[nodeIndex];
    if(!GlobalIlluminationContains(
      node.minAndLeft.xyz,
      node.maxAndRight.xyz,
      worldPosition))
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
        traversalComplete = false;
        continue;
      }
      const GlobalIlluminationBrick brick =
        globalIlluminationBricks.instance[brickIndex];
      const uint subdivision = floatBitsToUint(brick.minAndSubdivision.w);
      if(GlobalIlluminationContains(
        brick.minAndSubdivision.xyz,
        brick.maxAndFirstProbe.xyz,
        worldPosition) &&
        (!found || subdivision > selectedSubdivision))
      {
        found = true;
        selectedSubdivision = subdivision;
        selectedBrickIndex = brickIndex;
      }
      continue;
    }

    if(stackSize + 2u > GLOBAL_ILLUMINATION_MAX_BVH_STACK)
    {
      traversalComplete = false;
      break;
    }
    stack[stackSize++] = floatBitsToUint(node.maxAndRight.w);
    stack[stackSize++] = leftOrLeaf;
  }
  if(stackSize > 0u)
  {
    traversalComplete = false;
  }
  return found;
}

float GlobalIlluminationCoefficientComponent(
	uint stateIndex,
	uint probeIndex,
	uint coefficientIndex,
	uint colorChannel)
{
	const uint probeCount = globalIlluminationHeader.counts.w;
	const uint packedComponentIndex = coefficientIndex * 3u + colorChannel;
	const uint packedPairIndex = packedComponentIndex / 2u;
	const uint coefficientArrayIndex = stateIndex * probeCount + probeIndex;
	if(coefficientArrayIndex >= uint(globalIlluminationCoefficients.instance.length()))
	{
		return 0.0;
	}
	const uvec4 packedGroup = globalIlluminationCoefficients
		.instance[coefficientArrayIndex].packed[packedPairIndex / 4u];
	const uint packedPair = packedGroup[packedPairIndex % 4u];
	const vec2 values = unpackHalf2x16(packedPair);
	return (packedComponentIndex & 1u) == 0u ? values.x : values.y;
}

vec3 GlobalIlluminationEvaluateProbe(
	uint stateIndex,
	uint probeIndex,
	vec3 sourceNormal)
{
  const vec3 normal = length(sourceNormal) > 0.000001
    ? normalize(sourceNormal)
    : vec3(0.0, 1.0, 0.0);
  const float x = normal.x;
  const float y = normal.y;
  const float z = normal.z;
	const float basis[9] = float[9](
    0.2820947918,
    0.4886025119 * y,
    0.4886025119 * z,
    0.4886025119 * x,
    1.0925484306 * x * y,
    1.0925484306 * y * z,
    0.3153915653 * (3.0 * z * z - 1.0),
    1.0925484306 * x * z,
    0.5462742153 * (x * x - y * y));

  vec3 irradiance = vec3(0.0);
	for(uint coefficientIndex = 0u; coefficientIndex < 9u; ++coefficientIndex)
	{
		const vec3 coefficient = vec3(
			GlobalIlluminationCoefficientComponent(
				stateIndex, probeIndex, coefficientIndex, 0u),
			GlobalIlluminationCoefficientComponent(
				stateIndex, probeIndex, coefficientIndex, 1u),
			GlobalIlluminationCoefficientComponent(
				stateIndex, probeIndex, coefficientIndex, 2u));
		irradiance += coefficient * basis[coefficientIndex];
	}
	return irradiance;
}

vec3 GlobalIlluminationHueToRgb(float hue)
{
  const vec3 shifted = fract(hue + vec3(0.0, 0.6666667, 0.3333333));
  return clamp(abs(shifted * 6.0 - 3.0) - 1.0, 0.0, 1.0);
}

bool SampleGlobalIllumination(
	vec3 worldPosition,
	vec3 worldNormal,
	vec3 surfaceToCamera,
	vec3 environmentDirection,
	out vec3 irradiance,
	out float environmentVisibility,
	out GlobalIlluminationSampleDebug debugInfo)
{
	irradiance = vec3(0.0);
	environmentVisibility = 1.0;
	vec3 positiveDirectionVisibility = vec3(0.0);
	vec3 negativeDirectionVisibility = vec3(0.0);
  InitializeGlobalIlluminationSampleDebug(debugInfo);

  if(globalIlluminationHeader.counts.x == 0u ||
    globalIlluminationHeader.stateAndDebug.x == 0u)
  {
    return false;
  }

  const vec3 normal = length(worldNormal) > 0.000001
    ? normalize(worldNormal)
    : vec3(0.0, 1.0, 0.0);
  const vec3 viewDirection = length(surfaceToCamera) > 0.000001
    ? normalize(surfaceToCamera)
    : vec3(0.0);
  const vec3 samplingPosition = worldPosition +
    normal * globalIlluminationHeader.volumeMin.w +
    viewDirection * globalIlluminationHeader.volumeMax.w;
  uint brickIndex = 0u;
  if(!GlobalIlluminationFindBrick(
    samplingPosition,
    brickIndex,
    debugInfo.traversalComplete))
  {
    return false;
  }
  debugInfo.brickIndex = brickIndex;

  const GlobalIlluminationBrick brick =
    globalIlluminationBricks.instance[brickIndex];
  const vec3 extent = brick.maxAndFirstProbe.xyz -
    brick.minAndSubdivision.xyz;
  const uvec3 probeCounts = brick.probeCountsAndCount.xyz;
  if(any(lessThanEqual(extent, vec3(0.0))) || any(equal(probeCounts, uvec3(0u))))
  {
    return false;
  }

  const vec3 normalizedPosition = clamp(
    (samplingPosition - brick.minAndSubdivision.xyz) / extent,
    vec3(0.0),
    vec3(1.0));
  const vec3 cell = normalizedPosition * vec3(max(probeCounts, uvec3(1u)) - uvec3(1u));
  const uvec3 lower = uvec3(floor(cell));
  const uvec3 upper = min(lower + uvec3(1u), probeCounts - uvec3(1u));
  const vec3 fraction = fract(cell);
  const uint firstProbeIndex = floatBitsToUint(brick.maxAndFirstProbe.w);

  float totalInterpolationWeight = 0.0;
  float totalVisibleWeight = 0.0;
  float dominantProbeWeight = -1.0;
  const uint stateCount = min(
    globalIlluminationHeader.stateAndDebug.x,
    uint(globalIlluminationStates.instance.length()));
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
        const float visibility = GlobalIlluminationVisibilityWeight(
          probe,
          samplingPosition);
        const float interpolationWeight = trilinearWeight * validity;
        const float spatialWeight = interpolationWeight * visibility *
          GlobalIlluminationSurfaceFacingWeight(
            probe,
            worldPosition,
            normal);
        totalInterpolationWeight += interpolationWeight;
        totalVisibleWeight += spatialWeight;
        positiveDirectionVisibility += vec3(
          probe.environmentVisibility0123.x,
          probe.environmentVisibility0123.z,
          probe.environmentVisibility45.x) * spatialWeight;
        negativeDirectionVisibility += vec3(
          probe.environmentVisibility0123.y,
          probe.environmentVisibility0123.w,
          probe.environmentVisibility45.y) * spatialWeight;
        if(spatialWeight > dominantProbeWeight)
        {
          dominantProbeWeight = spatialWeight;
          debugInfo.dominantProbeIndex = probeIndex;
        }
        debugInfo.averageValidity += trilinearWeight * validity;
        debugInfo.averageVisibility += trilinearWeight * visibility;
		for(uint stateIndex = 0u; stateIndex < stateCount; ++stateIndex)
		{
			const float stateWeight =
				globalIlluminationStates.instance[stateIndex].parameters.x;
			irradiance += GlobalIlluminationEvaluateProbe(
				stateIndex,
				probeIndex,
				normal) * spatialWeight * stateWeight;
		}
      }
    }
  }

  if(totalInterpolationWeight <= 0.000001)
  {
    return false;
  }
	if(totalVisibleWeight <= 0.000001)
	{
    // The volume has valid probes, but none has a visible path to this
    // surface. Keep a valid black baked result instead of falling back to the
		// gray environment irradiance used by RealtimeAndBaked mode.
		environmentVisibility = 0.0;
		debugInfo.usedProbeVolume = true;
		return true;
	}
  // Visibility selects probes on the same side of nearby geometry. The SH
  // payload already contains the physical occlusion; multiplying the final
  // energy by the trilinear visibility coverage exposes brick-shaped bands.
	irradiance = max(irradiance / totalVisibleWeight, vec3(0.0));
	positiveDirectionVisibility = clamp(
    positiveDirectionVisibility / totalVisibleWeight,
    vec3(0.0),
    vec3(1.0));
	negativeDirectionVisibility = clamp(
		negativeDirectionVisibility / totalVisibleWeight,
		vec3(0.0),
		vec3(1.0));
	environmentVisibility = GlobalIlluminationEnvironmentVisibility(
		positiveDirectionVisibility,
		negativeDirectionVisibility,
		environmentDirection);
	debugInfo.usedProbeVolume = true;
	return true;
}

vec3 ResolveGlobalIlluminationDiffuseIrradiance(
  vec3 worldPosition,
  vec3 worldNormal,
  vec3 surfaceToCamera,
  vec3 environmentDirection,
  vec3 environmentIrradiance,
  out float environmentVisibility,
  out GlobalIlluminationSampleDebug debugInfo)
{
  environmentVisibility = 1.0;
  InitializeGlobalIlluminationSampleDebug(debugInfo);
  if(globalIlluminationHeader.settings.x == 0u)
  {
    return environmentIrradiance;
  }
  const uint mode = globalIlluminationHeader.settings.y;
  if(mode == GLOBAL_ILLUMINATION_MODE_REALTIME)
  {
    return environmentIrradiance;
  }
  vec3 probeIrradiance = vec3(0.0);
  if(SampleGlobalIllumination(
    worldPosition,
    worldNormal,
    surfaceToCamera,
    environmentDirection,
    probeIrradiance,
    environmentVisibility,
    debugInfo))
  {
    return probeIrradiance;
  }
  // Missing, disabled, non-resident, or out-of-volume GI always preserves the
  // pre-GI SkyComponent cubemap path. A valid black sample above is baked
  // occlusion and is deliberately not treated as fallback.
  environmentVisibility = 1.0;
  return environmentIrradiance;
}

bool GlobalIlluminationDebugSuppressesDirectLighting()
{
  return globalIlluminationHeader.stateAndDebug.z !=
    GLOBAL_ILLUMINATION_DEBUG_LIT;
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

#endif
