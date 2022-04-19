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

#ifdef USE_UNIFORM_BUFFER
layout(set = 1, binding = 0) uniform PerInstanceDataUBO
{
    PerInstanceData instance;
} data;
#else
layout(std140, set = 1, binding = 0) readonly buffer PerInstanceDataSSBO
{
    PerInstanceData instance[];
} data;
#endif

#ifdef CUSTOM_DATA
layout(set=2, binding=0) uniform MaterialData
{
    vec4 color;
} material;
#endif

layout(location=0) in vec3 inPosition;
layout(location=1) in vec3 inNormal;
layout(location=2) in vec2 inTexcoord;
layout(location=3) in vec4 inColor;

layout(location=0) out vec4 fragColor;
layout(location=1) out vec2 fragTexcoord;
layout(location=2) out vec3 fragNormal;

void main() 
{
#ifdef USE_UNIFORM_BUFFER
    gl_Position = frame.projection * frame.view * data.instance.model * vec4(inPosition, 1.0);
    vec4 worldNormal = data.instance.model * vec4(inNormal, 1.0);
#else
    gl_Position = frame.projection * frame.view * data.instance[gl_InstanceIndex].model * vec4(inPosition, 1.0);
    vec4 worldNormal = data.instance[gl_InstanceIndex].model * vec4(inNormal, 1.0);
#endif

    fragColor = 1 - inColor * gl_Position.z / 3000;

#ifdef CUSTOM_DATA
    fragColor *= material.color;
#endif

	fragNormal = worldNormal.xyz;
    fragTexcoord = inTexcoord;
}
END_CODE,

"glslFragment":
BEGIN_CODE
layout(location=0) in vec4 fragColor;
layout(location=1) in vec2 fragTexcoord;
layout(location=2) in vec3 fragNormal;
layout(set=0, binding=1) uniform sampler2D g_defaultSampler;

#ifndef NO_DIFFUSE
layout(set=2, binding=1) uniform sampler2D diffuseSampler;
#endif

layout(location = 0) out vec4 outColor;

void main() 
{
#ifndef NO_DIFFUSE
    outColor = fragColor * texture(diffuseSampler, fragTexcoord);
#else 
    outColor = fragColor * texture(g_defaultSampler, fragTexcoord);
#endif

	outColor.xyz *= max(0.065, dot(normalize(-vec3(-0.3, -0.5, 0.1)), fragNormal.xyz));
	outColor.xyz += 0.007;
}
END_CODE,

"defines":["CUSTOM_DATA", "USE_UNIFORM_BUFFER", "NO_DIFFUSE"]
}