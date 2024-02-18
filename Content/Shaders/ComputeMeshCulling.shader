includes:
- Shaders/Constants.glsl
- Shaders/Math.glsl
defines:
- OCCLUSION_CULLING
glslCommon: |
  #version 450
  #extension GL_ARB_separate_shader_objects : enable
  #extension GL_EXT_shader_atomic_float : enable
glslCompute: |
  // Inspired by https://vkguide.dev/docs/gpudriven/compute_culling/
  
  layout(push_constant) uniform Constants
  {
    uint numBatches;
    uint numInstances;
    uint firstInstanceIndex;
  } PushConstants;
  
  struct PerInstanceData
  {
      mat4 model;
      vec4 sphereBounds;
      uint materialInstance;
      uint isCulled;
  };
  
  struct DrawIndexedIndirectData
  {
    uint indexCount;
    uint instanceCount;
    uint firstIndex;
    int vertexOffset;
    uint firstInstance;
  };
  
  layout(set = 0, binding = 0) uniform sampler2D depthHighZ;
  layout(std430, set = 1, binding = 0) buffer PerInstanceDataSSBO
  {
      PerInstanceData instance[];
  } data;
  
  layout(std430, set = 2, binding = 0) buffer DrawIndexedIndirectBuffer
  {
    DrawIndexedIndirectData batches[];
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

  bool OcclusionCulling(uint instanceIndex)
  {
    ivec2 depthHighZSize = textureSize(depthHighZ, 0);
    
    vec4 sphereBounds = data.instance[instanceIndex].sphereBounds;
    vec4 center = frame.view * (data.instance[instanceIndex].model * vec4(sphereBounds.xyz, 1.0f));
    center.xyz /= center.w;
    center.z *= -1.0f;
    
    float lossyScale = length(data.instance[instanceIndex].model[0].xyz);
    float radius = sphereBounds.w * lossyScale;
    
    vec4 aabb;
    if (ProjectSphere(center.xyz, radius, frame.cameraZNearZFar.x, frame.projection[0][0], frame.projection[1][1], aabb))
    {   
      float width = (aabb.z - aabb.x) * depthHighZSize.x;
      float height = (aabb.w - aabb.y) * depthHighZSize.y;
    
      //find the mipmap level that will match the screen size of the sphere
      float level = floor(log2(max(width, height)));
    
      //sample the depth pyramid at that specific level
      vec2 uv = (aabb.xy + aabb.zw) * 0.5;
      float depth = textureLod(depthHighZ, uv, level).x;
    
      float depthSphere = frame.cameraZNearZFar.x / (center.z - radius);
    
      //if the depth of the sphere is in front of the depth pyramid value, then the object is visible
      return depthSphere < depth;
    }
    
    return false;
  }
  
  bool FrustumCulling(uint instanceIndex)
  {
    // Calculations are in view space
    vec4 sphereBounds = data.instance[instanceIndex].sphereBounds;
    vec4 center = frame.view * (data.instance[instanceIndex].model * vec4(sphereBounds.xyz, 1.0f));
    center.xyz /= center.w;
    center.z *= -1.0f;

    float lossyScale = length(data.instance[instanceIndex].model[0].xyz);
    float radius = sphereBounds.w * lossyScale;

    bool bIsCulled = !SphereFrustumOverlaps(center.xyz, radius, frustum, frame.cameraZNearZFar.y, frame.cameraZNearZFar.x);
  
    return bIsCulled;
  }
  
  layout(local_size_x = GPU_CULLING_GROUP_SIZE) in;
  void main()
  { 
    // Step 1: Calculate View Frustum
    if (gl_LocalInvocationIndex == 0)
    {
        frustum = CreateViewFrustum(frame.viewportSize, frame.invProjection);
    }
    
    barrier();
   
    // Step 2: Perform culling (split instances by threads)
    uint globalIndex = gl_GlobalInvocationID.x;
    uint threadCount = GPU_CULLING_GROUP_SIZE;
    uint instancePerThread = (PushConstants.numInstances + threadCount - 1) / threadCount;
        
    for (uint i = 0; i < instancePerThread; i++)
    {
        uint instanceId = PushConstants.firstInstanceIndex + i + globalIndex * instancePerThread;
        
        if(instanceId >= PushConstants.numInstances)
        {
            break;
        }
        
        #ifdef OCCLUSION_CULLING
          bool bIsCulled = FrustumCulling(instanceId) || OcclusionCulling(instanceId);
        #else
          bool bIsCulled = FrustumCulling(instanceId);
        #endif
        
        data.instance[instanceId].isCulled = bIsCulled ? 1 : 0;
    }

    barrier();
    
    // Step3: Remove empty draw calls (split batches per threads)
    uint batchPerThread = (PushConstants.numBatches + threadCount - 1) / threadCount;
    
    for (uint j = 0; j < batchPerThread; j++)
    {
        uint batchId = j + globalIndex * batchPerThread;
        
        if(batchId >= PushConstants.numBatches)
        {
            break;
        }
        
        uint readIndex = drawIndexedIndirect.batches[batchId].firstInstance;
        uint writeIndex = readIndex;
        
        for (uint i = 0; i < drawIndexedIndirect.batches[batchId].instanceCount; i++)
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

        drawIndexedIndirect.batches[batchId].instanceCount = writeIndex - drawIndexedIndirect.batches[batchId].firstInstance;
    }    
  }
