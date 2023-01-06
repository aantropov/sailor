--- 
includes:
- Shaders/Math.glsl
- Shaders/Noise.glsl

defines:
- FILL
- SUN
- CLOUDS_NOISE_HIGH
- CLOUDS_NOISE_LOW
- COMPOSE

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
  
  void main() 
  {
      gl_Position = vec4(inPosition, 1);
      fragTexcoord = inTexcoord;

      #if !defined(COMPOSE)
        fragTexcoord.y = 1.0f - fragTexcoord.y;
      #endif
  }
  
glslFragment: |
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
  } data;

  #if defined(COMPOSE)
  layout(set=1, binding=1) uniform sampler2D skySampler;
  layout(set=1, binding=2) uniform sampler2D sunSampler;
  layout(set=1, binding=3) uniform sampler2D cloudsMapSampler;
  layout(set=1, binding=4) uniform sampler2D cloudsNoiseLowSampler;
  layout(set=1, binding=5) uniform sampler2D cloudsNoiseHighSampler;  
  #endif
  
  layout(location = 0) in vec2 fragTexcoord;
  layout(location = 0) out vec4 outColor;
  
  #define INTEGRAL_STEPS 8
  #define INTEGRAL_STEPS_2 32
  
  const float R = 6371000.0f; // Earth radius in m
  const float AtmosphereR = 100000.0f; // Atmosphere radius
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
  
  vec3 IntersectSphere(vec3 origin, vec3 direction, float innerR, float outerR)
  {
      float outer = RaySphereIntersect(origin, direction, vec3(0), outerR).y;
      
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
  
  vec3 CalculateSunIlluminance(vec3 sunDirection)
  {
      const float w = 2*PI*(1-cos(SunAngularR));
      const float LsZenith = 120000.0f / w;
      const float LsGround = 100000.0f / w;
      
      const vec3 GroundIlluminance = LsGround * vec3(0.0499, 0.004, 4.10 * 0.00001);
      const vec3 ZenithIlluminance =  LsZenith * vec3(0.925, 0.861, 0.755);
      const float artisticTune = pow(dot(-sunDirection, vec3(0, 1, 0)), 3);
      
      return mix(GroundIlluminance, ZenithIlluminance, artisticTune);
  }
  
  vec3 SkyLighting(vec3 origin, vec3 direction, vec3 lightDirection)
  {
     const vec3 destination = IntersectSphere(origin, direction, R, R + AtmosphereR);
     
     const float LightIntensity = 1.0f;
     const float Angle = dot(normalize(destination - origin), -lightDirection);
           
     const vec3 step = (destination - origin) / INTEGRAL_STEPS_2;
     
     // Constants for PhaseR
     const vec3 B0R = vec3(5.8, 13.5, 33.1) * vec3(0.000001, 0.000001, 0.000001);
     const float H0R = 8000.0f;

     // Constants for PhaseMie
     const vec3 B0Mie = vec3(2.0, 2.0, 2.0) * vec3(0.000001);
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
         
         float dStepLight = length(point - toLight) / INTEGRAL_STEPS;
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
            vec3 aggr = exp(-B0R * (densityR + densityLightR) - B0Mie * (densityLightMie + densityMie));
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
            const vec3 SunIlluminance = attenuation * CalculateSunIlluminance(lightDirection);
            const vec3 final = SunIlluminance * (resR * B0R * PhaseR(Angle) + B0Mie * resMie * PhaseMie(Angle));
            return final;
        }
        else
        {
            return vec3(0);
        }
    #else
        const vec3 final = LightIntensity * (B0R * resR * PhaseR(Angle) + 1.1f * B0Mie * resMie * PhaseMie(Angle));
        return final;
    #endif
  }
  
  float Remap(float value, float minValue, float maxValue, float newMinValue, float newMaxValue)
  {
    return newMinValue + (value-minValue) / (maxValue-minValue) * (newMaxValue-newMinValue);
  }

  #if defined(COMPOSE)
  float CloudsGetHeight(vec3 position)
  {
    const float StartClouds = 6415;
    const float EndClouds = 6435;
    
    return clamp(((length(position) -  StartClouds) / (EndClouds - StartClouds)), 0, 1);
  }
  
  float CloudsSampleDensity(vec3 position)
  {
    //position.xz + = vec2(0.2f) * frame.time;
    
    const float cloudsLow = texture(cloudsNoiseLowSampler, position.xy / 48.0f).r;
    const float cloudsHigh = texture(cloudsNoiseHighSampler, position.xy / 4.8f).r;
    
    vec4 weather = textureLod(cloudsMapSampler, position.xz / 4096.0f, 0);
    float height = CloudsGetHeight(position);
    
    float SRb = clamp(Remap(height, 0, 0.07, 0, 1), 0, 1);
    float SRt = clamp(Remap(height, weather.b * 0.2, weather.b, 1, 0), 0, 1);
    
    float SA = SRb * SRt;
    
    float DRb = height * clamp(Remap(height, 0, 0.15, 0, 1), 0, 1);
    float DRt = height * clamp(Remap(height, 0.9, 1, 1, 0), 0, 1);
    
    // TODO: density parameter
    float cloudsDensity = 1.0f;
    float DA = DRb * DRt * weather.a * 2 * cloudsDensity;
    
    float SNsample = cloudsLow * 0.85f + cloudsHigh * 0.15f;
    
    // TODO: coverage parameter
    float coverage = 0.5f;
    float WMc = max(weather.r, clamp(coverage - 0.5, 0, 1) * weather.g * 2);
    
    float d = clamp(Remap(SNsample * SA, 1 - coverage * WMc, 1, 0, 1), 0, 1) * DA;
    
    return d;
  }
  
  float CloudsSampleDirectDensity(vec3 position, vec3 sunDir)
  {
    float avrStep = (6435.0 - 6415.0) * 0.01;
    float sumDensity = 0.0;
    
    for(int i = 0; i < 4; i++)
    {
        float step = avrStep;
        
        if(i == 3)
        {
            step = step * 6.0;
        }
        
        position += sunDir * step;
        
        float density = CloudsSampleDensity(position) * step;
        
        sumDensity += density;
    }
    
    return sumDensity;
  }
  
  vec4 CloudsMarching(vec3 viewDir, vec3 sunDir)
  {
    vec3 origin = vec3(0.0, 6400.0, 0.0);
    
  	const float shift = RaySphereIntersect(origin, viewDir, vec3(0), 6415.0).y;
    vec3 position = origin + viewDir * shift;
  	
  	const float avrStep = (6435.0 - 6415.0) / 64.0;
  	
    vec3 color = vec3(0.0);
    float transmittance = 1.0;

    vec3 sun = CalculateSunIlluminance(sunDir);
    
  	for(int i = 0; i < 128; i++)
  	{
  		position += viewDir * avrStep;
        
  		if(length(position) > 6435.0)
        {
  			break;
        }
        
        float density = CloudsSampleDensity(position) * avrStep;
        float sunDensity = CloudsSampleDirectDensity(position, sunDir);
        
        // TODO: Params cloudsAttenuation1, cloudsAttenuation2
        float cloudsAttenuation1 = 0.1;
        float cloudsAttenuation2 = 0.2;
        
        float m2 = exp(-cloudsAttenuation1 * sunDensity);
        float m3 = cloudsAttenuation2 * density;
        
        color += sun * m2 * m3 * transmittance;
        transmittance *= exp(-cloudsAttenuation1 * density);
  	}
  	
  	return vec4(color, 1.0 - transmittance);
  }
  
  #endif
  
  void main()
  {
    vec4 dirWorldSpace = vec4(0);
    const vec3 origin = vec3(0, R + 1000, 0) + frame.cameraPosition.xyz;
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
         
         const vec3 sunColor = texture(sunSampler, sunUv).xyz;
         float luminance = dot(sunColor,sunColor);
         
         outColor.xyz = max(outColor.xyz, mix(outColor.xyz, sunColor, clamp(0,1, luminance)));
       }

       //vec4 clouds = CloudsMarching(dirWorldSpace.xyz, dirToSun);
       //outColor.xyz = mix(clouds.xyz, outColor.xyz, clouds.a);
    #elif defined(CLOUDS_NOISE_HIGH)
    
      vec2 uv = fragTexcoord.xy;
      uv.y = 1 - uv.y;
      
      const float tiling = 5;
      
      const float cloudsHigh = 0.5f * (fBm(uv * tiling, 4) + 1) * 0.625f + 
                                  (1 - fBmCellular(uv * tiling * 2, 4)) * 0.25f + 
                                  (1 - fBmCellular(uv * tiling * 3, 4)) * 0.125f;
      outColor.x = cloudsHigh;
      
    #elif defined(CLOUDS_NOISE_LOW)
      
      vec2 uv = fragTexcoord.xy;
      uv.y = 1 - uv.y;
      
      const float tiling = 5;
      
      float perlinNoiseLow = pow((fBm(uv * tiling, 4) + 1) * 0.5, 1);
      float cellularNoiseLow = pow(1 - fBmCellular(uv * tiling, 4), 1);
      float cellularNoiseMid = pow(1 - fBmCellular(uv * tiling * 2, 4), 1);
      float cellularNoiseHigh = pow(1 - fBmCellular(uv * tiling * 3, 4), 1);
      
      const float cloudsLow = Remap(perlinNoiseLow, (cellularNoiseLow * 0.625f + cellularNoiseMid * 0.25f + cellularNoiseHigh * 0.125f) - 1.0f, 1.0f, 0.0f, 1.0f);
      
      outColor.x = cloudsLow;
      
    #elif defined(SUN)
    
        // World space
        const vec2 sunAngular = vec2(mix(-SunAngularR, SunAngularR, fragTexcoord.x),
                                    mix(-SunAngularR, SunAngularR, fragTexcoord.y));

        const vec3 right = normalize(cross(dirToSun, vec3(0,1,0)));
        const vec3 up = cross(right, dirToSun);
        
        vec3 viewDir = Rotate(dirToSun, up, sunAngular.x);
        dirWorldSpace.xyz = normalize(Rotate(viewDir, cross(dirToSun, up), sunAngular.y));
        
        outColor.xyz = SkyLighting(origin, dirWorldSpace.xyz, -dirToSun);
    #else
        dirWorldSpace.xyz = ScreenToView(fragTexcoord.xy, 1.0f, frame.invProjection).xyz;
        dirWorldSpace.z *= -1;
        dirWorldSpace = normalize(inverse(frame.view) * dirWorldSpace);
        
        outColor.xyz = SkyLighting(origin, dirWorldSpace.xyz, -dirToSun);
    #endif
  }
