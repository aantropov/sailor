const float EVSM_C1 = 40.0f;
const float EVSM_C2 = 40.0f;

layout(std430)
struct LightData
{
  uint type;
  uint shadowType;
  vec3 worldPosition;
  vec3 direction;
  vec3 intensity;
  vec3 attenuation;
  vec2 cutOff;
  vec3 bounds;
};

layout(std430)
struct LightsGrid
{
  uint offset;
  uint num;
}; 

// Importance sample GGX normal distribution function for a fixed roughness value.
// This returns normalized half-vector between Li & Lo.
// For derivation see: http://blog.tobias-franke.eu/2014/03/30/notes_on_importance_sampling.html
vec3 SampleGGX(float u1, float u2, float roughness)
{
  float alpha = roughness * roughness;

  float cosTheta = sqrt((1.0 - u2) / (1.0 + (alpha*alpha - 1.0) * u2));
  float sinTheta = sqrt(1.0 - cosTheta*cosTheta); // Trig. identity
  float phi = TwoPI * u1;

  // Convert to Cartesian upon return.
  return vec3(sinTheta * cos(phi), sinTheta * sin(phi), cosTheta);
}  

// GGX/Towbridge-Reitz normal distribution function.
// Uses Disney's reparametrization of alpha = roughness^2.
float NdfGGX(float cosLh, float roughness)
{
  float alpha   = roughness * roughness;
  float alphaSq = alpha * alpha;

  float denom = (cosLh * cosLh) * (alphaSq - 1.0) + 1.0;
  return alphaSq / (PI * denom * denom);
}

// Single term for separable Schlick-GGX below.
float GeometrySchlickG1(float cosTheta, float k)
{
  return cosTheta / (cosTheta * (1.0 - k) + k);
}

// Schlick-GGX approximation of geometric attenuation function using Smith's method.
float GeometrySchlickGGX(float cosLi, float cosLo, float roughness)
{
  float r = roughness + 1.0;
  float k = (r * r) / 8.0; // Epic suggests using this roughness remapping for analytic lights.
  return GeometrySchlickG1(cosLi, k) * GeometrySchlickG1(cosLo, k);
}

// Schlick-GGX approximation of geometric attenuation function using Smith's method (IBL version).
float GeometrySchlickGGX_IBL(float cosLi, float cosLo, float roughness)
{
  float r = roughness;
  float k = (r * r) / 2.0; // Epic suggests using this roughness remapping for IBL lighting.
  return GeometrySchlickG1(cosLi, k) * GeometrySchlickG1(cosLo, k);
}
  
// Shlick's approximation of the Fresnel factor.
vec3 FresnelSchlick(vec3 F0, float cosTheta)
{
  return F0 + (vec3(1.0) - F0) * pow(1.0 - cosTheta, 5.0);
}

vec3 FresnelSchlickRoughness(float cosTheta, vec3 F0, float roughness)
{
    return F0 + (max(vec3(1.0 - roughness), F0) - F0) * pow(clamp(1.0 - cosTheta, 0.0, 1.0), 5.0);
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

float ManualPCF(sampler2D shadowMap, vec3 projCoords, float currentDepth, float bias)
{
   float shadow = 0.0;
   vec2 texelSize = 1.0 / textureSize(shadowMap, 0);
   for(int x = -2; x <= 2; ++x)
   {
       for(int y = -2; y <= 2; ++y)
       {
           float pcfDepth = texture(shadowMap, projCoords.xy + vec2(x, y) * texelSize).r * 0.5 + 0.5; 
           shadow += currentDepth + bias > pcfDepth ? 1.0 : 0.0;
       }    
   }
   shadow /= 25.0;
   
   return shadow;
}

int SelectCascade(mat4 view, vec3 worldPosition, vec2 cameraZNearZFar)
{
  vec4 fragPosViewSpace = view * vec4(worldPosition, 1.0);
  float depthValue = abs(fragPosViewSpace.z / fragPosViewSpace.w);
  
  int layer = NUM_CSM_CASCADES;
  for (int i = 0; i < NUM_CSM_CASCADES; ++i)
  {
      if (depthValue < cameraZNearZFar.y * ShadowCascadeLevels[i])
      {
          layer = i;
          break;
      }
  }

  return layer;
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

float ShadowCalculation_Pcf(sampler2D shadowMap, vec4 fragPosLightSpace, float bias, int cascadeLayer)
{
  vec3 projCoords = fragPosLightSpace.xyz / fragPosLightSpace.w;
  projCoords = projCoords * 0.5 + 0.5;
  projCoords.y = 1.0f - projCoords.y;
  
  if(projCoords.x > 1.0f || projCoords.y > 1.0f ||
     projCoords.x < 0.0f || projCoords.y < 0.0f ||
     projCoords.z < 0.5f)
  {
    return 1.0f;
  }
  
  const float closestDepth = texture(shadowMap, projCoords.xy).r * 0.5 + 0.5;
  const float currentDepth = projCoords.z;
  
  //float shadow = currentDepth + bias > closestDepth ? 1.0 : 0.0;
  float shadow = ManualPCF(shadowMap, projCoords, currentDepth, bias);
  return shadow;  
}

float ShadowCalculation_Evsm(sampler2D shadowMap, vec4 fragPosLightSpace, float bias, int cascadeLayer)
{
  vec3 projCoords = fragPosLightSpace.xyz / fragPosLightSpace.w;
  projCoords.xy = projCoords.xy * 0.5 + 0.5;
  projCoords.y = 1.0f - projCoords.y;
  
  if(projCoords.x > 1.0f || projCoords.y > 1.0f ||
     projCoords.x < 0.0f || projCoords.y < 0.0f ||
     projCoords.z < 0.0f)
  {
    return 1.0f;
  }
  
  vec4 shadow = texture(shadowMap, projCoords.xy);// > 0.39 ? 1.0f : 0.0f;
  const float currentDepth = exp(EVSM_C1 * (projCoords.z + 0.003 * bias * pow(0.5, cascadeLayer)));
  const float negCurrentDepth = -exp(-EVSM_C2 * (projCoords.z + 0.0001 * bias));
  
  float posValue = Chebyshev(shadow.xy, currentDepth, 0.01, 0);
  float negValue = Chebyshev(shadow.zw, negCurrentDepth, 0, 0) * (cascadeLayer > 2 ? 0 : 1);
  
  return clamp(1 - max(posValue, negValue), 0, 1);
}
