---
includes:
- Shaders/Math.glsl

defines:
- ALPHA_CUTOUT
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
  
  const vec3 positions[6]       = vec3[6]( vec3(-0.5,-0.5,0), vec3(0.5,-0.5,0), vec3(0.5, 0.5, 0), vec3(0.5, 0.5, 0), vec3(-0.5,0.5,0), vec3(-0.5,-0.5,0) );
  const vec2 uvs[6]             = vec2[6]( vec2(0.0, 1.0),    vec2(1.0, 1.0),   vec2(1.0, 0.0), vec2(1.0, 0.0), vec2(0.0, 0.0), vec2(0.0, 1.0) );

  const float _35OVER13PI = 0.85698815511020565414014334123662;
  
  void main() 
  {
    gl_PointSize = 3;
  	gl_Position = frame.projection * frame.view * PushConstants.model * vec4(inPosition, 1.0);
    
    fragWPosition = gl_Position;
    fragWPosition /= fragWPosition.w;
    
    // Calculate color based on magnitude
    // Following paper "Single Pass Rendering of Day and Night Sky Phenomena"
    float m = gl_Position.w;
    float m_a = 7.0f;       // Average apparent magnitude

    float delta_m = pow(2.512, m_a - m);
    float size = 0.001f;
    
    // Magic from the papers. Investigate the WHY of this.
    float i_t = delta_m * _35OVER13PI;
    i_t *= 4e-7 / (size * size);  // resolution correlated 
    i_t = min(1.167, i_t);  // volume of smoothstep (V_T)

    float i_g = pow(2.512, m_a - (m + 0.167)) - 1;
    vec3 v_t = vec3(i_t);

    // v_k
    const float glare_scale = 2.0f;
    const float v_k = max(size, sqrt(i_g) * 2e-2 * glare_scale);

    // TODO: Scattering and Scintillation
    //v_t -= E_ext;
    fragUV.w = v_k / size;

    //
    fragColor.xyz = mix( inColor.xyz, vec3( 0.66, 0.78, 1.00 ), 0.66 );
    fragColor.xyz *= v_t;
    fragColor.xyz = max(vec3(0.0), fragColor.xyz);
    fragColor.w = 1.0f;
    
    fragColor = inColor;
    
    const uint vertex_index = gl_VertexIndex % 6;

    fragUV.xy = positions[vertex_index].xy * vec2(-1, 1);
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
    
    vec2 intersection = RaySphereIntersect(origin, dirWorldSpace.xyz, vec3(0), R);
    if(max(intersection.x, intersection.y) < 0.0f)
    {
        const float artisticTune = pow(max(0, 0.1 + dot(data.lightDirection.xyz, vec3(0, 1, 0))), 0.9);
        
        outColor = fragColor;
        outColor.a = artisticTune * 0.25;
    }
  }