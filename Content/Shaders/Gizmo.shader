{
"includes": [] ,

"glslCommon" :
BEGIN_CODE
#version 460
#extension GL_ARB_separate_shader_objects : enable
END_CODE,

"glslVertex":
BEGIN_CODE

layout(set = 0, binding = 0) uniform FrameData
{
    mat4 view;
    mat4 projection;
    float currentTime;
    float deltaTime;
} frame;

struct PerInstanceData
{
    mat4 model;
};

layout(std140, set = 1, binding = 0) readonly buffer PerInstanceDataSSBO
{
    PerInstanceData instance[];
} data;

layout(location=0) in vec3 inPosition;
layout(location=3) in vec4 inColor;
layout(location=0) out vec4 fragColor;

void main() 
{
    gl_Position = frame.projection * frame.view * vec4(inPosition, 1.0);
    fragColor = inColor;
}
END_CODE,

"glslFragment":
BEGIN_CODE
layout(location=0) in vec4 fragColor;
layout(location = 0) out vec4 outColor;

layout(set=0, binding=1) uniform sampler2D g_defaultSampler;

void main() 
{
    outColor = fragColor;
}
END_CODE,

"defines":[]
}