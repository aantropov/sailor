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

#ifdef USE_STORAGE_BUFFER
layout(std140, set = 1, binding = 0) readonly buffer PerInstanceDataSSBO
{
    PerInstanceData instance[];
} data;
#else
layout(set = 1, binding = 0) uniform PerInstanceDataUBO
{
    PerInstanceData instance;
} data;
#endif

#ifdef CUSTOM_DATA
layout(set=2, binding=0) uniform MaterialData
{
    vec4 color;
} material;
#endif

layout(location=0) in vec3 inPosition;
layout(location=1) in vec2 inTexcoord;
layout(location=2) in vec4 inColor;

layout(location=0) out vec4 fragColor;
layout(location=1) out vec2 fragTexcoord;

void main() 
{
#ifdef USE_STORAGE_BUFFER
    gl_Position = frame.projection * frame.view * data.instance[gl_BaseInstance].model * vec4(inPosition, 1.0);
#else
    gl_Position = frame.projection * frame.view * data.instance.model * vec4(inPosition, 1.0);
#endif

    fragColor = 1 - inColor * gl_Position.z / 1000;

#ifdef CUSTOM_DATA
    fragColor *= material.color;
#endif

    fragTexcoord = inTexcoord;
}
END_CODE,

"glslFragment":
BEGIN_CODE
layout(location=0) in vec4 fragColor;
layout(location=1) in vec2 fragTexcoord;

layout(set=0, binding=1) uniform sampler2D g_defaultSampler;
layout(set=2, binding=1) uniform sampler2D textureSampler;

layout(location = 0) out vec4 outColor;

void main() 
{
    outColor = fragColor * texture(textureSampler, fragTexcoord) * texture(g_defaultSampler, fragTexcoord);
}
END_CODE,

"defines":["CUSTOM_DATA", "USE_STORAGE_BUFFER"]
}