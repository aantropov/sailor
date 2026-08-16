---
includes:
- Shaders/Constants.glsl
- Shaders/Math.glsl

defines: ~

glslCommon: |
  #version 450
  #extension GL_ARB_separate_shader_objects : enable

glslVertex: |
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
  
  layout(set = 0, binding = 1) uniform PreviousFrameData
  {
      mat4 view;
      mat4 projection;
      mat4 invProjection;
      vec4 cameraPosition;
      ivec2 viewportSize;
      vec2 cameraZNearZFar;
      float currentTime;
      float deltaTime;
  } previousFrame;
  
  layout(push_constant) uniform Constants
  {
  	mat4 model;
  } PushConstants;
  
  layout(location=DefaultPositionBinding) in vec3 inPosition;
  layout(location=DefaultColorBinding) in vec4 inColor;
  
  layout(location=0) out vec4 fragColor;
  layout(location=1) out vec4 fragWPosition;
  layout(location=2) out vec4 fragUV;
  
  void main() 
  {
    gl_PointSize = 1;
  	gl_Position = frame.projection * frame.view * PushConstants.model * vec4(inPosition, 1.0);

    // Stars are directions on the celestial sphere. Keep them on the reverse-Z
    // far plane so changing the camera far distance cannot clip the catalogue.
    gl_Position.z = 0.0f;

    vec3 pos = gl_Position.xyz/gl_Position.w;
    
    //pos.y = 1.0f - pos.y;
    fragUV.xy = (pos.xy + 1) * 0.5f;
    fragColor = inColor;
  }

glslFragment: |
  layout(location=0) in vec4 fragColor;
  layout(location=1) in vec4 worldPos;
  layout(location=2) in vec4 fragUV;
  
  layout(location=0) out vec4 outColor;
  
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
  
  layout(set = 0, binding = 1) uniform PreviousFrameData
  {
      mat4 view;
      mat4 projection;
      mat4 invProjection;
      vec4 cameraPosition;
      ivec2 viewportSize;
      vec2 cameraZNearZFar;
      float currentTime;
      float deltaTime;
  } previousFrame;
  
  layout(set=1, binding=0) uniform PostProcessDataUBO
  {
    vec4 lightDirection;
  } data;
  
  layout(set=1, binding=6) uniform sampler2D cloudsSampler;

  const float earthRadius = 6371000.0f;

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

  float PlanetHeight(vec3 position)
  {
    const float verticalRadius = earthRadius + position.y;
    const float horizontalDistanceSquared = dot(position.xz, position.xz);
    const float radialDistance = sqrt(
      verticalRadius * verticalRadius + horizontalDistanceSquared);
    return position.y + horizontalDistanceSquared /
      max(radialDistance + verticalRadius, 1.0f);
  }

  float RadialMotion(vec3 position, vec3 direction)
  {
    return dot(direction, vec3(
      position.x,
      earthRadius + position.y,
      position.z));
  }

  bool RayCanReachSky(vec3 origin, vec3 direction)
  {
    const float height = PlanetHeight(origin);
    const float boundaryTolerance = 0.001f;
    if(height < -boundaryTolerance)
    {
      if(RadialMotion(origin, direction) < 0.0f)
      {
        return false;
      }

      const vec2 intersections =
        RaySphereAtAltitude(origin, direction, 0.0f);
      return max(intersections.x, intersections.y) >= 0.0f;
    }

    if(abs(height) <= boundaryTolerance)
    {
      return RadialMotion(origin, direction) >= 0.0f;
    }

    const vec2 intersections =
      RaySphereAtAltitude(origin, direction, 0.0f);
    const bool entersPlanet =
      intersections.x > 0.0f || intersections.y > 0.0f;
    return !entersPlanet;
  }
  
  void main()
  { 
    const vec3 origin = frame.cameraPosition.xyz;
    vec2 viewportPos = gl_FragCoord.xy / vec2(frame.viewportSize);
    viewportPos.y = 1.0f - viewportPos.y;
    
    vec4 dirWorldSpace = vec4(0);
    
    dirWorldSpace.xyz = ScreenSpaceToViewSpace(viewportPos, 1.0f, frame.invProjection).xyz;
    dirWorldSpace.z *= -1;
    dirWorldSpace = normalize(inverse(frame.view) * dirWorldSpace);

    outColor = vec4(0);

    float clouds = texture(cloudsSampler, viewportPos).a;

    if(RayCanReachSky(origin, dirWorldSpace.xyz))
    {
        const float mask = clamp(1 - 1000 * clamp(length(viewportPos.xy - fragUV.xy), 0.0, 1), 0, 1);
        const vec3 dirToSun = normalize(-data.lightDirection.xyz);
        const vec3 planetUp = normalize(vec3(
          origin.x,
          earthRadius + origin.y,
          origin.z));
        const float sunElevation = asin(clamp(
          dot(dirToSun, planetUp),
          -1.0f,
          1.0f));
        const float starVisibility = 1.0f - smoothstep(
          radians(-18.0f),
          radians(-6.0f),
          sunElevation);

        outColor = mask * fragColor;
        outColor.xyz *= starVisibility * (1.0f - clouds) * 0.15f;
        outColor.a = starVisibility;
    }
  }
