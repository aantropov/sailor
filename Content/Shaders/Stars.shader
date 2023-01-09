---
includes:
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
  	float currentTime;
  	float deltaTime;
  } frame;
  
  layout(push_constant) uniform Constants
  {
  	mat4 model;
  } PushConstants;
  
  layout(location=0) in vec3 inPosition;
  layout(location=3) in vec4 inColor;
  
  layout(location=0) out vec4 fragColor;
  layout(location=1) out vec4 fragWPosition;
  layout(location=2) out vec4 fragUV;
  
  void main() 
  {
    gl_PointSize = 3;
  	gl_Position = frame.projection * frame.view * PushConstants.model * vec4(inPosition, 1.0);

    vec3 pos = gl_Position.xyz/gl_Position.w;
    
    //pos.y = 1.0f - pos.y;
    fragUV.xy = (pos.xy + 1) * 0.5f;
    fragColor = inColor;
  }

glslFragment: |
  layout(location = 0) in vec4 fragColor;
  layout(location = 1) in vec4 worldPos;
  layout(location = 2) in vec4 fragUV;
  
  layout(location = 0) out vec4 outColor;
  
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
  
  layout(set = 1, binding = 0) uniform PostProcessDataUBO
  {
    vec4 lightDirection;
  } data;
  
  layout(set = 1, binding = 6) uniform sampler2D cloudsSampler;

  const float R = 6371000.0f; // Earth radius in m
  
  void main()
  { 
    const vec3 origin = vec3(0, R + 1000, 0) + frame.cameraPosition.xyz;
    vec2 viewportPos = gl_FragCoord.xy / vec2(frame.viewportSize);
    viewportPos.y = 1.0f - viewportPos.y;
    
    vec4 dirWorldSpace = vec4(0);
    
    dirWorldSpace.xyz = ScreenToView(viewportPos, 1.0f, frame.invProjection).xyz;
    dirWorldSpace.z *= -1;
    dirWorldSpace = normalize(inverse(frame.view) * dirWorldSpace);

    outColor = vec4(0);

    float clouds = texture(cloudsSampler, viewportPos).a;

    vec2 intersection = RaySphereIntersect(origin, dirWorldSpace.xyz, vec3(0), R);
    if(max(intersection.x, intersection.y) < 0.0f)
    {
        const float mask = clamp(1 - 1000 * clamp(length(viewportPos.xy - fragUV.xy), 0.0, 1), 0, 1);
        const float artisticTune = pow(max(0, 0.3 + dot(data.lightDirection.xyz, vec3(0, 1, 0))), 0.9);
        
        outColor = mask * fragColor;
        outColor.a = clamp(artisticTune * 0.25, 0, 1);
        
        outColor.xyz *= outColor.a * (1.0f - clouds);
    }
  }