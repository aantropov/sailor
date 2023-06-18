---
includes:
- Shaders/Constants.glsl
- Shaders/Math.glsl
- Shaders/Lighting.glsl

defines:
- CASCADES
- LIGHT_TILES

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
  
  void main() 
  {
    outColor = texture(ldrSceneSampler, fragTexcoord);
  #if defined(LIGHT_TILES)
    outColor = texture(linearDepthSampler, fragTexcoord).r / 50000;
  
    vec2 numTiles = floor(frame.viewportSize / LIGHTS_CULLING_TILE_SIZE);
    vec2 screenUv = vec2(gl_FragCoord.x, frame.viewportSize.y - gl_FragCoord.y);
    ivec2 tileId = ivec2(screenUv) / LIGHTS_CULLING_TILE_SIZE;
    
    ivec2 mod = ivec2(frame.viewportSize.x % LIGHTS_CULLING_TILE_SIZE, frame.viewportSize.y % LIGHTS_CULLING_TILE_SIZE);
    ivec2 padding = ivec2(min(1, mod.x), min(1, mod.y));
    
    uint tileIndex = uint(tileId.y * (numTiles.x + padding.x) + tileId.x);
  
    const uint offset = lightsGrid.instance[tileIndex].offset;
    const uint numLights = lightsGrid.instance[tileIndex].num;
    
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
