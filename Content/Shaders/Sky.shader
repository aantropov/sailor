--- 
includes :
  - Shaders/Lighting.glsl
defines : 
- FILL

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
      fragTexcoord.y = 1.0f - fragTexcoord.y;
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
    vec4 param;
  } data;
  
  layout(location = 0) in vec2 fragTexcoord;
  layout(location = 0) out vec4 outColor;
  
  #define INTEGRAL_STEPS 2
  #define INTEGRAL_STEPS_2 32
  
  const float R = 6371000.0f; // Earth radius in m
  const float AtmosphereR = 100000.0f; // Atmosphere radius
  const float SunAngularR = 0.5f * 5.45f;
  
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
  
  float PhaseMu(float cosAngle)
  {
    const float g = 0.9;
    const float phaseM = 3.0f / (8.0f * PI) * ((1.0f - g * g) * (1.0f + cosAngle * cosAngle)) / 
    ((2.0f + g * g) * pow(1.0f + g * g - 2.0f * g * cosAngle, 1.5f));

    return phaseM;
  }
  
  // https://gist.github.com/wwwtyro/beecc31d65d1004f5a9d
  vec2 raySphereIntersect(vec3 r0, vec3 rd, vec3 s0, float sr) 
  {
    // - r0: ray origin
    // - rd: normalized ray direction
    // - s0: sphere center
    // - sr: sphere radius
    // - Returns distance from r0 to first intersecion with sphere,
    //   or -1.0 if no intersection.
    float a = 1.0f;
    vec3 s0_r0 = r0 - s0;
    float b = 2.0 * dot(rd, s0_r0);
    float c = dot(s0_r0, s0_r0) - (sr * sr);
    if (b*b - 4.0*a*c < 0.0) 
    {
        return vec2(-1.0);
    }
    
    float tmp = sqrt((b*b) - 4.0*a*c);
    float x1 = (-b + tmp)/(2.0*a);
    float x2 = (-b - tmp)/(2.0*a);
    
    return x1 < x2 ? vec2(x1, x2) : vec2(x2, x1);
  }

  vec3 IntersectSphere(vec3 origin, vec3 direction, float innerR, float outerR)
  {
      float outer = raySphereIntersect(origin, direction, vec3(0), outerR).y;
      
      if(outer <= 0.0f)
      {
        // Return just constant trace
        return origin;
      }
      
      // Max view distance
      const float maxCast = AtmosphereR * 10;
      float shift = min(maxCast, outer);
      
  #if defined(FILL)
      vec2 tmp = raySphereIntersect(origin, direction, vec3(0), innerR);
      float inner = tmp.x > 0.0f ? tmp.x : tmp.y;
      
      if(inner > 0.0f)
      {
          // If we intersects the Earth we should tune
          //shift = AtmosphereR;
          shift = inner * 5;
      }
  #endif
      return origin + direction * shift;
  }
  
  vec3 CalculateSunIlluminance(vec3 sunDirection)
  {
      const float w = 2*PI*(1-cos(SunAngularR));
      const float Ls = 120000.0f / w;
      
      const vec3 GroundIlluminance = 15.0f * vec3(1);
      const vec3 ZenithIlluminance =  Ls * vec3(0.925, 0.861, 0.755);
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

     // Constants for PhaseMu
     const vec3 B0Mu = vec3(2.0, 2.0, 2.0) * vec3(0.000001);
     const float H0Mu = 1200.0f;

     const float heightOrigin = length(origin);
     const float dh = (length(destination) - heightOrigin) / INTEGRAL_STEPS_2;
     const float dStep = length(step);
     
     vec3 resR = vec3(0.0f);
     vec3 resMu = vec3(0.0f);

     float densityR = 0.0f;
     float densityMu = 0.0f;
     
     for(uint i = 0; i < INTEGRAL_STEPS_2 - 1; i++)
     {
         const vec3 point = origin + step * (i + 1);
         const float h = length(point) - R;
         
         const float hr = exp(-h/H0R)  * dStep;
         const float hm = exp(-h/H0Mu) * dStep;
         
         densityR  += hr;
         densityMu += hm;

         const vec3  toLight = IntersectSphere(point, -lightDirection, R, R + AtmosphereR);
         const float hLight  = length(toLight) - R;
         const float stepToLight = (hLight - h) / INTEGRAL_STEPS;
         
         float dStepLight = length(point - toLight) / INTEGRAL_STEPS;
         float densityLightR = 0.0f;
         float densityLightMu = 0.0f;

         bool bReached = true;
         for(int j = 0; j < INTEGRAL_STEPS; j++)
         {
            const float h1 = h + stepToLight * j;
            
            if(h1 < 0)
            {
                bReached = false;
                break;
            }
            
            densityLightMu += exp(-h1/H0Mu) * dStepLight;
            densityLightR  += exp(-h1/H0R)  * dStepLight;
         }
        
        if(bReached)
        {
            vec3 aggr = exp(-B0R * (densityR + densityLightR) + -1.1f * B0Mu * (densityLightMu + densityMu));
            resR  += aggr * hr;
            resMu += aggr * hm;
        }
    }
    
    // Sun disk
    const float angularSunSize = SunAngularR / 90.0f;
    const float theta = dot(direction, -lightDirection);
    const float zeta = cos(angularSunSize);
    if(theta > zeta)
    {   
        vec2 intersection = raySphereIntersect(origin, direction, vec3(0), R);

        if(max(intersection.x, intersection.y) < 0.0f)
        {
            const float t = (1 - pow((1 - theta)/(1-zeta), 2));
            const float attenuation = mix(0.765, 1.0f, t);
            const vec3 SunIlluminance = attenuation * CalculateSunIlluminance(lightDirection);
            const vec3 final = SunIlluminance * (resR * B0R * PhaseR(Angle) + B0Mu * resMu * PhaseMu(Angle));
            return final;
        }
    }
     
    const vec3 final = LightIntensity * (B0R * resR * PhaseR(Angle) + B0Mu * resMu * PhaseMu(Angle));
    return final;
  }
  
  void main()
  {
    // Calculate view direction
    vec4 dirViewSpace = vec4(0);
    dirViewSpace.xyz = ScreenToView(fragTexcoord.xy, 1.0f, frame.invProjection).xyz;
    dirViewSpace.z *= -1;
    dirViewSpace = normalize(inverse(frame.view) * dirViewSpace);

    // View position
    vec3 origin = vec3(0, R + 1000, 0) + frame.cameraPosition.xyz;
    
    vec3 sunDirection1 = normalize(vec3(0, -1, 0));
    vec3 sunDirection2 = normalize(vec3(0, 0.02, 1));
    
    outColor.xyz = fragTexcoord.x > 0.5 ? 
    SkyLighting(origin, dirViewSpace.xyz, sunDirection1) :
    SkyLighting(origin, dirViewSpace.xyz, sunDirection2);
    
    //outColor.xyz = SkyLighting(origin, dirViewSpace.xyz, sunDirection1);
  }
