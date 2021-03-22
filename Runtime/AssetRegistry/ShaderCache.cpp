#include "ShaderCache.h"

#include <iostream>

#include "AssetRegistry.h"
#include <shaderc/shaderc.hpp>
//#include <glslang/Public/ShaderLang.h>

#ifdef _DEBUG
#pragma comment(lib, "..//External//shaderc//Debug//OSDependentd.lib")
#pragma comment(lib, "..//External//shaderc//Debug//HLSLd.lib")
#pragma comment(lib, "..//External//shaderc//Debug//SPIRVd.lib")
#pragma comment(lib, "..//External//shaderc//Debug//OGLCompilerd.lib")
//#pragma comment(lib, "..//External//shaderc//Debug//shaderc.lib")
#pragma comment(lib, "..//External//shaderc//Debug//glslangd.lib")
//#pragma comment(lib, "..//External//shaderc//Debug//shaderc_combined.lib")
#else
#endif

using namespace Sailor;

std::vector<uint32_t> ShaderCache::CompileToSPIRV(const char* sPath)
{
	shaderc::Compiler compiler;
	shaderc::CompileOptions options;

	//shaderc::PreprocessedSourceCompilationResult preprocessed = compiler.PreprocessGlsl(shaderString, kind, fileName.c_str(), options);
	//shaderString = { preprocessed.cbegin(), preprocessed.cend() };

	// compile
	//shaderc::SpvCompilationResult module = compiler.CompileGlslToSpv(shaderString, kind, fileName.c_str(), options);

	//shaderBytecode = { module.cbegin(), module.cend() };// not sure why sample code copy vector like this
	//shaderc::Compiler::CompileGlslToSpv()
	
	/*
	glslang::InitializeProcess();
	glslang::TShader vShader(EShLanguage::EShLangAnyHit);

	std::vector<char> vShaderSrc;
	AssetRegistry::ReadFile(sPath, vShaderSrc);

	const char* srcs[] = { vShaderSrc.data() };
	const int   lens[] = { vShaderSrc.size() };

	const char* inputName[]{ "shaderSource" };
	vShader.setStringsWithLengthsAndNames(srcs, lens, inputName, 1);

	vShader.parse(&DefaultTBuiltInResource, 100, EProfile::EEsProfile, false, true, EShMessages::EShMsgDefault);*/
	return { 0 };
}
