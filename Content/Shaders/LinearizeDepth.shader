{
"includes": [] ,

"colorAttachments":["R32_SFLOAT"],
"glslCommon" :
BEGIN_CODE
#version 460
#extension GL_ARB_separate_shader_objects : enable

layout(set = 0, binding = 0) uniform FrameData
{
    mat4 view;
    mat4 projection;
    vec4 cameraPosition;
    ivec2 viewportSize;
    float currentTime;
    float deltaTime;
} frame;

END_CODE,

"glslVertex":
BEGIN_CODE

layout(location=0) in vec3 inPosition;
layout(location=1) in vec3 inNormal;
layout(location=2) in vec2 inTexcoord;
layout(location=3) in vec4 inColor;

layout(location=0) out vec4 fragColor;
layout(location=1) out vec2 fragTexcoord;

void main() 
{
    gl_Position = vec4(inPosition, 1);
    fragColor = inColor;
	fragTexcoord = inTexcoord;
	
	// Flip Y
	fragTexcoord.y = 1.0f - fragTexcoord.y;
}
END_CODE,

"glslFragment":
BEGIN_CODE
layout(location=0) in vec4 fragColor;
layout(location=1) in vec2 fragTexcoord;

layout(set=1, binding=0) uniform sampler2D depthSampler;
layout(location = 0) out vec4 outColor;

void main() 
{
	
	float depth = texture(depthSampler, fragTexcoord).x * 10000;
    outColor = vec4(depth);
}
END_CODE,

"defines":[]
}