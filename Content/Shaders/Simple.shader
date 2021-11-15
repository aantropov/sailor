{
"includes":[],

"glslCommon":
BEGIN_CODE
#version 450
#extension GL_ARB_separate_shader_objects : enable
END_CODE,

"glslVertex":
BEGIN_CODE

layout(binding = 0) uniform UBOTransform 
{
    mat4 model;
    mat4 view;
    mat4 projection;
} transform;

#ifdef CUSTOM_DATA
layout(binding = 1) uniform CustomData
{
    float color;
} custom;
#endif

layout(location = 0) in vec3 inPosition;
layout(location = 1) in vec2 inTexcoord;
layout(location = 2) in vec4 inColor;

layout(location = 0) out vec4 fragColor;
layout(location = 1) out vec2 fragTexcoord;

void main() 
{
    gl_Position = transform.projection * transform.view * transform.model * vec4(inPosition, 1.0);
    fragColor = 1 - inColor * gl_Position.z / 1000;
    fragTexcoord = inTexcoord;
}
END_CODE,

"glslFragment":
BEGIN_CODE
layout(location = 0) in vec4 fragColor;
layout(location = 1) in vec2 fragTexcoord;

layout(binding = 1) uniform sampler2D textureSampler;

layout(location = 0) out vec4 outColor;

void main() 
{
    outColor = fragColor * texture(textureSampler, fragTexcoord);
}
END_CODE,

"defines":["CUSTOM_DATA"]
}