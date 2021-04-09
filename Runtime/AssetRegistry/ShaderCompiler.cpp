#include "ShaderCompiler.h"

#include "UID.h"
#include "AssetRegistry.h"
#include "ShaderAssetInfo.h"
#include "ShaderCache.h"
#include <filesystem>
#include <fstream>
#include <shaderc/shaderc.hpp>

#include "nlohmann_json/include/nlohmann/json.hpp"

#ifdef _DEBUG
#pragma comment(lib, "shaderc_combinedd.lib")
#else
#pragma comment(lib, "shaderc_combined.lib")
#endif

using namespace Sailor::GfxDeviceVulkan;

void ns::to_json(json& j, const Sailor::GfxDeviceVulkan::Shader& p)
{
}

void ns::from_json(const json& j, Sailor::GfxDeviceVulkan::Shader& p)
{
	p.glslVertex = j["glslVertex"].get<std::string>();
	p.glslFragment = j["glslFragment"].get<std::string>();
	p.glslCommon = j["glslCommon"].get<std::string>();
	p.defines = j["defines"].get<std::vector<std::string>>();
	p.includes = j["includes"].get<std::vector<std::string>>();
}

void ShaderCompiler::CreatePrecompiledShaders(const Sailor::UID& assetUID)
{
	if (ShaderAssetInfo* shaderAssetInfo = dynamic_cast<ShaderAssetInfo*>(AssetRegistry::GetInstance()->GetAssetInfo(assetUID)))
	{
		//ShaderCache::GetInstance()->Load
		const std::string& filepath = shaderAssetInfo->Getfilepath();

		std::ifstream assetFile{ filepath };

		json shaderJson;
		assetFile >> shaderJson;

		Shader shader;
		ns::from_json(shaderJson, shader);

		assetFile.close();
	}
}

bool ShaderCompiler::CompileToSPIRV(const string& source, const vector<string>& defines, const vector<string>& includes, vector<uint32_t>& outByteCode)
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
	return false;
}
