includes:
- Shaders/Constants.glsl
- Shaders/Math.glsl
defines: []
glslCommon: |
  #version 450
  #extension GL_ARB_separate_shader_objects : enable
  #extension GL_EXT_shader_atomic_float : enable
glslCompute: |
  // Inspired by https://vkguide.dev/docs/gpudriven/compute_culling/  
  
  struct PerInstanceData
  {
      mat4 model;
      vec4 sphereBounds;
      uint materialInstance;
      uint isCulled;
  };
  
  layout(set = 0, binding = 0) uniform sampler2D depthHighZ;
  layout(set = 1, binding = 0) buffer PerInstanceDataSSBO
  {
      PerInstanceData instance[];
  } data;
  
  layout(std430, set = 2, binding = 0) buffer DrawIndexedIndirectBuffer
  {
    uint indexCount;
    uint instanceCount;
    uint firstIndex;
    int vertexOffset;
    uint firstInstance;
  } drawIndexedIndirect;
  
  layout(set = 3, binding = 0) uniform FrameData
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
  
  shared ViewFrustum frustum;
  
  #define GROUP_SIZE 16
  
  bool FrustumCulling(uint instanceIndex)
  {
    // Calculations are in view space
    vec4 sphereBounds = data.instance[instanceIndex].sphereBounds;
    vec3 center = (frame.view * (data.instance[instanceIndex].model * vec4(sphereBounds.xyz, 1.0f))).xyz;
    
    float radius = sphereBounds.w * max(max(data.instance[instanceIndex].model[0][0], data.instance[instanceIndex].model[1][1]), data.instance[instanceIndex].model[2][2]);
  
    bool bIsVisible = SphereFrustumOverlaps(center.xyz, radius, frustum, frame.cameraZNearZFar.x, frame.cameraZNearZFar.y);
  
  	return bIsVisible;
  }
  
  layout(local_size_x = GROUP_SIZE, local_size_y = GROUP_SIZE) in;
  void main()
  { 
    // Step 1: Calculate View Frustum
    if (gl_LocalInvocationIndex == 0)
  	{
        frustum = CreateViewFrustum(frame.viewportSize, frame.invProjection);		
  	}
  	
  	barrier();
   
   // Step 2: Perform culling
    uint globalIndex = gl_GlobalInvocationID.x + gl_GlobalInvocationID.y * GROUP_SIZE;    
    uint threadCount = GROUP_SIZE * GROUP_SIZE;    
  	uint instancePerThread = (drawIndexedIndirect.instanceCount + threadCount - 1) / threadCount;
        
    for (uint i = 0; i < instancePerThread; i++)
    {
        uint instanceId = drawIndexedIndirect.firstInstance + i + globalIndex * instancePerThread;
        
        if(instanceId >= drawIndexedIndirect.firstInstance + drawIndexedIndirect.instanceCount)
        {
            break;
        }
        
        bool bIsInView = !FrustumCulling(instanceId);
        data.instance[instanceId].isCulled = bIsInView ? 0 : 1;
    }

    barrier();
    
    // Step3: Remove empty draw calls
    if (gl_LocalInvocationIndex == 0)
    {
        uint readIndex = drawIndexedIndirect.firstInstance;
        uint writeIndex = readIndex;

        for (uint i = 0; i < drawIndexedIndirect.instanceCount; i++)
        {
            if (data.instance[readIndex].isCulled == 0)
            {
                if (readIndex != writeIndex)
                {
                    data.instance[writeIndex] = data.instance[readIndex];
                }
                writeIndex++;
            }
            readIndex++;
        }

        drawIndexedIndirect.instanceCount = writeIndex - drawIndexedIndirect.firstInstance;
    }    
  }
