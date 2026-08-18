includes:
- Shaders/Constants.glsl
- Shaders/Math.glsl
- Shaders/Lighting.glsl

defines: []
glslCommon: |
  #version 460
  #extension GL_ARB_separate_shader_objects : enable
glslCompute: |
  layout(local_size_x = LIGHTS_CULLING_TILE_SIZE, local_size_y = LIGHTS_CULLING_TILE_SIZE, local_size_z = 1) in;
  layout(push_constant) uniform Constants
  {
    mat4 invViewProjection;
    ivec2 viewportSize;
    ivec2 numTiles;
    int lightsNum;
  } PushConstants;

  layout(std430, set = 0, binding = 0) readonly buffer LightDataSSBO
  {
      LightData instance[];
  } light;

  layout(std430, set = 1, binding = 0) buffer CulledLightsSSBO
  {
      uint indices[];
  } culledLights;

  layout(std430, set = 1, binding = 1) writeonly buffer LightsGridSSBO
  {
      LightsGrid instance[];
  } lightsGrid;

  layout(set = 1, binding = 2) uniform sampler2D linearDepth;

  layout(set = 2, binding = 0) uniform FrameData
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

  shared ViewFrustum tileFrustum;
  shared int lightCountForTile;
  shared uint minDepthInt;
  shared uint maxDepthInt;

  ViewFrustum CreateTileFrustum(ivec2 tileId)
  {
      const vec2 tileMin = vec2(tileId * LIGHTS_CULLING_TILE_SIZE);
      const vec2 tileMax = min(
          vec2((tileId + ivec2(1)) * LIGHTS_CULLING_TILE_SIZE),
          vec2(PushConstants.viewportSize));
      const vec2 tileCenter = (tileMin + tileMax) * 0.5f;
      const vec3 eyePosition = vec3(0.0f);
      vec3 corners[4];

      corners[0] = ScreenSpaceToViewSpace(
          vec4(tileMin, NdcNearPlane, 1.0f),
          PushConstants.viewportSize,
          frame.invProjection).xyz;
      corners[1] = ScreenSpaceToViewSpace(
          vec4(tileMax.x, tileMin.y, NdcNearPlane, 1.0f),
          PushConstants.viewportSize,
          frame.invProjection).xyz;
      corners[2] = ScreenSpaceToViewSpace(
          vec4(tileMin.x, tileMax.y, NdcNearPlane, 1.0f),
          PushConstants.viewportSize,
          frame.invProjection).xyz;
      corners[3] = ScreenSpaceToViewSpace(
          vec4(tileMax, NdcNearPlane, 1.0f),
          PushConstants.viewportSize,
          frame.invProjection).xyz;

      ViewFrustum result;
      result.planes[0] = ComputePlane(eyePosition, corners[2], corners[0]);
      result.planes[1] = ComputePlane(eyePosition, corners[1], corners[3]);
      result.planes[2] = ComputePlane(eyePosition, corners[0], corners[1]);
      result.planes[3] = ComputePlane(eyePosition, corners[3], corners[2]);
      result.center = ScreenSpaceToViewSpace(
          vec4(tileCenter, NdcNearPlane, 1.0f),
          PushConstants.viewportSize,
          frame.invProjection).xy;

      return result;
  }

  bool SphereTileOverlaps(vec3 lightPosition, float radius,
      ViewFrustum frustum, float zNear, float zFar)
  {
      if(lightPosition.z - radius > zFar || lightPosition.z + radius < zNear)
      {
          return false;
      }

      for(int i = 0; i < 4; ++i)
      {
          if(dot(frustum.planes[i].xyz, lightPosition) -
              frustum.planes[i].w < -radius)
          {
              return false;
          }
      }

      return true;
  }

  void main()
  {
      ivec2 location = ivec2(gl_GlobalInvocationID.xy);
      ivec2 tileId = ivec2(gl_WorkGroupID.xy);
      ivec2 tileNumber = ivec2(gl_NumWorkGroups.xy);
      uint tileIndex = tileId.y * tileNumber.x + tileId.x;

      if (gl_LocalInvocationIndex == 0)
      {
          maxDepthInt = 0;
          minDepthInt = 0xFFFFFFFF;
          lightCountForTile = 0;
      }

      barrier();

      // Read the depth-prepass pixel that belongs to this framebuffer tile and
      // reconstruct positive view-space distance without another viewport pass.
      const bool isInsideViewport = all(lessThan(location, PushConstants.viewportSize));
      if (isInsideViewport)
      {
          const float viewDepth = texelFetch(linearDepth, location, 0).x;

          // Positive IEEE floats preserve ordering when reduced through uint atomics.
          const uint depthInt = floatBitsToUint(viewDepth);
          atomicMax(maxDepthInt, depthInt);
          atomicMin(minDepthInt, depthInt);
      }

      barrier();

      if(gl_LocalInvocationIndex == 0)
      {
          tileFrustum = CreateTileFrustum(tileId);
      }

      barrier();

      // Step 3: Cull lights.
      // Parallelize the threads against the lights now.
      // Can handle 256 simultaniously. Anymore lights than that and additional passes are performed
      uint threadCount = LIGHTS_CULLING_TILE_SIZE * LIGHTS_CULLING_TILE_SIZE;
      uint passCount = (PushConstants.lightsNum + threadCount - 1) / threadCount;

      for (uint i = 0; i < passCount; i++)
      {
          // Get the lightIndex to test for this thread / pass. If the index is >= light count, then this thread can stop testing lights
          uint lightIndex = i * threadCount + gl_LocalInvocationIndex;
          if (lightIndex >= PushConstants.lightsNum)
          {
              break;
          }

          if(light.instance[lightIndex].type == INVALID_LIGHT_TYPE)
          {
              continue;
          }

          // Directional light
          if(light.instance[lightIndex].type == 0)
          {
              uint offset = atomicAdd(lightCountForTile, 1);
              if(offset < LIGHTS_PER_TILE)
              {
                  culledLights.indices[
                      tileIndex * LIGHTS_PER_TILE + offset] =
                      lightIndex;
              }
              continue;
          }

          const float radius = light.instance[lightIndex].bounds.x;
          vec4 lightPosViewSpace = frame.view * vec4(light.instance[lightIndex].worldPosition, 1);
          lightPosViewSpace /= lightPosViewSpace.w;

          // Reverse Z
          lightPosViewSpace.z *= -1;

          const float zNear = uintBitsToFloat(minDepthInt);
          const float zFar = uintBitsToFloat(maxDepthInt);

          // Cull against the tile's view frustum and the exact depth interval
          // occupied by this tile.
          if (SphereTileOverlaps(
              lightPosViewSpace.xyz, radius, tileFrustum, zNear, zFar))
          {
              // Add index to the shared array of visible indices
              uint offset = atomicAdd(lightCountForTile, 1);
              if(offset < LIGHTS_PER_TILE)
              {
                  culledLights.indices[
                      tileIndex * LIGHTS_PER_TILE + offset] =
                      lightIndex;
              }
          }
      }
      barrier();

      if(gl_LocalInvocationIndex == 0)
      {
          // Fill lightsGrid
          const uint uncappedNumLights = uint(lightCountForTile);
          const uint numLights = min(uncappedNumLights, uint(LIGHTS_PER_TILE));
          const uint offset = tileIndex * LIGHTS_PER_TILE;

          const bool lightsOverflow = uncappedNumLights > LIGHTS_PER_TILE;
          lightsGrid.instance[tileIndex].num = lightsOverflow ?
              (uint(PushConstants.lightsNum) | LIGHT_TILE_OVERFLOW_BIT) :
              numLights;
          lightsGrid.instance[tileIndex].offset = offset;
      }
  }
