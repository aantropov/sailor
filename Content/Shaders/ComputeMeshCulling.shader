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
    uint phase;
    uint enableOcclusion;
  } PushConstants;
  
  struct PerInstanceData
  {
      mat4 model;
      vec4 sphereBounds;
      uint materialInstance;
      uint skeletonOffset;
      uint isCulled;
      uint padding;
      vec4 bakedVolumeScale;
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

  float ConservativeSphereScale(mat4 model)
  {
    vec3 column0 = abs(model[0].xyz);
    vec3 column1 = abs(model[1].xyz);
    vec3 column2 = abs(model[2].xyz);

    float matrixOneNorm = max(max(
      column0.x + column0.y + column0.z,
      column1.x + column1.y + column1.z),
      column2.x + column2.y + column2.z);
    float matrixInfinityNorm = max(max(
      column0.x + column1.x + column2.x,
      column0.y + column1.y + column2.y),
      column0.z + column1.z + column2.z);

    // The spectral norm is bounded by sqrt(||M||1 * ||M||inf). Unlike using
    // only model[0], this remains conservative for non-uniform scale and shear.
    return sqrt(matrixOneNorm * matrixInfinityNorm);
  }

  bool OcclusionCulling(uint instanceIndex)
  {
    ivec2 depthHighZSize = textureSize(depthHighZ, 0);
    
    vec4 sphereBounds = data.instance[instanceIndex].sphereBounds;
    vec4 center = frame.view * (data.instance[instanceIndex].model * vec4(sphereBounds.xyz, 1.0f));
    center.xyz /= center.w;
    center.z *= -1.0f;
    
    float radius = sphereBounds.w * ConservativeSphereScale(data.instance[instanceIndex].model);
    
    vec4 aabb;
    if (ProjectSphere(center.xyz, radius, frame.cameraZNearZFar.x, frame.projection[0][0], frame.projection[1][1], aabb))
    {   
      vec4 clippedAabb = clamp(aabb, vec4(0.0), vec4(1.0));
      float width = max((clippedAabb.z - clippedAabb.x) * depthHighZSize.x, 1.0);
      float height = max((clippedAabb.w - clippedAabb.y) * depthHighZSize.y, 1.0);
    
      // Pick a mip whose texel covers the whole projected sphere and clamp it
      // to the actual pyramid. A lower mip can miss an occluder discontinuity.
      float maxLevel = float(max(textureQueryLevels(depthHighZ) - 1, 0));
      float level = clamp(ceil(log2(max(width, height))), 0.0, maxLevel);
    
      // The projected rectangle can cross a mip texel boundary even when its
      // dimensions fit one texel. Reduce every touched texel so a single center
      // sample cannot reject geometry at an occluder edge.
      int mipLevel = int(level);
      ivec2 mipSize = textureSize(depthHighZ, mipLevel);
      ivec2 texelBegin = clamp(
        ivec2(floor(clippedAabb.xy * vec2(mipSize))),
        ivec2(0),
        mipSize - ivec2(1));
      ivec2 texelEnd = clamp(
        ivec2(ceil(clippedAabb.zw * vec2(mipSize))) - ivec2(1),
        texelBegin,
        mipSize - ivec2(1));

      float depth = texelFetch(depthHighZ, texelBegin, mipLevel).x;
      for (int y = texelBegin.y; y <= texelEnd.y; ++y)
      {
        for (int x = texelBegin.x; x <= texelEnd.x; ++x)
        {
          depth = min(depth, texelFetch(depthHighZ, ivec2(x, y), mipLevel).x);
        }
      }
    
      float depthSphere = frame.cameraZNearZFar.x / (center.z - radius);
    
      // Reverse-Z: the sphere is occluded only when its nearest depth is still
      // behind the farthest occluder depth stored for the selected footprint.
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

    float radius = sphereBounds.w * ConservativeSphereScale(data.instance[instanceIndex].model);

    bool bIsCulled = !SphereFrustumOverlaps(center.xyz, radius, frustum, frame.cameraZNearZFar.y, frame.cameraZNearZFar.x);
  
    return bIsCulled;
  }
  
  layout(local_size_x = GPU_CULLING_GROUP_SIZE) in;
  void main()
  {
    uint globalIndex = gl_GlobalInvocationID.x;

    if (PushConstants.phase == 0)
    {
      // Step 1: Calculate View Frustum
      if (gl_LocalInvocationIndex == 0)
      {
        frustum = CreateViewFrustum(frame.viewportSize, frame.invProjection);
      }

      barrier();

      // Step 2: Perform culling. Compaction is a separate dispatch because a
      // GLSL workgroup barrier cannot synchronize multiple workgroups.
      if (globalIndex < PushConstants.numInstances)
      {
        uint instanceId = PushConstants.firstInstanceIndex + globalIndex;

        bool bIsCulled = FrustumCulling(instanceId);
        #ifdef OCCLUSION_CULLING
          if (!bIsCulled && PushConstants.enableOcclusion != 0u)
          {
            bIsCulled = OcclusionCulling(instanceId);
          }
        #endif
        
        data.instance[instanceId].isCulled = bIsCulled ? 1u : 0u;
      }
    }
    else if (globalIndex < PushConstants.numBatches)
    {
        // Step 3: Compact visible instances and update the matching indirect
        // command after the host-side shader-write barrier.
        uint batchId = globalIndex;
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
