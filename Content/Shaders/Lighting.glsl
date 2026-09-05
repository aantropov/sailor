const float EVSM_C1 = 40.0f;
const float EVSM_C2 = 40.0f;
// Offset the receiver before projecting it into a cascade. Applying bias after
// projection makes the effective surface offset depend on that cascade's depth
// range and precision, which exposes the split as a band.
const float SHADOW_RECEIVER_LIGHT_OFFSET = 0.005f;
const float SHADOW_RECEIVER_NORMAL_OFFSET = 0.02f;
const uint SHADOW_TYPE_NONE = 0u;
const uint SHADOW_TYPE_PCF = 1u;
const uint SHADOW_TYPE_EVSM = 2u;
const uint INVALID_SHADOW_MAP_INDEX = 0xFFFFFFFFu;
const uint SOFT_SHADOW_MAP_BIT = 0x80000000u;
const uint SHADOW_MAP_INDEX_MASK = 0x7FFFFFFFu;

uint GetDirectionalCascadeShadowType(uint lightShadowType, int cascadeLayer)
{
  if (lightShadowType == SHADOW_TYPE_NONE)
  {
    return SHADOW_TYPE_NONE;
  }

  // The renderer stores the near cascade using the light's requested mode,
  // while all wider cascades use depth-only PCF maps. Sampling a PCF map as
  // EVSM moments produces a dark band during the cross-cascade blend.
  return cascadeLayer == 0 ? lightShadowType : SHADOW_TYPE_PCF;
}

layout(std430)
struct LightData
{
  uint type;
  uint shadowType;
  uint activeCascadeCount;
  float shadowBias;
  vec3 worldPosition;
  vec3 direction;
  vec3 intensity;
  vec2 cutOff;
  vec3 bounds;
};

const uint INVALID_LIGHT_TYPE = 0xFFFFFFFFu;
const uint LIGHT_TILE_OVERFLOW_BIT = 0x80000000u;
const uint LIGHT_TILE_COUNT_MASK = 0x7FFFFFFFu;

layout(std430)
struct LightsGrid
{
  uint offset;
  uint num;
}; 

uint GetLightTileIndex(vec2 fragmentPosition, ivec2 viewportSize)
{
  const ivec2 safeViewportSize = max(viewportSize, ivec2(1));
  const ivec2 numTiles =
    (safeViewportSize + ivec2(LIGHTS_CULLING_TILE_SIZE - 1)) /
    LIGHTS_CULLING_TILE_SIZE;
  const ivec2 tileId = clamp(
    ivec2(fragmentPosition) / LIGHTS_CULLING_TILE_SIZE,
    ivec2(0),
    numTiles - ivec2(1));
  return uint(tileId.y * numTiles.x + tileId.x);
}

float CalculateLocalLightRangeAttenuation(LightData light, float distanceToLight)
{
  const float safeRadius = max(light.bounds.x, 0.00001f);
  const float normalizedDistance = clamp(distanceToLight / safeRadius, 0.0f, 1.0f);
  const float minimumDistance = 0.01f;
  const float safeDistance = max(distanceToLight, minimumDistance);
  const float inverseSquareFalloff = 1.0f / (safeDistance * safeDistance);

  // Radius is a culling range in world metres, not a brightness parameter.
  // This is the KHR_lights_punctual range window: inverse-square falloff is
  // preserved near the source and reaches zero with a continuous derivative.
  const float normalizedDistanceSquared =
    normalizedDistance * normalizedDistance;
  const float rangeBase = clamp(
    1.0f - normalizedDistanceSquared * normalizedDistanceSquared,
    0.0f,
    1.0f);
  const float rangeWindow = rangeBase * rangeBase;
  return inverseSquareFalloff * rangeWindow;
}

float CalculateSpotLightAngularAttenuation(
  LightData light,
  vec3 surfaceToLightDirection)
{
  const float coneRange = max(light.cutOff.x - light.cutOff.y, 0.00001f);
  const float actualCosine = dot(
    surfaceToLightDirection,
    normalize(-light.direction));
  const float angularBase = clamp(
    (actualCosine - light.cutOff.y) / coneRange,
    0.0f,
    1.0f);
  return angularBase * angularBase;
}

vec3 CalculateLambertDiffuseBRDF(vec3 diffuseWeight, vec3 albedo)
{
  return diffuseWeight * albedo / PI;
}

vec3 NormalizeOrFallback(vec3 value, vec3 fallback)
{
  const float lengthSquared = dot(value, value);
  return lengthSquared > 1e-12 ?
    value * inversesqrt(lengthSquared) :
    fallback;
}

#ifdef FRAGMENT
void ResolveFragmentSurfaceGeometry(
  vec3 worldPosition,
  vec3 surfaceToCamera,
  vec3 interpolatedNormal,
  mat3 interpolatedTangentBasis,
  out vec3 geometricNormal,
  out vec3 vertexNormal,
  out mat3 tangentBasis)
{
  const vec3 derivativeNormal = cross(
    dFdx(worldPosition),
    dFdy(worldPosition));
  const float derivativeLengthSquared = dot(
    derivativeNormal,
    derivativeNormal);
  const vec3 interpolatedNormalFallback = NormalizeOrFallback(
    interpolatedNormal,
    vec3(0.0, 1.0, 0.0));
  geometricNormal = derivativeLengthSquared > 1e-12 ?
    derivativeNormal * inversesqrt(derivativeLengthSquared) :
    interpolatedNormalFallback;
  if(dot(geometricNormal, surfaceToCamera) < 0.0)
  {
    geometricNormal *= -1.0;
  }
  vertexNormal = NormalizeOrFallback(
    interpolatedNormal,
    geometricNormal);
  tangentBasis = interpolatedTangentBasis;
  if(dot(vertexNormal, geometricNormal) < 0.0)
  {
    vertexNormal *= -1.0;
    tangentBasis[1] *= -1.0;
  }

  vec3 tangent = tangentBasis[0] -
    vertexNormal * dot(vertexNormal, tangentBasis[0]);
  const float tangentLengthSquared = dot(tangent, tangent);
  if(tangentLengthSquared <= 1e-12)
  {
    const vec3 fallbackAxis = abs(vertexNormal.y) < 0.999
      ? vec3(0.0, 1.0, 0.0)
      : vec3(1.0, 0.0, 0.0);
    tangent = cross(fallbackAxis, vertexNormal);
  }
  tangent = normalize(tangent);
  const vec3 canonicalBitangent = normalize(cross(vertexNormal, tangent));
  const float handedness = dot(
    canonicalBitangent,
    tangentBasis[1]) < 0.0 ? -1.0 : 1.0;
  tangentBasis = mat3(
    tangent,
    canonicalBitangent * handedness,
    vertexNormal);
}

vec3 KeepShadingNormalOnGeometricSurface(
  vec3 shadingNormal,
  vec3 geometricNormal)
{
  const float geometricCosine = dot(shadingNormal, geometricNormal);
  return normalize(shadingNormal -
    2.0 * min(geometricCosine, 0.0) * geometricNormal);
}
#endif

// Importance sample GGX normal distribution function for a fixed roughness value.
// This returns normalized half-vector between Li & Lo.
// For derivation see: http://blog.tobias-franke.eu/2014/03/30/notes_on_importance_sampling.html
vec3 SampleGGX(float u1, float u2, float roughness)
{
  float alpha = max(roughness * roughness, 0.001);

  float cosTheta = sqrt((1.0 - u2) / (1.0 + (alpha*alpha - 1.0) * u2));
  float sinTheta = sqrt(max(1.0 - cosTheta*cosTheta, 0.0)); // Trig. identity
  float phi = TwoPI * u1;

  // Convert to Cartesian upon return.
  return vec3(sinTheta * cos(phi), sinTheta * sin(phi), cosTheta);
}

// Importance sample the Charlie distribution used by KHR_materials_sheen.
vec3 SampleCharlie(float u1, float u2, float roughness)
{
  const float alpha = max(roughness * roughness, 0.000001);
  const float sinTheta = pow(
    clamp(u2, 0.000001, 0.999999),
    alpha / (2.0 * alpha + 1.0));
  const float cosTheta = sqrt(max(1.0 - sinTheta * sinTheta, 0.0));
  const float phi = TwoPI * u1;
  return vec3(
    sinTheta * cos(phi),
    sinTheta * sin(phi),
    cosTheta);
}

// GGX/Towbridge-Reitz normal distribution function.
// Uses Disney's reparametrization of alpha = roughness^2.
float NdfGGX(float cosLh, float roughness)
{
  float alpha   = max(roughness * roughness, 0.001);
  float alphaSq = alpha * alpha;

  float denom = (cosLh * cosLh) * (alphaSq - 1.0) + 1.0;
  return alphaSq / max(PI * denom * denom, 1e-7);
}

// Height-correlated Smith masking-shadowing for isotropic GGX.
float GeometrySmithGGXCorrelated(float cosLi, float cosLo, float roughness)
{
  const float nDotL = clamp(cosLi, 0.0, 1.0);
  const float nDotV = clamp(cosLo, 0.0, 1.0);
  if(nDotL <= 0.0 || nDotV <= 0.0)
  {
    return 0.0;
  }

  const float alpha = max(roughness * roughness, 0.001);
  const float alphaSq = alpha * alpha;
  const float lambdaV = nDotL * sqrt(
    nDotV * nDotV * (1.0 - alphaSq) + alphaSq);
  const float lambdaL = nDotV * sqrt(
    nDotL * nDotL * (1.0 - alphaSq) + alphaSq);
  return 2.0 * nDotL * nDotV /
    max(lambdaV + lambdaL, 1e-7);
}

// Charlie microfacet normal distribution function used for cloth-like sheen.
float NdfCharlie(float cosLh, float roughness)
{
  const float alpha = max(roughness * roughness, 0.000001);
  float invAlpha = 1.0 / alpha;
  float cos2h = cosLh * cosLh;
  float sin2h = max(1.0 - cos2h, 0.0);
  return (2.0 + invAlpha) * pow(sin2h, 0.5 * invAlpha) / (2.0 * PI);
}

float LambdaSheenNumericHelper(float cosine, float alpha)
{
  const float oneMinusAlphaSquared =
    (1.0 - alpha) * (1.0 - alpha);
  const float a = mix(21.5473, 25.3245, oneMinusAlphaSquared);
  const float b = mix(3.82987, 3.32435, oneMinusAlphaSquared);
  const float c = mix(0.19823, 0.16801, oneMinusAlphaSquared);
  const float d = mix(-1.97760, -1.27393, oneMinusAlphaSquared);
  const float e = mix(-4.32054, -4.85967, oneMinusAlphaSquared);
  const float x = clamp(cosine, 0.0, 1.0);
  return a / (1.0 + b * pow(x, c)) + d * x + e;
}

float LambdaSheen(float cosine, float alpha)
{
  const float x = clamp(abs(cosine), 0.0, 1.0);
  return x < 0.5 ?
    exp(LambdaSheenNumericHelper(x, alpha)) :
    exp(
      2.0 * LambdaSheenNumericHelper(0.5, alpha) -
      LambdaSheenNumericHelper(1.0 - x, alpha));
}

// Charlie masking-shadowing visibility fitted to the production sheen model.
float VisibilitySheen(float cosLi, float cosLo, float roughness)
{
  const float clampedCosLi = clamp(cosLi, 0.0, 1.0);
  const float clampedCosLo = clamp(cosLo, 0.0, 1.0);
  if(clampedCosLi <= 0.0 || clampedCosLo <= 0.0)
  {
    return 0.0;
  }

  const float alpha = max(roughness * roughness, 0.000001);
  const float denominator =
    (1.0 +
      LambdaSheen(clampedCosLo, alpha) +
      LambdaSheen(clampedCosLi, alpha)) *
    4.0 * clampedCosLo * clampedCosLi;
  return 1.0 / max(denominator, 1e-7);
}

// Schlick's approximation of the Fresnel factor.
vec3 FresnelSchlick(vec3 F0, float cosTheta)
{
  const vec3 clampedF0 = clamp(F0, vec3(0.0), vec3(1.0));
  const float clampedCosine = clamp(cosTheta, 0.0, 1.0);
  return clampedF0 + (vec3(1.0) - clampedF0) *
    pow(1.0 - clampedCosine, 5.0);
}

float FresnelDielectric(float cosTheta, float fromIor, float toIor)
{
  const float etaI = max(fromIor, 0.0001);
  const float etaT = max(toIor, 0.0001);
  if(abs(etaI - etaT) <= 0.0001)
  {
    return 0.0;
  }
  // KHR_materials_ior uses zero as the specular-glossiness compatibility
  // value. The importer represents its effective infinite IOR with 1e6.
  if(etaI >= 100000.0 || etaT >= 100000.0)
  {
    return 1.0;
  }

  const float cosThetaI = clamp(abs(cosTheta), 0.0, 1.0);
  const float sinThetaISquared = max(
    0.0,
    1.0 - cosThetaI * cosThetaI);
  const float eta = etaI / etaT;
  const float sinThetaTSquared =
    eta * eta * sinThetaISquared;
  if(sinThetaTSquared >= 1.0)
  {
    return 1.0;
  }

  const float cosThetaT = sqrt(max(
    0.0,
    1.0 - sinThetaTSquared));
  const float parallelNumerator =
    etaT * cosThetaI - etaI * cosThetaT;
  const float parallelDenominator =
    etaT * cosThetaI + etaI * cosThetaT;
  const float perpendicularNumerator =
    etaI * cosThetaI - etaT * cosThetaT;
  const float perpendicularDenominator =
    etaI * cosThetaI + etaT * cosThetaT;
  const float parallel = abs(parallelDenominator) > 0.0001 ?
    parallelNumerator / parallelDenominator : 1.0;
  const float perpendicular = abs(perpendicularDenominator) > 0.0001 ?
    perpendicularNumerator / perpendicularDenominator : 1.0;
  return clamp(
    0.5 * (parallel * parallel + perpendicular * perpendicular),
    0.0,
    1.0);
}

vec3 FresnelSchlickRoughness(float cosTheta, vec3 F0, float roughness)
{
    const vec3 clampedF0 = clamp(F0, vec3(0.0), vec3(1.0));
    const float clampedRoughness = clamp(roughness, 0.0, 1.0);
    return clampedF0 +
      (max(vec3(1.0 - clampedRoughness), clampedF0) - clampedF0) *
      pow(clamp(1.0 - cosTheta, 0.0, 1.0), 5.0);
} 

// Screen-space ambient occlusion describes the visibility of indirect light.
// Diffuse IBL can use the visibility directly, while glossy reflections need
// a view- and roughness-dependent approximation to avoid over-darkening.
float CalculateSpecularOcclusion(float ambientOcclusion, float cosLo, float roughness)
{
  const float ao = clamp(ambientOcclusion, 0.0, 1.0);
  const float nDotV = clamp(cosLo, 0.0, 1.0);
  const float clampedRoughness = clamp(roughness, 0.0, 1.0);
  const float exponent = exp2(-16.0 * clampedRoughness - 1.0);
  return clamp(pow(nDotV + ao, exponent) - 1.0 + ao, 0.0, 1.0);
}

float CalculateSpecularEnvironmentLod(int mipLevelCount, float roughness)
{
  const int maximumMipLevel = max(mipLevelCount - 1, 0);
  return clamp(roughness, 0.0, 1.0) * float(maximumMipLevel);
}

vec4 GaussianBlur_Evsm(sampler2D textureSampler, vec2 uv, vec2 texelSize, ivec2 radius)
{
  const int stepCount = 12;
  
  const float weights[stepCount][stepCount] = {
    { 0.5, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0},
    { 0.281088, 0.218912, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0},
    { 0.197159, 0.176426, 0.126415, 0, 0, 0, 0, 0, 0, 0, 0, 0},
    { 0.152068, 0.142855, 0.118431, 0.0866459, 0, 0, 0, 0, 0, 0, 0, 0},
    { 0.123827, 0.118971, 0.105518, 0.0863909, 0.0652929, 0, 0, 0, 0, 0, 0, 0},
    { 0.104454, 0.101593, 0.0934699, 0.0813492, 0.0669741, 0.0521595, 0, 0, 0, 0, 0, 0},
    { 0.0903332, 0.0885083, 0.083252, 0.0751759, 0.0651684, 0.0542336, 0.0433285, 0, 0, 0, 0, 0},
    { 0.07958, 0.0783462, 0.0747585, 0.0691403, 0.061977, 0.0538465, 0.0453433, 0.0370081, 0, 0, 0, 0},
    { 0.0711171, 0.0702445, 0.0676904, 0.0636383, 0.0583697, 0.0522315, 0.0455989, 0.0388376, 0.0322721, 0, 0, 0},
    { 0.0642825, 0.0636429, 0.0617619, 0.0587498, 0.0547779, 0.0500633, 0.0448484, 0.0393811, 0.0338957, 0.0285966, 0, 0},
    { 0.0586472, 0.0581645, 0.0567402, 0.0544433, 0.0513831, 0.0476999, 0.0435548, 0.039118, 0.0345572, 0.0300277, 0.0256641, 0},
    { 0.0539209, 0.0535478, 0.0524437, 0.050654, 0.0482506, 0.0453272, 0.0419936, 0.0383686, 0.034573, 0.0307232, 0.0269255, 0.0232718}};

    const uint blurRadius = min(max(radius.x, radius.y), stepCount);
    const uint blurRadius1 = min(radius.x, stepCount);
    const uint blurRadius2 = min(radius.y, stepCount);

    vec4 pixelSum = vec4(0.0f);
  
    for(int i = 0; i < blurRadius; i++)
    {  
        vec2 texCoordOffset = i * texelSize;
        vec4 umbra = vec4(0.0f);
        vec4 penumbra = vec4(0.0f);
        
        if(i < radius.x)
        {
            umbra.zw = texture(textureSampler, uv + texCoordOffset).zw + texture(textureSampler, uv - texCoordOffset).zw;
            pixelSum += umbra * weights[blurRadius1-1][i];
        }
        
        if(i < radius.y)
        {
            penumbra.xy = texture(textureSampler, uv + texCoordOffset).xy + texture(textureSampler, uv - texCoordOffset).xy;
            pixelSum += penumbra * weights[blurRadius2-1][i];
        }
    }

    return pixelSum;
}

vec4 GaussianBlur(sampler2D textureSampler, vec2 uv, vec2 texelSize, uint radius)
{
  const int stepCount = 12;
  
  const float weights[stepCount][stepCount] = {
    { 0.5, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0},
    { 0.281088, 0.218912, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0},
    { 0.197159, 0.176426, 0.126415, 0, 0, 0, 0, 0, 0, 0, 0, 0},
    { 0.152068, 0.142855, 0.118431, 0.0866459, 0, 0, 0, 0, 0, 0, 0, 0},
    { 0.123827, 0.118971, 0.105518, 0.0863909, 0.0652929, 0, 0, 0, 0, 0, 0, 0},
    { 0.104454, 0.101593, 0.0934699, 0.0813492, 0.0669741, 0.0521595, 0, 0, 0, 0, 0, 0},
    { 0.0903332, 0.0885083, 0.083252, 0.0751759, 0.0651684, 0.0542336, 0.0433285, 0, 0, 0, 0, 0},
    { 0.07958, 0.0783462, 0.0747585, 0.0691403, 0.061977, 0.0538465, 0.0453433, 0.0370081, 0, 0, 0, 0},
    { 0.0711171, 0.0702445, 0.0676904, 0.0636383, 0.0583697, 0.0522315, 0.0455989, 0.0388376, 0.0322721, 0, 0, 0},
    { 0.0642825, 0.0636429, 0.0617619, 0.0587498, 0.0547779, 0.0500633, 0.0448484, 0.0393811, 0.0338957, 0.0285966, 0, 0},
    { 0.0586472, 0.0581645, 0.0567402, 0.0544433, 0.0513831, 0.0476999, 0.0435548, 0.039118, 0.0345572, 0.0300277, 0.0256641, 0},
    { 0.0539209, 0.0535478, 0.0524437, 0.050654, 0.0482506, 0.0453272, 0.0419936, 0.0383686, 0.034573, 0.0307232, 0.0269255, 0.0232718}};

    const uint blurRadius = min(radius, stepCount);

    vec4 pixelSum = vec4(0.0f);
  
    for(int i = 0; i < blurRadius; i++)
    {  
        vec2 texCoordOffset = i * texelSize;
        vec4 color = texture(textureSampler, uv + texCoordOffset) + texture(textureSampler, uv - texCoordOffset);
        pixelSum += color * weights[blurRadius-1][i];
    }

    return pixelSum;
}

float LuminanceCzm(vec3 rgb)
{
    // Algorithm from Chapter 10 of Graphics Shaders.
    const vec3 w = vec3(0.2125, 0.7154, 0.0721);
    return dot(rgb, w);
}

float ManualPCF(sampler2D shadowMap, vec3 projCoords, float currentDepth)
{
   float shadow = 0.0;
   vec2 texelSize = 1.0 / textureSize(shadowMap, 0);
   int samples = 16; // Number of samples
   float radius = 2.0; // Radius of the kernel

   // Poisson disk sampling pattern
   vec2 poissonDisk[16] = vec2[](
       vec2(-0.94201624, -0.39906216), vec2(0.94558609, -0.76890725), 
       vec2(-0.094184101, -0.92938870), vec2(0.34495938, 0.29387760), 
       vec2(-0.91588581, 0.45771432), vec2(-0.81544232, -0.87912464), 
       vec2(-0.38277543, 0.27676845), vec2(0.97484398, 0.75648379), 
       vec2(0.44323325, -0.97511554), vec2(0.53742981, -0.47373420), 
       vec2(-0.26496911, -0.41893023), vec2(0.79197514, 0.19090188), 
       vec2(-0.24188840, 0.99706507), vec2(-0.81409955, 0.91437590), 
       vec2(0.19984126, 0.78641367), vec2(0.14383161, -0.14100790)
   );

   for(int i = 0; i < samples; ++i)
   {
       const vec2 offset = poissonDisk[i] * radius * texelSize;
       const vec2 sampleUv = projCoords.xy + offset;
       if(any(lessThan(sampleUv, vec2(0.0f))) ||
          any(greaterThan(sampleUv, vec2(1.0f))))
       {
           // A clamped depth lookup repeats the outer texel and creates a dark
           // PCF rim at the cascade boundary. Outside the current projection
           // there is no occluder represented by this map; the overlapping
           // cascade supplies the valid shadow sample for this region.
           shadow += 1.0f;
           continue;
       }

       const float pcfDepth = texture(shadowMap, sampleUv).r;
       shadow += currentDepth > pcfDepth ? 1.0 : 0.0;
   }

   shadow /= float(samples);
   
   return shadow;
}

uint SelectPointShadowFace(vec3 lightToReceiver)
{
  const vec3 axis = abs(lightToReceiver);
  if(axis.x >= axis.y && axis.x >= axis.z)
  {
    return lightToReceiver.x >= 0.0f ? 0u : 1u;
  }
  if(axis.y >= axis.z)
  {
    return lightToReceiver.y >= 0.0f ? 2u : 3u;
  }
  return lightToReceiver.z >= 0.0f ? 4u : 5u;
}

vec4 DecodeShadowAtlasRect(uint packedTile)
{
  const uint tileX = packedTile & 63u;
  const uint tileY = (packedTile >> 6u) & 63u;
  const uint tileCells = 1u << ((packedTile >> 12u) & 7u);
  return vec4(tileX, tileY, tileCells, tileCells) / 64.0f;
}

uint DecodeShadowAtlasIndex(uint packedTile)
{
  return packedTile >> 15u;
}

vec2 CalculateLocalShadowReceiverDepthGradient(
  mat4 lightMatrix,
  vec3 projectedPosition,
  vec3 surfaceNormal)
{
  const vec3 lightW = vec3(lightMatrix[0][3], lightMatrix[1][3], lightMatrix[2][3]);
  // Jacobian of perspective division. Its common 1/w scale cancels in the
  // inverse-transpose normal ratios, so this also handles translated lights.
  const vec3 lightX = vec3(lightMatrix[0][0], lightMatrix[1][0], lightMatrix[2][0]) -
    projectedPosition.x * lightW;
  const vec3 lightY = vec3(lightMatrix[0][1], lightMatrix[1][1], lightMatrix[2][1]) -
    projectedPosition.y * lightW;
  const vec3 lightZ = vec3(lightMatrix[0][2], lightMatrix[1][2], lightMatrix[2][2]) -
    projectedPosition.z * lightW;
  const vec3 normal = NormalizeOrFallback(surfaceNormal, vec3(0.0f, 1.0f, 0.0f));
  const vec3 depthAxis = cross(lightX, lightY);
  const float depthAxisLength = length(depthAxis);
  const float planeDepth = dot(depthAxis, normal);
  if(depthAxisLength <= 1e-12f || abs(planeDepth) < 0.001f * depthAxisLength)
  {
    return vec2(0.0f);
  }

  const vec2 planeXY = vec2(
    dot(cross(lightY, lightZ), normal),
    dot(cross(lightZ, lightX), normal));
  return vec2(-2.0f, 2.0f) * planeXY / planeDepth;
}

float SampleLocalShadowReceiverPlane(
  sampler2D shadowMap,
  vec2 sampleUv,
  vec2 receiverUv,
  float receiverDepth,
  vec2 receiverDepthGradient)
{
  const ivec2 mapSize = textureSize(shadowMap, 0);
  const ivec2 texel = clamp(ivec2(floor(sampleUv * vec2(mapSize))),
    ivec2(0), mapSize - ivec2(1));
  const vec2 texelCenterUv = (vec2(texel) + 0.5f) / vec2(mapSize);
  const float depthAtTexel = receiverDepth +
    dot(receiverDepthGradient, texelCenterUv - receiverUv);
  const float shadowDepth = texelFetch(shadowMap, texel, 0).r;
  return shadowDepth == 0.0f || depthAtTexel > shadowDepth ? 1.0f : 0.0f;
}

float CalculateLocalPcfShadow(
  sampler2D shadowMap,
  mat4 lightMatrix,
  vec4 atlasRect,
  vec3 worldPosition,
  vec3 surfaceNormal,
  bool softShadow,
  float receiverBiasScale)
{
  const vec4 fragPosLightSpace = lightMatrix * vec4(worldPosition, 1.0f);
  if(fragPosLightSpace.w <= 0.0f)
  {
    return 1.0f;
  }
  vec3 projCoords = fragPosLightSpace.xyz / fragPosLightSpace.w;
  const vec2 tileDepthGradient = CalculateLocalShadowReceiverDepthGradient(
    lightMatrix, projCoords, surfaceNormal);
  projCoords.xy = projCoords.xy * 0.5f + 0.5f;
  projCoords.y = 1.0f - projCoords.y;
  if(any(lessThan(projCoords, vec3(0.0f))) ||
     any(greaterThan(projCoords, vec3(1.0f))))
  {
    return 1.0f;
  }

  const vec2 atlasTexelSize = 1.0f / textureSize(shadowMap, 0);
  const vec2 tileMin = atlasRect.xy + atlasTexelSize * 0.5f;
  const vec2 tileMax = atlasRect.xy + atlasRect.zw - atlasTexelSize * 0.5f;
  const vec2 atlasUv = clamp(
    atlasRect.xy + projCoords.xy * atlasRect.zw,
    tileMin,
    tileMax);
  const vec2 receiverUv = atlasRect.xy + projCoords.xy * atlasRect.zw;
  const vec2 receiverDepthGradient = tileDepthGradient / max(atlasRect.zw, atlasTexelSize);
  // The atlas stores R16_UNORM color depth. World-space offsets can disappear
  // after that quantization, and they displace shadows differently per triangle.
  const float depthQuantization = 1.0f / 65535.0f;
  const float receiverDepth = projCoords.z + receiverBiasScale * depthQuantization;
  if(softShadow)
  {
    const vec2 poissonDisk[16] = vec2[](
      vec2(-0.94201624, -0.39906216), vec2(0.94558609, -0.76890725),
      vec2(-0.094184101, -0.92938870), vec2(0.34495938, 0.29387760),
      vec2(-0.91588581, 0.45771432), vec2(-0.81544232, -0.87912464),
      vec2(-0.38277543, 0.27676845), vec2(0.97484398, 0.75648379),
      vec2(0.44323325, -0.97511554), vec2(0.53742981, -0.47373420),
      vec2(-0.26496911, -0.41893023), vec2(0.79197514, 0.19090188),
      vec2(-0.24188840, 0.99706507), vec2(-0.81409955, 0.91437590),
      vec2(0.19984126, 0.78641367), vec2(0.14383161, -0.14100790));
    float lit = 0.0f;
    for(int sampleIndex = 0; sampleIndex < 16; ++sampleIndex)
    {
      const vec2 sampleUv = clamp(
        atlasUv + poissonDisk[sampleIndex] * 2.0f * atlasTexelSize,
        tileMin,
        tileMax);
      lit += SampleLocalShadowReceiverPlane(shadowMap, sampleUv, receiverUv,
        receiverDepth, receiverDepthGradient);
    }
    return lit / 16.0f;
  }

  return SampleLocalShadowReceiverPlane(shadowMap, atlasUv, receiverUv,
    receiverDepth, receiverDepthGradient);
}


float GetActiveShadowCascadeLevel(int cascadeLayer, uint activeCascadeCount)
{
  const uint safeCascadeCount = clamp(activeCascadeCount, 1u, uint(NUM_CSM_CASCADES));
  const int levelIndex = NUM_CSM_CASCADES - int(safeCascadeCount) + cascadeLayer;
  return ShadowCascadeLevels[clamp(levelIndex, 0, NUM_CSM_CASCADES - 1)];
}

int SelectCascade(
  mat4 view,
  vec3 worldPosition,
  vec2 cameraZNearZFar,
  uint activeCascadeCount)
{
  vec4 fragPosViewSpace = view * vec4(worldPosition, 1.0);
  float depthValue = abs(fragPosViewSpace.z / fragPosViewSpace.w);
  float shadowFarPlane = min(cameraZNearZFar.y, ShadowMaxDistance);
  const uint safeCascadeCount = clamp(activeCascadeCount, 1u, uint(NUM_CSM_CASCADES));
  
  int layer = int(safeCascadeCount);
  for (int i = 0; i < int(safeCascadeCount); ++i)
  {
      if (depthValue < shadowFarPlane *
        GetActiveShadowCascadeLevel(i, safeCascadeCount))
      {
          layer = i;
          break;
      }
  }

  return layer;
}

float CalculateCascadeBlend(
  mat4 view,
  vec3 worldPosition,
  vec2 cameraZNearZFar,
  int cascadeLayer,
  uint activeCascadeCount)
{
  const uint safeCascadeCount = clamp(activeCascadeCount, 1u, uint(NUM_CSM_CASCADES));
  if(cascadeLayer < 0 || cascadeLayer >= int(safeCascadeCount) - 1)
  {
    return 0.0f;
  }

  vec4 fragPosViewSpace = view * vec4(worldPosition, 1.0f);
  float depthValue = abs(fragPosViewSpace.z / fragPosViewSpace.w);
  float shadowFarPlane = min(cameraZNearZFar.y, ShadowMaxDistance);
  float cascadeNear = cascadeLayer == 0 ?
    cameraZNearZFar.x :
    shadowFarPlane * GetActiveShadowCascadeLevel(
      cascadeLayer - 1, safeCascadeCount);
  float cascadeFar = shadowFarPlane * GetActiveShadowCascadeLevel(
    cascadeLayer, safeCascadeCount);
  float blendStart = cascadeFar -
    (cascadeFar - cascadeNear) * ShadowCascadeBlendFraction;
  return smoothstep(blendStart, cascadeFar, depthValue);
}

float CalculateShadowDistanceFade(
  mat4 view,
  vec3 worldPosition,
  vec2 cameraZNearZFar,
  int cascadeLayer,
  uint activeCascadeCount)
{
  const uint safeCascadeCount = clamp(activeCascadeCount, 1u, uint(NUM_CSM_CASCADES));
  if(cascadeLayer != int(safeCascadeCount) - 1)
  {
    return 0.0f;
  }

  const vec4 fragPosViewSpace = view * vec4(worldPosition, 1.0f);
  const float depthValue = abs(fragPosViewSpace.z / fragPosViewSpace.w);
  const float shadowFarPlane = min(cameraZNearZFar.y, ShadowMaxDistance);
  const float cascadeNear = safeCascadeCount > 1u ?
    shadowFarPlane * GetActiveShadowCascadeLevel(
      int(safeCascadeCount) - 2, safeCascadeCount) :
    cameraZNearZFar.x;
  const float fadeStart = shadowFarPlane -
    (shadowFarPlane - cascadeNear) * ShadowCascadeBlendFraction;
  return smoothstep(fadeStart, shadowFarPlane, depthValue);
}
  
float Linstep(float minVal, float maxVal, float val) 
{
	return clamp((val - minVal) / (maxVal - minVal), 0.0, 1.0);
}

float ReduceLightBleed(float p_max, float amount) 
{
	return Linstep(amount, 1.0, p_max);
}

float Chebyshev(vec2 moments, float currentDepth, float minVariance, float linstep)
{
    float d = currentDepth - moments.x;
    
    if(d < 0)
    {
        return 1.0;
    }
    
    float variance = max(minVariance, moments.y - moments.x * moments.x);
    
    return ReduceLightBleed(variance / (variance + d * d), linstep);
}

float ShadowCalculation_Pcf(sampler2D shadowMap, vec4 fragPosLightSpace, int cascadeLayer)
{
  vec3 projCoords = fragPosLightSpace.xyz / fragPosLightSpace.w;
  projCoords.xy = projCoords.xy * 0.5 + 0.5;
  projCoords.y = 1.0f - projCoords.y;
  
  if(projCoords.x > 1.0f || projCoords.y > 1.0f ||
     projCoords.x < 0.0f || projCoords.y < 0.0f ||
     projCoords.z < 0.0f || projCoords.z > 1.0f)
  {
    return 1.0f;
  }
  
  const float currentDepth = projCoords.z;
  
  float shadow = ManualPCF(shadowMap, projCoords, currentDepth);
  return shadow;  
}

float ShadowCalculation_Evsm(sampler2D shadowMap, vec4 fragPosLightSpace, int cascadeLayer)
{
  vec3 projCoords = fragPosLightSpace.xyz / fragPosLightSpace.w;
  projCoords.xy = projCoords.xy * 0.5 + 0.5;
  projCoords.y = 1.0f - projCoords.y;
  
  if(projCoords.x > 1.0f || projCoords.y > 1.0f ||
     projCoords.x < 0.0f || projCoords.y < 0.0f ||
     projCoords.z < 0.0f || projCoords.z > 1.0f)
  {
    return 1.0f;
  }
  
  vec4 shadow = texture(shadowMap, projCoords.xy);// > 0.39 ? 1.0f : 0.0f;
  const float currentDepth = exp(EVSM_C1 * projCoords.z);
  const float negCurrentDepth = -exp(-EVSM_C2 * projCoords.z);
  
  float posValue = Chebyshev(shadow.xy, currentDepth, 0.01, 0);
  float negValue = Chebyshev(shadow.zw, negCurrentDepth, 0, 0) * (cascadeLayer > 2 ? 0 : 1);
  
  return clamp(1 - max(posValue, negValue), 0, 1);
}

float CalculateDirectionalShadow(
  uint shadowType,
  sampler2D shadowMap,
  vec4 fragPosLightSpace,
  int cascadeLayer)
{
  if (shadowType == SHADOW_TYPE_NONE)
  {
    return 1.0f;
  }

  if (shadowType == SHADOW_TYPE_EVSM && cascadeLayer == 0)
  {
    return ShadowCalculation_Evsm(
      shadowMap,
      fragPosLightSpace,
      cascadeLayer);
  }

  return ShadowCalculation_Pcf(shadowMap, fragPosLightSpace, cascadeLayer);
}

vec3 OffsetDirectionalShadowReceiver(
  vec3 worldPosition,
  vec3 surfaceNormal,
  vec3 surfaceToLightDirection)
{
  const vec3 normal = normalize(surfaceNormal);
  const vec3 toLight = normalize(surfaceToLightDirection);
  const float cosTheta = clamp(dot(normal, toLight), 0.0f, 1.0f);
  const float sinTheta = sqrt(max(1.0f - cosTheta * cosTheta, 0.0f));
  return worldPosition +
    toLight * SHADOW_RECEIVER_LIGHT_OFFSET +
    normal * (SHADOW_RECEIVER_NORMAL_OFFSET * sinTheta);
}
