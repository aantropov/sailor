---
includes:
- Shaders/Constants.glsl

defines: 
- REVERSE_Z_INF_FAR_PLANE

colorAttachments :
- R32_SFLOAT

glslCommon: |
 #version 460
 #extension GL_ARB_separate_shader_objects : enable
 
 layout(set = 0, binding = 0) uniform FrameData
 {
 	mat4 view;
 	mat4 projection;
 	vec4 cameraPosition;
 	ivec2 viewportSize;
    vec2 cameraParams;
 	float currentTime;
 	float deltaTime;
 } frame;
 
 layout(push_constant) uniform Constants
 {
 	mat4 invProjection;	
 	vec4 cameraParams; //cameraParams: (Znear, ZFar, 0, 0)
 } PushConstants;
 
glslVertex: |
 layout(location=DefaultPositionBinding) in vec3 inPosition;
 layout(location=DefaultTexcoordBinding) in vec2 inTexcoord;
 
 layout(location=0) out vec2 fragTexcoord;
 
 void main() 
 {
 	gl_Position = vec4(inPosition, 1);
 	fragTexcoord = inTexcoord;
 
 	// Flip Y
 	fragTexcoord.y = 1.0f - fragTexcoord.y;
 }

glslFragment: |
 layout(location=0) in vec2 fragTexcoord;
 
 layout(set=1, binding=0) uniform sampler2D depthSampler;
 layout(location=0) out vec4 outColor;
 
 void main() 
 {
 	float depth = texture(depthSampler, fragTexcoord).x;
 	vec4 vss = vec4(0, 0, depth, 1);	
 	vec4 invVss = PushConstants.invProjection * vss;
 	float zvs = invVss.z / invVss.w;
 	
 #ifdef REVERSE_Z_INF_FAR_PLANE
 	float linearDepth = -PushConstants.cameraParams.y / depth;
 #else
 	float linearDepth = -(PushConstants.cameraParams.x * PushConstants.cameraParams.y)/(depth*(PushConstants.cameraParams.x - PushConstants.cameraParams.y) + PushConstants.cameraParams.y);
 #endif
 	outColor = vec4(-linearDepth);
 }