---
includes:
- Shaders/Constants.glsl
- Shaders/Math.glsl
- Shaders/Lighting.glsl

defines:
- CASCADES
- LIGHT_TILES
- AO

glslCommon: |
  #version 460
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

  layout(set=1, binding=1) uniform sampler2D ldrSceneSampler;
  layout(set=1, binding=2) uniform sampler2D linearDepthSampler;

  layout(std430, set = 2, binding = 0) readonly buffer LightDataSSBO
  {  
    LightData instance[];
  } light;
  
  layout(std430, set = 2, binding = 1) readonly buffer CulledLightsSSBO
  {
      uint indices[];
  } culledLights;
  
  layout(std430, set = 2, binding = 2) readonly buffer LightsGridSSBO
  {
      LightsGrid instance[];
  } lightsGrid;

  layout(set=2, binding=9) uniform sampler2D g_aoSampler;
  
  layout(location=DefaultPositionBinding) in vec3 inPosition;
  layout(location=DefaultTexcoordBinding) in vec2 inTexcoord;
  
  layout(location=0) out vec2 fragTexcoord;
  
  layout(set=1, binding=0) uniform PostProcessDataUBO
  {
    vec4 param;    
  } data;
  
  void main() 
  {
      gl_Position = vec4(inPosition, 1);
      fragTexcoord = inTexcoord;
  }

glslFragment: |
  layout(location=0) in vec2 fragTexcoord;
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
   
  const float Epsilon = 0.00001;
  
  layout(set=1, binding=1) uniform sampler2D ldrSceneSampler;
  layout(set=1, binding=2) uniform sampler2D linearDepthSampler;
   
  layout(std430, set = 2, binding = 0) readonly buffer LightDataSSBO
  {  
    LightData instance[];
  } light;
  
  layout(std430, set = 2, binding = 1) readonly buffer CulledLightsSSBO
  {
      uint indices[];
  } culledLights;
  
  layout(std430, set = 2, binding = 2) readonly buffer LightsGridSSBO
  {
      LightsGrid instance[];
  } lightsGrid;
  
  layout(set=2, binding=9) uniform sampler2D g_aoSampler;
  
  void main() 
  {
    outColor = texture(ldrSceneSampler, fragTexcoord);
    
  #if defined(AO)
    outColor = texture(g_aoSampler, fragTexcoord);
  #elif defined(LIGHT_TILES)
    outColor = vec4(texture(linearDepthSampler, fragTexcoord).r / 50000);
  
    const uint tileIndex = GetLightTileIndex(gl_FragCoord.xy, frame.viewportSize);
    const uint gridLength = uint(lightsGrid.instance.length());
    const bool hasLightTile = tileIndex < gridLength;
    const uint offset = hasLightTile ? lightsGrid.instance[tileIndex].offset : 0;
    const uint listLength = uint(culledLights.indices.length());
    const uint availableLights = offset < listLength ? listLength - offset : 0;
    const uint numLights = hasLightTile ? min(
        min(lightsGrid.instance[tileIndex].num, uint(LIGHTS_PER_TILE)),
        availableLights) : 0;
    
    for(int i = 0; i < numLights; i++)
    {
        uint index = culledLights.indices[offset + i];
        if(index == uint(-1))
        {
            break;
        }
        outColor.xyz += vec3(0.05,0.05,0.05);
    }
   #elif defined(CASCADES)
    float linearDepth = texture(linearDepthSampler, fragTexcoord).r;
    int layer = NUM_CSM_CASCADES;
    for (int i = 0; i < NUM_CSM_CASCADES; ++i)
    {
        if (linearDepth < frame.cameraZNearZFar.y * ShadowCascadeLevels[i])
        {
            layer = i;
            break;
        }
    }
    
    vec3 dColor = vec3(1,0,0);
    if(layer == 0)
    {
        dColor = vec3(0,1,0);
    }
    else if(layer == 1)
    {
        dColor = vec3(1,1,0);
    }
    else if(layer == 2)
    {
        dColor = vec3(0,0,1);
    }
    else
        dColor = vec3(0,1,1);
    
    outColor.rgb = mix(outColor.rgb, dColor, 0.5);
   #endif
   

    /**/
  }
