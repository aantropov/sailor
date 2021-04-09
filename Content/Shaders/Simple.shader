{
"includes":["basic.shader"],

"glslVertex":{
BEGIN_CODE
	#version 450
	#extension GL_ARB_separate_shader_objects : enable
	
	layout(location = 0) out vec3 fragColor;
	
	vec2 positions[3] = vec2[]
	(
		vec2(0.0, -0.5),
		vec2(0.5, 0.5),
		vec2(-0.5, 0.5)
	);
	
	vec3 colors[3] = vec3[]
	(
		vec3(1.0, 0.0, 0.0),
		vec3(0.0, 1.0, 0.0),
		vec3(0.0, 0.0, 1.0)
	);
	
	void main() 
	{
		gl_Position = vec4(positions[gl_VertexIndex], 0.0, 1.0);
		fragColor = colors[gl_VertexIndex];
	}
END_CODE
},

"glslFragment":{
BEGIN_CODE
	#version 450
	#extension GL_ARB_separate_shader_objects : enable

	layout(location = 0) in vec3 fragColor;
	layout(location = 0) out vec4 outColor;

	void main() 
	{
	#ifdef TEST_DEFINE1
		outColor = vec4(fragColor, 1.0);
	#endif
	
	#ifdef TEST_DEFINE2
		outColor = vec4(fragColor, 1.0);
	#endif
	}
END_CODE
},

"defines":["TEST_DEFINE1","TEST_DEFINE2"]
}