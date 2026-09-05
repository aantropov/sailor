--- 
includes:
- Shaders/Constants.glsl
- Shaders/Math.glsl
- Shaders/Noise.glsl

defines:
- FILL
- SUN
- COMPOSE
- CLOUDS
- DITHER
- DISCARD_BY_DEPTH

depthAttachment :
- UNDEFINED

glslCommon: |
  #version 450

glslVertex: |
  layout(location=DefaultPositionBinding) in vec3 inPosition;
  layout(location=DefaultTexcoordBinding) in vec2 inTexcoord;
  
  layout(location=0) out vec2 fragTexcoord;

  #if defined(CLOUDS)
  layout(push_constant) uniform Constants
  {
    uint ditherPattern;
  } PushConstants;
  #endif
  
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
  
  layout(set=1, binding=0) uniform PostProcessDataUBO
  {
      vec4 lightDirection;
      vec4 sunIlluminance;
      vec4 groundRadiance;
      float cloudsAttenuation1;
    float cloudsAttenuation2;
    float cloudsDensity;
    float cloudsCoverage;
    float phaseInfluence1;
    float phaseInfluence2;
    float eccentrisy1;
    float eccentrisy2;
    float fog;
    float cloudScatteringScale;
    float ambient;
    int   scatteringSteps;
    float scatteringDensity;
    float scatteringIntensity;
    float scatteringPhase;
    float sunShaftsIntensity;
    int   sunShaftsDistance;
  } data;

  #if defined(COMPOSE)
    layout(set=1, binding=1) uniform sampler2D skySampler;
    layout(set=1, binding=2) uniform sampler2D sunSampler;
  #elif defined(SUN)
    layout(set=1, binding=6) uniform sampler2D cloudsSampler;
  #elif defined(CLOUDS)
    layout(set=1, binding=1) uniform sampler2D skySampler;
    layout(set=1, binding=3) uniform sampler2D cloudsMapSampler;
    layout(set=1, binding=4) uniform sampler3D cloudsNoiseLowSampler;
    layout(set=1, binding=5) uniform sampler3D cloudsNoiseHighSampler;
    layout(set=1, binding=7) uniform sampler2D g_ditherPatternSampler;
    layout(set=1, binding=8) uniform sampler2D g_noiseSampler;
    layout(set=1, binding=9) uniform sampler2D linearDepth;
  #else
    layout(set=1, binding=9) uniform sampler2D linearDepth;
  #endif
  
  void main() 
  {
      gl_Position = vec4(inPosition, 1);
      fragTexcoord = inTexcoord;

      #if !defined(COMPOSE)
        fragTexcoord.y = 1.0f - fragTexcoord.y;
      #endif
  }
  
glslFragment: |

  #if defined(CLOUDS)
  layout(push_constant) uniform Constants
  {
    uint ditherPattern;
  } PushConstants;
  #endif
  
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
  
  layout(set=1, binding=0) uniform PostProcessDataUBO
  {
    vec4 lightDirection;
    vec4 sunIlluminance;
    vec4 groundRadiance;
    float cloudsAttenuation1;
    float cloudsAttenuation2;
    float cloudsDensity;
    float cloudsCoverage;
    float phaseInfluence1;
    float phaseInfluence2;
    float eccentrisy1;
    float eccentrisy2;
    float fog;
    float cloudScatteringScale;
    float ambient;
    int   scatteringSteps;
    float scatteringDensity;
    float scatteringIntensity;
    float scatteringPhase;
    float sunShaftsIntensity;
    int   sunShaftsDistance;
  } data;

  #if defined(COMPOSE)
    layout(set=1, binding=1) uniform sampler2D skySampler;
    layout(set=1, binding=2) uniform sampler2D sunSampler;
  #elif defined(SUN)
    layout(set=1, binding=6) uniform sampler2D cloudsSampler;
  #elif defined(CLOUDS)
    layout(set=1, binding=1) uniform sampler2D skySampler;
    layout(set=1, binding=3) uniform sampler2D cloudsMapSampler;
    layout(set=1, binding=4) uniform sampler3D cloudsNoiseLowSampler;
    layout(set=1, binding=5) uniform sampler3D cloudsNoiseHighSampler;
    layout(set=1, binding=7) uniform sampler2D g_ditherPatternSampler;
    layout(set=1, binding=8) uniform sampler2D g_noiseSampler;
    layout(set=1, binding=9) uniform sampler2D linearDepth;  
  #else
    layout(set=1, binding=9) uniform sampler2D linearDepth;  
  #endif
  
  layout(location=0) in vec2 fragTexcoord;
  layout(location=0) out vec4 outColor;
  
  #define INTEGRAL_STEPS 8
  #define INTEGRAL_STEPS_2 128
  
  // Physical and Artistic Constants
  const float earthRadius = 6371000.0f;        // Earth radius in meters
  const float atmosphereRadius = 160000.0f;    // Atmosphere thickness in meters
  const float cloudsStartHeight = 7000.0f;     // Height where clouds start above earth
  const float cloudsThickness = 15000.0f;      // Thickness of the cloud layer
  const float sunAngularRadius = radians(0.266f); // Sun's angular radius in radians
  const float maxHalfFloat = 65504.0f;
  const float bigDistance = 600000.0f;         // Used for max trace distance
  const uint integralSteps = 8u;               // Steps for density integration
  const uint integralSteps2 = 128u;            // Steps for atmospheric scattering
  #if defined(SUN)
  const uint sunTransmittanceSteps = 32u;
  #endif

  // Atmospheric positions are kept relative to the local tangent surface:
  // sea level is y=0 and the planet center is (0, -earthRadius, 0). This avoids
  // losing the low bits of meter-scale camera heights by adding earthRadius.
  float PlanetHeight(vec3 position)
  {
    const float verticalRadius = earthRadius + position.y;
    const float horizontalDistanceSquared = dot(position.xz, position.xz);
    const float radialDistance = sqrt(
      verticalRadius * verticalRadius + horizontalDistanceSquared);
    return position.y + horizontalDistanceSquared /
      max(radialDistance + verticalRadius, 1.0f);
  }

  vec2 RaySphereAtAltitude(vec3 origin, vec3 direction, float altitude)
  {
    const float a = dot(direction, direction);
    const float halfB = dot(direction, origin) + earthRadius * direction.y;
    const float heightDelta = origin.y - altitude;
    const float c = dot(origin.xz, origin.xz) +
      heightDelta * (2.0f * earthRadius + origin.y + altitude);
    const float discriminant = halfB * halfB - a * c;
    if(discriminant < 0.0f)
    {
      return vec2(-1.0f);
    }

    const float root = sqrt(discriminant);
    const float q = -halfB - (halfB >= 0.0f ? root : -root);
    if(abs(q) <= 0.000001f)
    {
      const float repeatedRoot = -halfB / max(a, 0.000001f);
      return vec2(repeatedRoot);
    }

    const float firstRoot = q / a;
    const float secondRoot = c / q;
    return firstRoot < secondRoot
      ? vec2(firstRoot, secondRoot)
      : vec2(secondRoot, firstRoot);
  }

  float NearestPositiveIntersection(vec2 intersections)
  {
    if(intersections.x > 0.0f)
    {
      return intersections.x;
    }

    return intersections.y > 0.0f ? intersections.y : -1.0f;
  }

  float RadialMotion(vec3 position, vec3 direction)
  {
    return dot(direction, vec3(
      position.x,
      earthRadius + position.y,
      position.z));
  }

  bool RayEntersAltitudeSphere(
    vec3 origin,
    vec3 direction,
    float altitude,
    out float distance)
  {
    const float heightFromBoundary = PlanetHeight(origin) - altitude;
    const float boundaryTolerance = 0.001f;
    distance = -1.0f;

    if(heightFromBoundary < -boundaryTolerance)
    {
      // The ray already starts inside this sphere. Its positive root exits the
      // sphere and therefore must not be treated as an opaque entry.
      return false;
    }

    if(abs(heightFromBoundary) <= boundaryTolerance)
    {
      if(RadialMotion(origin, direction) < 0.0f)
      {
        distance = 0.0f;
        return true;
      }

      return false;
    }

    distance = NearestPositiveIntersection(
      RaySphereAtAltitude(origin, direction, altitude));
    return distance > 0.0f;
  }

  bool ResolveAtmosphereRayOrigin(
    vec3 origin,
    vec3 direction,
    out vec3 atmosphereOrigin,
    out float distanceToAtmosphere)
  {
    atmosphereOrigin = origin;
    distanceToAtmosphere = 0.0f;
    if(PlanetHeight(origin) >= 0.0f)
    {
      return true;
    }

    // World-space zero is only the atmosphere datum, not a hard limit on game
    // coordinates. From below the datum, an outward ray enters the atmosphere
    // at the positive surface exit. An inward ray cannot see the sky.
    if(RadialMotion(origin, direction) < 0.0f)
    {
      return false;
    }

    const vec2 surfaceIntersections =
      RaySphereAtAltitude(origin, direction, 0.0f);
    const float surfaceExit = max(
      surfaceIntersections.x,
      surfaceIntersections.y);
    if(surfaceExit < 0.0f)
    {
      return false;
    }

    distanceToAtmosphere = surfaceExit;
    atmosphereOrigin = origin + direction * surfaceExit;
    return true;
  }

  float SunVisibility(vec3 position, vec3 directionToSun)
  {
    const vec3 radialPosition = vec3(
      position.x,
      earthRadius + position.y,
      position.z);
    const float motionTowardSun = dot(radialPosition, directionToSun);
    if(motionTowardSun >= 0.0f)
    {
      return 1.0f;
    }

    const float closestRadius = sqrt(max(
      dot(radialPosition, radialPosition) -
        motionTowardSun * motionTowardSun,
      0.0f));
    const float tangentAltitude = closestRadius - earthRadius;
    const float penumbraWidth = max(
      -motionTowardSun * tan(sunAngularRadius),
      1.0f);
    return smoothstep(
      -penumbraWidth,
      penumbraWidth,
      tangentAltitude);
  }
  
  float Density(vec3 a, vec3 b, float H0)
  {
    float res = 0.0f;
    
    float heightA = PlanetHeight(a);
    float heightB = PlanetHeight(b);
    float stepH = (heightB - heightA) / INTEGRAL_STEPS;
    
    float step = length(b - a) / INTEGRAL_STEPS;
    
    for(uint i = 0; i < INTEGRAL_STEPS; i++)
    {
        res += exp(-(heightA + stepH * i)/H0) * step;
    }
    
    return res;
  }

  #if defined(SUN)
  vec2 IntegrateSunDensity(
    vec3 origin,
    vec3 direction,
    float rangeStart,
    float rangeEnd,
    bool concentrateAtEnd)
  {
    if(rangeEnd <= rangeStart)
    {
      return vec2(0.0f);
    }

    const float rangeLength = rangeEnd - rangeStart;
    vec2 density = vec2(0.0f);
    for(uint index = 0u; index < sunTransmittanceSteps; ++index)
    {
      const float u0 = float(index) / float(sunTransmittanceSteps);
      const float u1 = float(index + 1u) / float(sunTransmittanceSteps);
      const float shapedU0 = concentrateAtEnd
        ? 1.0f - (1.0f - u0) * (1.0f - u0)
        : u0 * u0;
      const float shapedU1 = concentrateAtEnd
        ? 1.0f - (1.0f - u1) * (1.0f - u1)
        : u1 * u1;
      const float distance0 = rangeStart + rangeLength * shapedU0;
      const float distance1 = rangeStart + rangeLength * shapedU1;
      const float sampleDistance = 0.5f * (distance0 + distance1);
      const float stepLength = distance1 - distance0;
      const float height = max(
        PlanetHeight(origin + direction * sampleDistance),
        0.0f);
      density.x += exp(-height / 7994.0f) * stepLength;
      density.y += exp(-height / 1200.0f) * stepLength;
    }
    return density;
  }

  vec3 SunRayTransmittance(vec3 origin, vec3 directionToSun)
  {
    const float directionLengthSquared = dot(
      directionToSun,
      directionToSun);
    if(directionLengthSquared <= 0.000001f || PlanetHeight(origin) < 0.0f)
    {
      return vec3(0.0f);
    }

    const vec3 direction = directionToSun *
      inversesqrt(directionLengthSquared);
    float planetEntryDistance = -1.0f;
    if(RayEntersAltitudeSphere(
      origin,
      direction,
      0.0f,
      planetEntryDistance))
    {
      return vec3(0.0f);
    }

    const float originHeight = PlanetHeight(origin);
    const vec2 atmosphereIntersections = RaySphereAtAltitude(
      origin,
      direction,
      atmosphereRadius);
    float rangeStart = 0.0f;
    float rangeEnd = -1.0f;
    if(originHeight >= atmosphereRadius)
    {
      if(atmosphereIntersections.y <= 0.0f)
      {
        // No atmosphere lies between the observer and the Sun.
        return vec3(1.0f);
      }
      rangeStart = max(atmosphereIntersections.x, 0.0f);
      rangeEnd = atmosphereIntersections.y;
    }
    else
    {
      rangeEnd = max(
        atmosphereIntersections.x,
        atmosphereIntersections.y);
    }

    if(rangeEnd <= rangeStart)
    {
      return vec3(1.0f);
    }

    const vec3 radialPosition = vec3(
      origin.x,
      earthRadius + origin.y,
      origin.z);
    const float closestDistance = clamp(
      -dot(radialPosition, direction),
      rangeStart,
      rangeEnd);
    vec2 density = IntegrateSunDensity(
      origin,
      direction,
      rangeStart,
      closestDistance,
      true);
    density += IntegrateSunDensity(
      origin,
      direction,
      closestDistance,
      rangeEnd,
      false);

    const vec3 rayleighCoefficient = vec3(3.8e-6, 13.5e-6, 33.1e-6);
    const vec3 mieCoefficient = vec3(22e-6);
    return exp(
      -rayleighCoefficient * density.x -
      mieCoefficient * 1.1f * density.y);
  }

  vec3 SunDiskLighting(
    vec3 origin,
    vec3 direction,
    vec3 directionToSun)
  {
    const float theta = dot(direction, directionToSun);
    const float diskEdgeCosine = cos(sunAngularRadius);
    if(theta < diskEdgeCosine)
    {
      return vec3(0.0f);
    }

    const float normalizedRadius = clamp(
      (1.0f - theta) / max(1.0f - diskEdgeCosine, 0.000001f),
      0.0f,
      1.0f);
    const float limbDarkening = mix(
      0.83f,
      1.0f,
      1.0f - normalizedRadius * normalizedRadius);
    // Keep the explicit disk in Sailor's scene-linear emission scale. A true
    // lux-to-radiance conversion would exceed the R16F sky target and erase
    // the spectral energy loss by saturating every channel before bloom.
    const vec3 sourceEmission =
      max(data.sunIlluminance.xyz, vec3(0.0f));
    return min(
      limbDarkening * sourceEmission *
        SunRayTransmittance(origin, direction),
      vec3(maxHalfFloat));
  }
  #endif
  
  float PhaseR(float cosAngle)
  {
    return (3.0f / (16.0f * PI)) * (1.0f + cosAngle * cosAngle);
  }
  
  // The best variant with the precomputed values
  // taken from Github
  float PhaseMie(float x)
  {
    const vec3 c = vec3(.256098,.132268,.010016);
    const vec3 d = vec3(-1.5,-1.74,-1.98);
    const vec3 e = vec3(1.5625,1.7569,1.9801);
    return dot((x * x + 1.) * c / pow(d * x + e, vec3(1.5)),
      vec3(.33333333333)) / (4.0f * PI);
  }
  
  /*
  // Scratch pixel variant
  float PhaseMie(float cosAngle)
  {
    const float g = 0.76;
    const float phaseM = (3.0f / (8.0f * PI)) * ((1.0f - g * g) * (1.0f + cosAngle * cosAngle)) / 
    ((2.0f + g * g) * pow(1.0f + g * g - 2.0f * g * cosAngle, 1.5f));

    return phaseM;
  }*/
  
  float PhaseHenyeyGreenstein(float a, float g) 
  {
      const float safeCosine = clamp(a, -1.0f, 1.0f);
      const float safeEccentricity = clamp(g, -0.999f, 0.999f);
      const float g2 = safeEccentricity * safeEccentricity;
      const float denominator = max(
        1.0f + g2 - 2.0f * safeEccentricity * safeCosine,
        0.000001f);
      return (1.0f - g2) /
        (4.0f * PI * pow(denominator, 1.5f));
  }

  vec3 IntersectSphere(vec3 origin, vec3 direction, float innerHeight, float outerHeight)
  {
      vec2 intersections = RaySphereAtAltitude(origin, direction, outerHeight);
      float outer = NearestPositiveIntersection(intersections);

      if(outer <= 0.0f)
      {
        // Return just constant trace
        return origin;
      }
      
      // Max view distance
      const float maxCast = atmosphereRadius * 10;
      float shift = min(maxCast, outer);
      
      float inner = -1.0f;
      if(RayEntersAltitudeSphere(
        origin,
        direction,
        innerHeight,
        inner))
      {
          // The planet is opaque. Integrate only the atmosphere between the
          // observer and the first surface intersection.
          shift = min(shift, max(inner, 0.0f));
      }
      return origin + direction * shift;
  }

  vec3 DirectSunAtmosphereTransmittance(
    vec3 position,
    vec3 directionToSun)
  {
      const float visibility = SunVisibility(position, directionToSun);
      if(visibility <= 0.0f)
      {
          return vec3(0.0f);
      }

      const vec3 atmosphereExit = IntersectSphere(
        position,
        directionToSun,
        0.0f,
        atmosphereRadius);
      if(length(atmosphereExit - position) <= 0.01f)
      {
          return vec3(0.0f);
      }

      const vec3 rayleighCoefficient = vec3(3.8e-6, 13.5e-6, 33.1e-6);
      const vec3 mieCoefficient = vec3(22e-6);
      const float rayleighDensity = Density(
        position,
        atmosphereExit,
        7994.0f);
      const float mieDensity = Density(
        position,
        atmosphereExit,
        1200.0f);
      return visibility * exp(
        -rayleighCoefficient * rayleighDensity -
        mieCoefficient * 1.1f * mieDensity);
  }
  
  vec3 SkyLighting(vec3 origin, vec3 direction, vec3 lightDirection)
  {
     #if defined(SUN)
       return SunDiskLighting(origin, direction, -lightDirection);
     #endif

     vec3 atmosphereOrigin;
     float distanceToAtmosphere;
     if(!ResolveAtmosphereRayOrigin(
       origin,
       direction,
       atmosphereOrigin,
       distanceToAtmosphere))
     {
       return vec3(0.0f);
     }

     origin = atmosphereOrigin;
     const vec3 destination = IntersectSphere(origin, direction, 0.0f, atmosphereRadius);
     float groundDistance;
     const bool hitsGround = RayEntersAltitudeSphere(origin, direction, 0.0f, groundDistance);
     
     if(length(destination - origin) < 0.01)
     {
         return hitsGround ? max(data.groundRadiance.xyz, vec3(0.0f)) : vec3(0.0f);
     }

     const float Angle = dot(normalize(destination - origin), -lightDirection);
           
     const vec3 step = (destination - origin) / INTEGRAL_STEPS_2;
     
     // Constants for PhaseR
     const vec3 B0R = vec3(3.8e-6, 13.5e-6, 33.1e-6);
     const float H0R = 7994.0f;

     // Constants for PhaseMie
     const vec3 B0Mie = vec3(22e-6);
     const float H0Mie = 1200.0f;

     const float dStep = length(step);
     
     vec3 resR = vec3(0.0f);
     vec3 resMie = vec3(0.0f);

     float densityR = 0.0f;
     float densityMie = 0.0f;
     
     #if defined(SUN)
       const float theta = dot(direction, -lightDirection);
       const float zeta = cos(sunAngularRadius);
       if(theta < zeta)
       {
         return vec3(0);
       }
     #endif
     
     float phaseR = PhaseR(Angle);
     float phaseMie = PhaseMie(Angle);
     
     for(uint i = 0; i < integralSteps2; i++)
     {
         const vec3 point = origin + step * (float(i) + 0.5f);
         const float h = max(PlanetHeight(point), 0.0f);
         
         const float hr = exp(-h/H0R)  * dStep;
         const float hm = exp(-h/H0Mie) * dStep;
         
         densityR  += hr;
         densityMie += hm;

         const float sunVisibility = SunVisibility(
           point,
           -lightDirection);
         if(sunVisibility <= 0.0f)
         {
            continue;
         }

         const vec3  toLight = IntersectSphere(point, -lightDirection, 0.0f, atmosphereRadius);
         const float hLight  = max(PlanetHeight(toLight), 0.0f);
         const float stepToLight = (hLight - h) / INTEGRAL_STEPS;
         float dStepLight = length(toLight - point) / INTEGRAL_STEPS;
         float densityLightR = 0.0f;
         float densityLightMie = 0.0f;

         for(int j = 0; j < INTEGRAL_STEPS; j++)
         {
            const float h1 = h + stepToLight * j;
            
            if(h1 < 0)
            {
                break;
            }
            
            densityLightMie += exp(-h1/H0Mie) * dStepLight;
            densityLightR  += exp(-h1/H0R)  * dStepLight;
         }
        
        vec3 aggr = exp(-B0R * (densityR + densityLightR) - B0Mie * 1.1f * (densityLightMie + densityMie));
        resR  += aggr * hr * sunVisibility;
        resMie += aggr * hm * sunVisibility;
    }
    
    #if defined(SUN)
    // Sun disk
        float planetEntryDistance = -1.0f;
        if(!RayEntersAltitudeSphere(
          origin,
          direction,
          0.0f,
          planetEntryDistance))
        {
            const float t = (1 - pow((1 - theta)/(1-zeta), 2));
            const float diskEdgeAttenuation = mix(0.83, 1.0f, t);
            const float solidAngle = max(
              2.0f * PI * (1.0f - cos(sunAngularRadius)),
              0.000001f);
            const vec3 sunDiskRadiance =
              max(data.sunIlluminance.xyz, vec3(0.0f)) / solidAngle;
            const vec3 atmosphereTransmittance = exp(
              -B0R * densityR - B0Mie * 1.1f * densityMie);
            return min(
              diskEdgeAttenuation * sunDiskRadiance *
                atmosphereTransmittance,
              vec3(maxHalfFloat));
        }
        else
        {
            return vec3(0);
        }
    #else
        const vec3 scattering =
          B0R * resR * phaseR + B0Mie * resMie * phaseMie;
        const vec3 ground = hitsGround ? max(data.groundRadiance.xyz, vec3(0.0f)) *
          exp(-B0R * densityR - B0Mie * 1.1f * densityMie) : vec3(0.0f);
        return min(
          max(data.sunIlluminance.xyz, vec3(0.0f)) * scattering + ground,
          vec3(maxHalfFloat));
    #endif
  }
  
  float Remap(float value, float minValue, float maxValue, float newMinValue, float newMaxValue)
  {
    return newMinValue + (value-minValue) / (maxValue-minValue) * (newMaxValue-newMinValue);
  }

  #if defined(CLOUDS)
  float CloudsGetHeight(vec3 position)
  {
    return clamp((PlanetHeight(position) - cloudsStartHeight) / cloudsThickness, 0, 1);
  }
  
  float CloudsSampleDensity(vec3 position)
  {
    position.xz += vec2(0.1, 0.05) * frame.currentTime * 1000;
    
    vec3 shift1 = vec3(-0.0021, 0.0017, -0.02f) * frame.currentTime * -0.5;
    vec3 shift2 = vec3(0.021, 0.017, 0.0f) * frame.currentTime * -0.2;
    
    // Implicit derivatives are undefined in this divergent ray-march path and
    // produced different mip selection on native Vulkan and Metal/MoltenVK.
    const float cloudsLow = textureLod(cloudsNoiseLowSampler,
      shift1 + position.xyz / 9000.0f, 0.0f).r;
    const float cloudsHigh = textureLod(cloudsNoiseHighSampler,
      shift2 + position.xyz / 1300.0f, 0.0f).r;

    vec2 uv = position.xz / 409600.0f + vec2(0.2, 0.1);

    vec4 weather = textureLod(cloudsMapSampler, uv, 0.0f);

    float height = CloudsGetHeight(position);
    
    float SRb = clamp(Remap(height, 0, 0.07, 0, 1), 0, 1);
    float SRt = clamp(Remap(height, weather.b * 0.35, weather.b, 1, 0), 0, 1);
    
    float SA = SRb * SRt;
    
    float DRb = height * clamp(Remap(height, 0, 0.15, 0, 1), 0, 1);
    float DRt = height * clamp(Remap(height, 0.9, 1, 1, 0), 0, 1);
    
    float DA = DRb * DRt * weather.a * 2 * data.cloudsDensity;
    
    float SNsample = cloudsLow * 0.85f + cloudsHigh * 0.15f;
    
    float WMc = max(weather.r, clamp(data.cloudsCoverage - 0.5, 0, 1) * weather.g * 2);
    
    float d = clamp(Remap(SNsample * SA, 1 - data.cloudsCoverage * WMc, 1, 0, 1), 0, 1) * DA;
    
    return d;
  }
  
  float CloudsSampleDirectDensity(vec3 position, vec3 dirToSun)
  {
    float avrStep = cloudsThickness * 0.01f;
    float sumDensity = 0.0;
    
    for(int i = 0; i < 4; i++)
    {
        float step = avrStep;
        
        if(i == 3)
        {
            step = step * 6.0;
        }
        
        position += dirToSun * step;
        
        float density = CloudsSampleDensity(position) * step;
        sumDensity += density;
    }
    
    return sumDensity;
  } 
  
  vec4 CloudsMarching(vec3 origin, vec3 viewDir, vec3 dirToSun, float maxTraceDistance)
  {
    vec3 traceStart;
    vec3 traceEnd;
    vec3 atmosphereOrigin;
    float distanceToAtmosphere;
    if(!ResolveAtmosphereRayOrigin(
      origin,
      viewDir,
      atmosphereOrigin,
      distanceToAtmosphere))
    {
      return vec4(0.0f);
    }

    origin = atmosphereOrigin;
    maxTraceDistance = max(
      maxTraceDistance - distanceToAtmosphere,
      0.0f);
    const float originHeight = PlanetHeight(origin);
    
    // Trace inner and outer spheres
    vec2 cloudsStartIntersections = RaySphereAtAltitude(origin, viewDir, cloudsStartHeight);
    vec2 cloudsEndIntersections = RaySphereAtAltitude(origin, viewDir, cloudsStartHeight + cloudsThickness);
    const float planetIntersection = NearestPositiveIntersection(
      RaySphereAtAltitude(origin, viewDir, 0.0f));
    const float traceLimit = planetIntersection > 0.0f
      ? min(maxTraceDistance, planetIntersection)
      : maxTraceDistance;
    
    const float shiftCloudsStart = cloudsStartIntersections.x < 0 ? max(0, cloudsStartIntersections.y) : cloudsStartIntersections.x;
    const float shiftCloudsEnd = min(traceLimit, cloudsEndIntersections.x < 0 ? max(0, cloudsEndIntersections.y) : cloudsEndIntersections.x);
    
    if(shiftCloudsStart >= traceLimit ||
       (shiftCloudsStart > shiftCloudsEnd && cloudsEndIntersections.x < 0))
    {
        return vec4(0.0f);
    }

    if(originHeight < cloudsStartHeight)
    {
        traceStart = origin + viewDir * shiftCloudsStart;
        traceEnd = origin + viewDir * shiftCloudsEnd;
    }
    else if(originHeight > cloudsStartHeight + cloudsThickness)
    {
        traceStart = origin + viewDir * shiftCloudsEnd;
        traceEnd = origin + viewDir * shiftCloudsStart;
    }
    else
    {
        traceStart = origin;
        
        if(shiftCloudsStart == 0)
        {
            traceEnd = origin + viewDir * shiftCloudsEnd;
        }
        else if(shiftCloudsEnd == 0)
        {
            traceEnd = origin + viewDir * shiftCloudsStart;
        }
        else
        {
            traceEnd = origin + viewDir * min(shiftCloudsStart, shiftCloudsEnd);       
        }
    }
    
    // We traced the opposite Earth side,
    // that means there is no clouds on our way
    if(shiftCloudsStart > bigDistance)
    {
        return vec4(0);
    }
    
    const float mu = clamp(dot(viewDir, dirToSun), -1.0f, 1.0f);
    
    float dA[10];
    float dB[10];
    float dC[10];
    
    for(int j = 0; j < data.scatteringSteps; j++)
    {
       dA[j] = pow(data.scatteringDensity, j);
       dB[j] = pow(data.scatteringIntensity, j);
       dC[j] = pow(data.scatteringPhase, j);
    }
    
    const uint StepsHighDetail = 128;
    const uint StepsLowDetail = 256;
    
    vec3 position = traceStart;
    vec3 colorLow = vec3(0.0);
    float transmittanceLow = 1.0f;
    const vec3 directSunIlluminance =
      max(data.sunIlluminance.xyz, vec3(0.0f)) *
      DirectSunAtmosphereTransmittance(traceStart, dirToSun);

    float baseStep = 150.0;
    float density = CloudsSampleDensity(position);
    float avrStep = mix(baseStep * 2.0, baseStep * 0.5, clamp(density, 0.0, 1.0));

    position = traceStart;
    
    for(int i = 0; i < StepsHighDetail + StepsLowDetail; i++)
    {
        float density = CloudsSampleDensity(position) * avrStep;
        if(density > 0)
        {
            for(int j = 0; j < data.scatteringSteps; j++)
            {
                vec3 randomVec = vec3(0);
                if(j > 0)
                {
                    const vec3 randomSample =
                      texture(g_noiseSampler, position.xz + j / 16.0f).xyz -
                      0.5f;
                    const float randomLengthSquared = dot(
                      randomSample,
                      randomSample);
                    randomVec = randomLengthSquared > 1e-12f
                      ? randomSample * inversesqrt(randomLengthSquared) * 10.0f
                      : vec3(0.0f);
                }
                
                vec3 localPosition = position + randomVec;
                
                float sunDensity = CloudsSampleDirectDensity(localPosition, dirToSun);
                
                float m11 = data.phaseInfluence1 * PhaseHenyeyGreenstein(mu, dC[j] * data.eccentrisy1);
                float m12 = data.phaseInfluence2 * PhaseHenyeyGreenstein(mu, dC[j] * data.eccentrisy2);
                float m2 = exp(-dA[j] * data.cloudsAttenuation1 * sunDensity);
                float m3 = data.cloudsAttenuation2 * density;
                
                const float sunVisibility = SunVisibility(
                  localPosition,
                  dirToSun);
                if(sunVisibility > 0.0f)
                {
                    colorLow += sunVisibility * dB[j] *
                      (m11 + m12) * m2 * m3 * transmittanceLow;
                }
                
            }

            // Multiple-scattering orders alter in-scattered radiance, not the
            // primary view-ray extinction. Applying extinction per order made
            // cloud opacity depend on the quality setting.
            transmittanceLow *= exp(
              -data.cloudsAttenuation1 * density);
        }
        
        position += viewDir * avrStep;
        float height = PlanetHeight(position);

        // Early out if fully opaque or out of bounds
        if(transmittanceLow < 0.01 ||
           height > cloudsStartHeight + cloudsThickness ||
           height < cloudsStartHeight ||
           length(position - traceStart) > maxTraceDistance)
        {
            break;
        }
        if(i >= StepsHighDetail)
        {
            avrStep += 4;
        }
    }
   
    vec4 finalColor = vec4(
      min(
        data.cloudScatteringScale * directSunIlluminance * colorLow,
        vec3(maxHalfFloat)),
      1.0 - transmittanceLow);
    return finalColor;
  }
  
  #endif
  
  void BuildSunBasis(vec3 dirToSun, out vec3 right, out vec3 up)
  {
      const vec3 referenceAxis = abs(dirToSun.y) < 0.999f
        ? vec3(0.0f, 1.0f, 0.0f)
        : vec3(1.0f, 0.0f, 0.0f);
      right = normalize(cross(dirToSun, referenceAxis));
      up = normalize(cross(right, dirToSun));
  }
  
  void main()
  {
    outColor = vec4(0.0f, 0.0f, 0.0f, 1.0f);
    vec4 dirWorldSpace = vec4(0);
    
    // Keep the actual camera position. Atmospheric functions resolve a ray
    // crossing the datum geometrically, including cameras below world y=0.
    const vec3 origin = frame.cameraPosition.xyz;
    const vec3 dirToSun = normalize(-data.lightDirection.xyz);
    
    #if defined(COMPOSE)
       vec2 uv = fragTexcoord.xy;
       uv.y = 1 - uv.y;
       
       dirWorldSpace.xyz = ScreenSpaceToViewSpace(uv, 1.0f, frame.invProjection).xyz;
       dirWorldSpace.z *= -1;
       dirWorldSpace = normalize(inverse(frame.view) * dirWorldSpace);
        
       outColor.xyz = texture(skySampler, fragTexcoord).xyz; 

       vec3 right;
       vec3 up;
       BuildSunBasis(dirToSun, right, up);

       float dx = dot(dirWorldSpace.xyz - dirToSun, right);
       float dy = dot(dirWorldSpace.xyz - dirToSun, up);
       
       const float angle = atan(dot(cross(dirWorldSpace.xyz, dirToSun), up), dot(dirWorldSpace.xyz, dirToSun));
            
       if(dx > -sunAngularRadius && 
          dy > -sunAngularRadius && 
          dx < sunAngularRadius && 
          dy < sunAngularRadius && 
          abs(angle) < PI * 0.5)
       {
         vec2 sunUv = ((vec2(dx, dy) / sunAngularRadius) + 1.0f) / 2.0f;
         sunUv.y = 1 - sunUv.y;
         sunUv.x = 1 - sunUv.x;
         const vec3 sunColor = texture(sunSampler, sunUv).xyz;
         outColor.xyz += sunColor;
         if(any(greaterThan(sunColor, vec3(0.0f))))
         {
           // Reserve zero alpha on the opaque sky for the explicit solar disk.
           // Bloom uses it to retain solar energy without weakening the global
           // firefly filter for stars and isolated HDR pixels.
           outColor.a = 0.0f;
         }
       }

    #elif defined(CLOUDS)
      const float cloudsVisibilityEpsilon = 0.0001f;
      if(data.cloudsDensity <= cloudsVisibilityEpsilon ||
         data.cloudsCoverage <= cloudsVisibilityEpsilon)
      {
          outColor = vec4(0.0f);
          return;
      }

      #if defined(DITHER)
       vec2 ditherUv = vec2(mod(gl_FragCoord.x, 4), mod(gl_FragCoord.y, 4)) / 4.0f;
       
       float dither = mod(round(texture(g_ditherPatternSampler, ditherUv).r * 255), 4);
       if(dither != mod(PushConstants.ditherPattern, 4))
       {
           discard;
       }
      #endif
      
      float linearDepth = abs(texture(linearDepth, fragTexcoord.xy).r);
      #if defined(DISCARD_BY_DEPTH)
       if(linearDepth < 20000.0f)
       {
          discard;
       }
      #endif

       vec2 uv = fragTexcoord.xy;
       uv.y = 1 - uv.y;
       
       dirWorldSpace.xyz = ScreenSpaceToViewSpace(uv, 1.0f, frame.invProjection).xyz;
       dirWorldSpace.z *= -1;
       dirWorldSpace = normalize(inverse(frame.view) * dirWorldSpace);
        
       const vec3 skyRadiance = max(
         texture(skySampler, fragTexcoord).xyz,
         vec3(0.0f));

       vec3 viewDir = normalize(dirWorldSpace.xyz);
       float horizon = 1.0f;
       
       horizon -= exp(-abs(dot(viewDir, vec3(0.0, 1.0, 0.0))) * data.fog);
       horizon = horizon * horizon * horizon;
       horizon += 1 - clamp((cloudsStartHeight - PlanetHeight(origin)) / 500, 0, 1);
       horizon = clamp(horizon, 0, 1);
       
       float maxTraceDistance = abs(linearDepth - frame.cameraZNearZFar.y) < 1.0f ? bigDistance : linearDepth;

       const vec4 rawClouds = CloudsMarching(
         origin,
         viewDir,
         dirToSun,
         maxTraceDistance);
       const float opacity = clamp(rawClouds.a * horizon, 0.0f, 1.0f);
       if(opacity <= cloudsVisibilityEpsilon)
       {
           outColor = vec4(0.0f);
           return;
       }

       // CloudsMarching integrates direct radiance in premultiplied form.
       // Approximate hemispherical skylight with a bounded artistic weight and
       // keep it in the same HDR units as the atmospheric sky. The cloud pass
       // uses standard alpha blending, so convert the complete premultiplied
       // result to straight alpha exactly once before composition.
       const float ambientWeight = 1.0f - exp(-max(data.ambient, 0.0f));
       const vec3 ambientScattering =
         skyRadiance * ambientWeight * rawClouds.a;
       const vec3 premultipliedRadiance =
         (rawClouds.xyz + ambientScattering) * horizon;
       outColor = vec4(
         premultipliedRadiance / max(opacity, cloudsVisibilityEpsilon),
         opacity);
    #elif defined(SUN)
    
        // World space
        const vec2 sunAngular = vec2(mix(-sunAngularRadius, sunAngularRadius, fragTexcoord.x),
                                    mix(-sunAngularRadius, sunAngularRadius, fragTexcoord.y));

        vec3 right;
        vec3 up;
        BuildSunBasis(dirToSun, right, up);

        vec3 viewDir = Rotate(dirToSun, up, sunAngular.x);
        dirWorldSpace.xyz = normalize(Rotate(viewDir, cross(dirToSun, up), sunAngular.y));
        
        outColor = vec4(0,0,0,0);

        const vec4 sunClip = frame.projection * frame.view * dirWorldSpace;
        float cloudOpacity = 0.0f;
        if(sunClip.w > 0.000001f)
        {
            const vec2 sunNdc = sunClip.xy / sunClip.w;
            const vec2 sunUv = vec2(
              sunNdc.x * 0.5f + 0.5f,
              0.5f - sunNdc.y * 0.5f);
            if(all(greaterThanEqual(sunUv, vec2(0.0f))) &&
               all(lessThanEqual(sunUv, vec2(1.0f))))
            {
                cloudOpacity = texture(cloudsSampler, sunUv).a;
            }
        }

        const float cloudTransmittance =
          1.0f - clamp(cloudOpacity, 0.0f, 1.0f);
        outColor.xyz = SkyLighting(origin, dirWorldSpace.xyz, -dirToSun) * cloudTransmittance;
        
    #else
        vec2 uv = fragTexcoord.xy;
        uv.y = 1 - uv.y;
      
      #if defined(DISCARD_BY_DEPTH)
        float linearDepth = abs(texture(linearDepth, uv).r);
        if(linearDepth < 20000.0f)
        {
           discard;
        }
      #endif
      
        dirWorldSpace.xyz = ScreenSpaceToViewSpace(fragTexcoord.xy, 1.0f, frame.invProjection).xyz;
        dirWorldSpace.z *= -1;
        dirWorldSpace = normalize(inverse(frame.view) * dirWorldSpace);
        
        outColor.xyz = SkyLighting(origin, dirWorldSpace.xyz, -dirToSun);
    #endif

    if(any(isnan(outColor.xyz)) || any(isinf(outColor.xyz)))
    {
        outColor.xyz = vec3(0.0f);
    }
    outColor.xyz = clamp(outColor.xyz, vec3(0.0f), vec3(maxHalfFloat));
    if(isnan(outColor.a) || isinf(outColor.a))
    {
        outColor.a = 0.0f;
    }
    outColor.a = clamp(outColor.a, 0.0f, 1.0f);
  }
