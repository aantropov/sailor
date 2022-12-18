--- 
includes :
- Shaders/Math.glsl

defines :
- FILL
- SUN
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
          shift = inner*3;
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
         
       const float theta = dot(dirWorldSpace.xyz, dirToSun);
       const float zeta = cos(SunAngularR);
       
       if(dx > -SunAngularR && 
          dy > -SunAngularR && 
          dx < SunAngularR && 
          dy < SunAngularR)
       {
         vec2 sunUv = ((vec2(dx, dy) / SunAngularR) + 1.0f) / 2.0f;
         sunUv.y = 1 - sunUv.y;
         
         const vec3 sunColor = texture(sunSampler, sunUv).xyz;
         float luminance = dot(sunColor,sunColor);
         
         outColor.xyz = max(outColor.xyz, mix(outColor.xyz, sunColor, clamp(0,1, luminance)));
       }
       
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
