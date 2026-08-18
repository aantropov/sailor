---
includes:
- Shaders/Constants.glsl
- Shaders/Math.glsl

colorAttachments :
- R32_SFLOAT

glslCommon: |
 #version 460
 #extension GL_ARB_separate_shader_objects : enable
 
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
 
glslVertex: |
 layout(location=DefaultPositionBinding) in vec3 inPosition;
 
 void main() 
 {
     gl_Position = vec4(inPosition, 1);
 }

glslFragment: |
 layout(set=1, binding=0) uniform sampler2D depthSampler;
 layout(location=0) out vec4 outColor;
 
 void main() 
 {
    // Preserve framebuffer coordinates exactly. This avoids coupling depth
    // reconstruction to viewport orientation or platform vertex-Y conversion.
    ivec2 depthSize = textureSize(depthSampler, 0);
    ivec2 pixel = clamp(ivec2(gl_FragCoord.xy), ivec2(0), depthSize - ivec2(1));
    float depth = texelFetch(depthSampler, pixel, 0).x;
 #ifdef REVERSE_Z_INF_FAR_PLANE
    float linearDepth = frame.cameraZNearZFar.x / depth;
 #else
    // Reconstruct from the same projection matrix that produced the depth.
    // This remains exact when the camera projection parameters or conventions
    // change and keeps the result in positive view-space distance.
    vec4 viewPosition = frame.invProjection * vec4(0.0, 0.0, depth, 1.0);
    float linearDepth = abs(viewPosition.z / viewPosition.w);
 #endif
    outColor = vec4(linearDepth);
 }
