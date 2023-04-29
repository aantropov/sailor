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
  layout(location=0) in vec3 inPosition;
  layout(location=1) in vec3 inNormal;
  layout(location=2) in vec2 inTexcoord;
  layout(location=3) in vec4 inColor;
  
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
      float currentTime;
      float deltaTime;
  } frame;

  layout(set=1, binding=0) uniform PostProcessDataUBO
  {
    vec4 lightDirection;
    float cloudsAttenuation1;
    float cloudsAttenuation2;
    float cloudsDensity;
    float cloudsCoverage;
    float phaseInfluence1;
    float phaseInfluence2;
    float eccentrisy1;
    float eccentrisy2;
    float fog;
    float sunIntensity;
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
      float currentTime;
      float deltaTime;
  } frame;

  layout(set=1, binding=0) uniform PostProcessDataUBO
  {
    vec4 lightDirection;
    float cloudsAttenuation1;
    float cloudsAttenuation2;
    float cloudsDensity;
    float cloudsCoverage;
    float phaseInfluence1;
    float phaseInfluence2;
    float eccentrisy1;
    float eccentrisy2;
    float fog;
    float sunIntensity;
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
  
  layout(location = 0) in vec2 fragTexcoord;
  layout(location = 0) out vec4 outColor;
  
  #define INTEGRAL_STEPS 8
  #define INTEGRAL_STEPS_2 128
  
  const float R = 6371000.0f; // Earth radius in m
  const float AtmosphereR = 160000.0f; // Atmosphere radius
  const float CloudsStartR = R + 7000.0f;
  const float CloudsEndR = CloudsStartR + 15000.0f;
  
  const float SunAngularR = radians(0.545f);
  
  float Density(vec3 a, vec3 b, float H0)
  {
    float res = 0.0f;
    
    float heightA = length(a) - R;
    float heightB = length(b) - R;
    float stepH = (heightB - heightA) / INTEGRAL_STEPS;
    
    float step = length(b - a) / INTEGRAL_STEPS;
    
    for(uint i = 0; i < INTEGRAL_STEPS; i++)
    {
        res += exp(-(heightA + stepH * i)/H0) * step;
    }
    
    return res;
  }
  
  float PhaseR(float cosAngle)
  {
    return ((3.0f * PI) / 16.0f) * (1.0f + cosAngle * cosAngle);
  }
  
  // The best variant with the precomputed values
  // taken from Github
  float PhaseMie(float x)
  {
    const vec3 c = vec3(.256098,.132268,.010016);
    const vec3 d = vec3(-1.5,-1.74,-1.98);
    const vec3 e = vec3(1.5625,1.7569,1.9801);
    return dot((x * x + 1.) * c / pow( d * x + e, vec3(1.5)),vec3(.33333333333));
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
      float g2 = g * g;
      return (1.0f - g2) / (4.0f * 3.1415f * pow(1.0f + g2 - 2.0f * g * (a), 1.5f));
  }

  vec3 IntersectSphere(vec3 origin, vec3 direction, float innerR, float outerR)
  {
      vec2 intersections = RaySphereIntersect(origin, direction, vec3(0), outerR);
      float outer = intersections.x < 0 ? intersections.y : intersections.x;

      if(outer <= 0.0f)
      {
        // Return just constant trace
        return origin;
      }
      
      // Max view distance
      const float maxCast = AtmosphereR * 10;
      float shift = min(maxCast, outer);
      
  #if defined(FILL)
      vec2 tmp = RaySphereIntersect(origin, direction, vec3(0), innerR);
      float inner = tmp.x > 0.0f ? tmp.x : tmp.y;
      
      if(inner > 0.0f)
      {
          // If we intersects the Earth we should tune
          //shift = AtmosphereR;
          shift = inner * 3;
      }
  #endif
      return origin + direction * shift;
  }
  
  vec3 CalculateSunColor(vec3 sunDirection)
  {
      const vec3 ZenithIlluminance =  vec3(0.925, 0.861, 0.755);
      const vec3 HalfIlluminance = vec3(0.6f, 0.4490196f, 0.1588f);
      const vec3 GroundIlluminance = vec3(0.0499, 0.004, 4.10 * 0.00001) * 2;
      
      const float angle = dot(-sunDirection, vec3(0, 1, 0));
      
      const float border = 0.1f;
      
      const float artisticTune1 = clamp(pow((angle - border) / (1.0f - border), 0.5), 0, 1);
      const float artisticTune2 = clamp(pow(angle / border, 3), 0, 1);
      
      vec3 color = angle > border ? mix(HalfIlluminance, ZenithIlluminance, artisticTune1) : 
      mix(GroundIlluminance, HalfIlluminance, artisticTune2);
      
      return color;
  }
  
  vec3 CalculateSunIlluminance(vec3 sunDirection)
  {
      const float w = 2*PI*(1-cos(SunAngularR));
      const float LsZenith = 120000.0f / w;
      const float LsGround = 100000.0f / w;
      
      const float artisticTune = pow(dot(-sunDirection, vec3(0, 1, 0)), 3);
      
      return mix(LsGround, LsZenith, artisticTune) * CalculateSunColor(sunDirection);
  }
  
  vec3 SkyLighting(vec3 origin, vec3 direction, vec3 lightDirection)
  {
     const vec3 destination = IntersectSphere(origin, direction, R, R + AtmosphereR);
     
     if(length(destination - origin) < 0.01)
     {
         return vec3(0);
     }

     const float LightIntensity = 7.0f;
     const float Angle = dot(normalize(destination - origin), -lightDirection);
           
     const vec3 step = (destination - origin) / INTEGRAL_STEPS_2;
     
     // Constants for PhaseR
     const vec3 B0R = vec3(3.8e-6, 13.5e-6, 33.1e-6);
     const float H0R = 7994.0f;

     // Constants for PhaseMie
     const vec3 B0Mie = vec3(22e-6);
     const float H0Mie = 1200.0f;

     const float heightOrigin = length(origin);
     const float dh = (length(destination) - heightOrigin) / INTEGRAL_STEPS_2;
     const float dStep = length(step);
     
     vec3 resR = vec3(0.0f);
     vec3 resMie = vec3(0.0f);

     float densityR = 0.0f;
     float densityMie = 0.0f;
     
     #if defined(SUN)
       const float theta = dot(direction, -lightDirection);
       const float zeta = cos(SunAngularR);
       if(theta < zeta)
       {
         return vec3(0);
       }
     #endif
     
     for(uint i = 0; i < INTEGRAL_STEPS_2 - 1; i++)
     {
         const vec3 point = origin + step * (i + 1);
         const float h = length(point) - R;
         
         const float hr = exp(-h/H0R)  * dStep;
         const float hm = exp(-h/H0Mie) * dStep;
         
         densityR  += hr;
         densityMie += hm;

         const vec3  toLight = IntersectSphere(point, -lightDirection, R, R + AtmosphereR);
         const float hLight  = length(toLight) - R;
         const float stepToLight = (hLight - h) / INTEGRAL_STEPS;
         
         float dStepLight = length(toLight - point) / INTEGRAL_STEPS;
         float densityLightR = 0.0f;
         float densityLightMie = 0.0f;

         bool bReached = true;
         for(int j = 0; j < INTEGRAL_STEPS; j++)
         {
            const float h1 = h + stepToLight * j;
            
            if(h1 < 0)
            {
                bReached = false;
                break;
            }
            
            densityLightMie += exp(-h1/H0Mie) * dStepLight;
            densityLightR  += exp(-h1/H0R)  * dStepLight;
         }
        
        if(bReached)
        {
            vec3 aggr = exp(-B0R * (densityR + densityLightR) - B0Mie * 1.1f * (densityLightMie + densityMie));
            resR  += aggr * hr;
            resMie += aggr * hm;
        }
    }
    
    #if defined(SUN)
    // Sun disk
        vec2 intersection = RaySphereIntersect(origin, direction, vec3(0), R);
        if(max(intersection.x, intersection.y) < 0.0f)
        {
            const float t = (1 - pow((1 - theta)/(1-zeta), 2));
            const float attenuation = mix(0.83, 1.0f, t);
            const vec3 SunIlluminance = attenuation * vec3(1.0f) * 12000000.0f;
            const vec3 final = SunIlluminance;// * (resR * B0R * PhaseR(Angle) + B0Mie * resMie * PhaseMie(Angle));
            return final;
        }
        else
        {
            return vec3(0);
        }
    #else
        const vec3 final = LightIntensity * (B0R * resR * PhaseR(Angle) + B0Mie * resMie * PhaseMie(Angle));
        return final;
    #endif
  }
  
  float Remap(float value, float minValue, float maxValue, float newMinValue, float newMaxValue)
  {
    return newMinValue + (value-minValue) / (maxValue-minValue) * (newMaxValue-newMinValue);
  }

  #if defined(CLOUDS)
  float CloudsGetHeight(vec3 position)
  {
    return clamp(((length(position.y) -  CloudsStartR) / (CloudsEndR - CloudsStartR)), 0, 1);
  }
  
  float CloudsSampleDensity(vec3 position)
  {
    position.xz += vec2(0.1, 0.05) * frame.currentTime * 1000;
    
    vec3 shift1 = vec3(-0.0021, 0.0017, -0.02f) * frame.currentTime * -0.5;
    vec3 shift2 = vec3(0.021, 0.017, 0.0f) * frame.currentTime * -0.2;
    
    const float cloudsLow = pow(texture(cloudsNoiseLowSampler, shift1 + position.xyz / 9000.0f).r, 1);
    const float cloudsHigh = pow(texture(cloudsNoiseHighSampler, shift2 + position.xyz / 1300.0f).r, 1);

    vec2 uv = position.xz / 409600.0f + vec2(0.2, 0.1);

    vec4 weather = texture(cloudsMapSampler, uv);

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
    float avrStep = (CloudsEndR - CloudsStartR) * 0.01f;
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
  
  vec4 CloudsMarching(vec3 origin, vec3 viewDir, vec3 dirToSun)
  {
    vec3 traceStart;
    vec3 traceEnd;
    const float originHeight = length(origin);
    
    // Trace inner and outer spheres
    vec2 cloudsStartIntersections = RaySphereIntersect(origin, viewDir, vec3(0), CloudsStartR);
    vec2 cloudsEndIntersections = RaySphereIntersect(origin, viewDir, vec3(0), CloudsEndR);
    
    const float shiftCloudsStart = cloudsStartIntersections.x < 0 ? max(0, cloudsStartIntersections.y) : cloudsStartIntersections.x;
    const float shiftCloudsEnd = cloudsEndIntersections.x < 0 ? max(0, cloudsEndIntersections.y) : cloudsEndIntersections.x;
    
    if(originHeight < CloudsStartR)
    {
        traceStart = origin + viewDir * shiftCloudsStart;
        traceEnd = origin + viewDir * shiftCloudsEnd;
    }
    else if(originHeight > CloudsEndR)
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
    
    const float BigDistance = 600000.0f;
    
    // We traced the opposite Earth side,
    // that means there is no clouds on our way
    if(shiftCloudsStart > BigDistance)
    {
        return vec4(0);
    }
    
    vec3 sunColor = CalculateSunColor(-dirToSun);
    float mu = max(0, dot(viewDir, dirToSun));
    
    float dA[10];
    float dB[10];
    float dC[10];
    
    for(int j = 0; j < data.scatteringSteps; j++)
    {
       dA[j] = pow(data.scatteringDensity, j);
       dB[j] = pow(data.scatteringIntensity, j);
       dC[j] = pow(data.scatteringPhase, j);
    }
    
    const vec3 finalTrace = traceEnd;
    const uint StepsHighDetail = 128;
    const uint StepsLowDetail = 256;
    
    vec3 position = traceStart;
    vec3 color = vec3(0.0);
    vec3 colorLow = vec3(0.0);
    float transmittance = 1.0;
    float transmittanceLow = 1.0f;

    // Perspective compensation
    /*vec4 cameraDir = vec4(0,0,0,0);
    cameraDir.xyz = ScreenToView(vec2(0.5, 0.5), 1.0f, frame.invProjection).xyz;    
    cameraDir.z *= -1;
    cameraDir = normalize(inverse(frame.view) * cameraDir);
    const float cosA = dot(cameraDir.xyz, viewDir);
    */

    float avrStep = 150;

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
                    randomVec = normalize(texture(g_noiseSampler, position.xz + j / 16.0f).xyz - 0.5f) * 10.0f;
                }
                
                vec3 localPosition = position + randomVec;
                
                float sunDensity = CloudsSampleDirectDensity(localPosition, dirToSun);
                
                float m11 = data.phaseInfluence1 * PhaseHenyeyGreenstein(mu, dC[j] * data.eccentrisy1);
                float m12 = data.phaseInfluence2 * PhaseHenyeyGreenstein(mu, dC[j] * data.eccentrisy2);
                float m2 = exp(-dA[j] * data.cloudsAttenuation1 * sunDensity);
                float m3 = data.cloudsAttenuation2 * density;
                
                vec2 intersections = RaySphereIntersect(localPosition, dirToSun, vec3(0), R);
        
                // No sun rays throw the Earth
                if(max(intersections.x, intersections.y) < 0)
                {
                    colorLow += dB[j] * (m11 + m12) * m2 * m3 * transmittanceLow;
                }
                
                transmittanceLow *= exp(-dA[j] * data.cloudsAttenuation1 * density);
            }
        }
        
        position += viewDir * avrStep;
        const float height = length(position);
        
        if(transmittanceLow < 0.05 || height > CloudsEndR || height < CloudsStartR)
        {
            break;
        }
        
        if(i >= StepsHighDetail)
        {
            avrStep += 4;
        }
    }
   
    vec4 finalColor = vec4(data.sunIntensity * sunColor * colorLow, 1.0 - transmittanceLow);
    return finalColor;
  }
  
  #endif
  
  float CalculateSunHeight(vec3 originWorldPos, vec3 worldViewDir, vec3 dirToSun)
  {
      const float l = (dot(worldViewDir, dirToSun) * length(originWorldPos));
      return length(l * worldViewDir + originWorldPos) - R;
  }
  
  void main()
  {
    vec4 dirWorldSpace = vec4(0);
    const vec3 origin = vec3(0, R, 0) + frame.cameraPosition.xyz;
    const vec3 dirToSun = normalize(-data.lightDirection.xyz);
    
    #if defined(COMPOSE)
       vec2 uv = fragTexcoord.xy;
       uv.y = 1 - uv.y;
       
       dirWorldSpace.xyz = ScreenToView(uv, 1.0f, frame.invProjection).xyz;
       dirWorldSpace.z *= -1;
       dirWorldSpace = normalize(inverse(frame.view) * dirWorldSpace);
        
       outColor.xyz = texture(skySampler, fragTexcoord).xyz; 

       const vec3 right = normalize(cross(dirToSun, vec3(0,1,0)));
       const vec3 up = cross(right, dirToSun);

       float dx = dot(dirWorldSpace.xyz - dirToSun, right);
       float dy = dot(dirWorldSpace.xyz - dirToSun, up);
       
       const float angle = atan(dot(cross(dirWorldSpace.xyz, dirToSun), up), dot(dirWorldSpace.xyz, dirToSun));
            
       if(dx > -SunAngularR && 
          dy > -SunAngularR && 
          dx < SunAngularR && 
          dy < SunAngularR && 
          abs(angle) < PI * 0.5)
       {
         vec2 sunUv = ((vec2(dx, dy) / SunAngularR) + 1.0f) / 2.0f;
         sunUv.y = 1 - sunUv.y;
         sunUv.x = 1 - sunUv.x;
         const vec3 sunColor = texture(sunSampler, sunUv).xyz;
         float luminance = dot(sunColor,sunColor);
         outColor.xyz = max(outColor.xyz, mix(outColor.xyz, sunColor, clamp(0,1, luminance)));         
       }

    #elif defined(CLOUDS)
      #if defined(DITHER)
       vec2 ditherUv = vec2(mod(gl_FragCoord.x, 4), mod(gl_FragCoord.y, 4)) / 4.0f;
       
       float dither = mod(round(texture(g_ditherPatternSampler, ditherUv).r * 255), 4);
       if(dither != mod(PushConstants.ditherPattern, 4))
       {
           discard;
       }
      #endif
      
      #if defined(DISCARD_BY_DEPTH)
       float linearDepth = abs(texture(linearDepth, fragTexcoord.xy).r);
       if(linearDepth < 20000.0f)
       {
          discard;
       }
      #endif


       vec2 uv = fragTexcoord.xy;
       uv.y = 1 - uv.y;
       
       dirWorldSpace.xyz = ScreenToView(uv, 1.0f, frame.invProjection).xyz;
       dirWorldSpace.z *= -1;
       dirWorldSpace = normalize(inverse(frame.view) * dirWorldSpace);
        
       outColor.xyz = texture(skySampler, fragTexcoord).xyz;
       
       // Remove horizon red line
       vec3 sky = vec3(outColor.b);
       sky = sky / (1 + sky);

       vec3 viewDir = normalize(dirWorldSpace.xyz);
       float horizon = 1.0f;
       
       horizon -= exp(-abs(dot(viewDir, vec3(0.0, 1.0, 0.0))) * data.fog);
       horizon = horizon * horizon * horizon;
       horizon += 1 - clamp((CloudsStartR - length(origin)) / 500, 0, 1);
       horizon = clamp(horizon, 0, 1);
       
       vec4 rawClouds = CloudsMarching(origin, viewDir, dirToSun) + vec4(sky.xyz, 0.0f) * data.ambient;
       vec3 tunedClouds = mix(outColor.xyz, rawClouds.xyz, horizon);
       
       outColor.xyz = tunedClouds;
       outColor.a = rawClouds.a;
    #elif defined(SUN)
    
        // World space
        const vec2 sunAngular = vec2(mix(-SunAngularR, SunAngularR, fragTexcoord.x),
                                    mix(-SunAngularR, SunAngularR, fragTexcoord.y));

        const vec3 right = normalize(cross(dirToSun, vec3(0,1,0)));
        const vec3 up = cross(right, dirToSun);

        vec3 viewDir = Rotate(dirToSun, up, sunAngular.x);
        dirWorldSpace.xyz = normalize(Rotate(viewDir, cross(dirToSun, up), sunAngular.y));
        
        outColor = vec4(0,0,0,0);

        vec4 uvView = ((frame.projection * frame.view * dirWorldSpace) + 1.0f) * 0.5f;
        uvView /= uvView.w;
        
        float clouds = texture(cloudsSampler, uvView.xy).a;
        
        if(clouds < 0.5)
        {
            outColor.xyz = SkyLighting(origin, dirWorldSpace.xyz, -dirToSun);
        }
        
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
      
        dirWorldSpace.xyz = ScreenToView(fragTexcoord.xy, 1.0f, frame.invProjection).xyz;
        dirWorldSpace.z *= -1;
        dirWorldSpace = normalize(inverse(frame.view) * dirWorldSpace);
        
        outColor.xyz = SkyLighting(origin, dirWorldSpace.xyz, -dirToSun);
    #endif
  }
