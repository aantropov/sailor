includes:
- Shaders/Constants.glsl
- Shaders/Math.glsl

defines: []

glslCommon: |
  #version 460
  #extension GL_ARB_separate_shader_objects : enable

glslCompute: |
  layout(local_size_x = 16, local_size_y = 16, local_size_z = 1) in;

  layout(set = 0, binding = 0) uniform sampler3D u_densityVolume;
  layout(set = 0, binding = 1, rgba16f) writeonly uniform image2D u_output_image;

  layout(set = 1, binding = 0) uniform FrameData
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

  layout(push_constant) uniform Constants
  {
      float stepSize;
      vec3 fogColor;
  } PushConstants;

  void main()
  {
      ivec2 pixel = ivec2(gl_GlobalInvocationID.xy);
      ivec2 dim = imageSize(u_output_image);
      if (pixel.x >= dim.x || pixel.y >= dim.y) return;

      vec2 uv = (vec2(pixel) + vec2(0.5)) / vec2(dim);
      vec4 ndc = vec4(uv * 2.0 - 1.0, 1.0, 1.0);
      vec4 viewPos = frame.invProjection * ndc;
      viewPos.xyz /= viewPos.w;
      vec3 dir = normalize(viewPos.xyz);
      vec3 origin = frame.cameraPosition.xyz;

      float t = frame.cameraZNearZFar.x;
      float tFar = frame.cameraZNearZFar.y;
      float trans = 1.0;

      for (; t < tFar && trans > 0.01; t += PushConstants.stepSize)
      {
          vec3 samplePos = origin + dir * t;
          float density = texture(u_densityVolume, samplePos).r;
          float atten = exp(-density * PushConstants.stepSize);
          trans *= atten;
      }

      vec3 color = mix(PushConstants.fogColor, vec3(0.0), trans);
      imageStore(u_output_image, pixel, vec4(color, 1.0 - trans));
  }
