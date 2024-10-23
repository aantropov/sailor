includes:
- Shaders/Constants.glsl
- Shaders/Math.glsl

glslCommon: |
  #version 450
  #extension GL_ARB_separate_shader_objects : enable
  #extension GL_EXT_shader_atomic_float : enable
glslCompute: |

  struct PerInstanceData
  {
      mat4 model;
      vec4 color;
      vec4 colorOld;
      uint materialInstance;
      uint isCulled;
  };
  
  struct ParticleData
  {
    float m_bIsEnabled, m_size1, m_size2, m_nouse_;
    float m_x1, m_y1, m_z1, m_w1_;
    float m_r1, m_g1, m_b1, m_a1;
    float m_x2, m_y2, m_z2, m_w2_;
    float m_r2, m_g2, m_b2, m_a2;
  };
  
  layout(std430, set = 0, binding = 0) buffer PerInstanceDataSSBO
  {
      PerInstanceData instance[];     
  } data;
  
  layout(std430, set = 0, binding = 1) buffer ParticlesDataSSBO
  {
      ParticleData instance[];
  } particlesData;
  
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
    uint numInstances;
    uint numFrames;
    uint fps;
    uint traceFrames;
    float traceDecay;
  } PushConstants;
  
  mat4 rotationMatrix(vec3 axis, float angle) 
  {
    float cosAngle = cos(angle);
    float sinAngle = sin(angle);
    float oneMinusCos = 1.0 - cosAngle;

    axis = normalize(axis);

    mat4 result = mat4(1.0);
    result[0][0] = cosAngle + axis.x * axis.x * oneMinusCos;
    result[0][1] = axis.x * axis.y * oneMinusCos - axis.z * sinAngle;
    result[0][2] = axis.x * axis.z * oneMinusCos + axis.y * sinAngle;

    result[1][0] = axis.y * axis.x * oneMinusCos + axis.z * sinAngle;
    result[1][1] = cosAngle + axis.y * axis.y * oneMinusCos;
    result[1][2] = axis.y * axis.z * oneMinusCos - axis.x * sinAngle;

    result[2][0] = axis.z * axis.x * oneMinusCos - axis.y * sinAngle;
    result[2][1] = axis.z * axis.y * oneMinusCos + axis.x * sinAngle;
    result[2][2] = cosAngle + axis.z * axis.z * oneMinusCos;

    return result;
  }

  layout(local_size_x = 256) in;
  void main()
  { 
    uint threadCount = 256;
    uint globalIndex = gl_GlobalInvocationID.x;    
    uint instancePerThread = PushConstants.numInstances / threadCount + 1;

    for (uint i = 0; i < instancePerThread; i++)
    {
        uint instanceId = i + globalIndex * instancePerThread;
        
        if(instanceId >= PushConstants.numInstances)
        {
            break;
        }
        
        uint frame = uint(mod(frame.currentTime * PushConstants.fps, float(PushConstants.numFrames)));
        uint current = uint(mod(instanceId, PushConstants.traceFrames));
        float decay = pow(PushConstants.traceDecay, current);
        
        const int segmentation = 1;
        
        ParticleData newFrame = particlesData.instance[max(0, (frame - current)) * PushConstants.numInstances / PushConstants.traceFrames + instanceId / PushConstants.traceFrames];
        ParticleData oldFrame = particlesData.instance[max(0, (frame - current - segmentation)) * PushConstants.numInstances / PushConstants.traceFrames + instanceId / PushConstants.traceFrames];
        /////////
        
        float distanceBetweenPoints = distance(vec3(newFrame.m_x2, newFrame.m_y2, newFrame.m_z2), vec3(newFrame.m_x1, newFrame.m_y1, newFrame.m_z1));
        vec3 direction = normalize(vec3(newFrame.m_x2 - newFrame.m_x1, newFrame.m_y2 - newFrame.m_y1, newFrame.m_z2 - newFrame.m_z1));
        
        vec3 up = vec3(0.0, 0.0, 1.0);
        vec3 axis = cross(up, direction);
        float angle = acos(dot(up, direction));
        
        mat4 rotation = mat4(1.0);
        if (length(axis) > 0.0) 
        {
            rotation = rotationMatrix(axis, angle);
        }
        
        mat4 scale = mat4(1.0);
        scale[2][2] = distanceBetweenPoints;
        scale[0][0] = scale[1][1] = newFrame.m_size2 * decay * 1.5;
        
        data.instance[instanceId].model = mat4(1.0);
        data.instance[instanceId].model[3] = vec4((newFrame.m_x1 + newFrame.m_x2) * 0.5, 
                                                (newFrame.m_y1 + newFrame.m_y2) * 0.5, 
                                                (newFrame.m_z1 + newFrame.m_z2) * 0.5, 1.0);
        
        data.instance[instanceId].model *= rotation;
        data.instance[instanceId].model *= scale;
        data.instance[instanceId].colorOld = vec4(oldFrame.m_r2, oldFrame.m_g2, oldFrame.m_b2, oldFrame.m_a2 * decay);
        data.instance[instanceId].color = vec4(newFrame.m_r2, newFrame.m_g2, newFrame.m_b2, newFrame.m_a2 * decay);
        data.instance[instanceId].isCulled = newFrame.m_bIsEnabled > 0.5f ? 1 : 0;
    }

    barrier();
  }
