#include "ShaderCompiler.h"

#include "UID.h"
#include "AssetRegistry.h"
#include "ShaderAssetInfo.h"
#include "ShaderCache.h"
#include "Utils.h"
#include <filesystem>
#include <fstream>
#include <algorithm>
#include <shaderc/shaderc.hpp>
#include "nlohmann_json/include/nlohmann/json.hpp"

#ifdef _DEBUG
#pragma comment(lib, "shaderc_combinedd.lib")
#else
#pragma comment(lib, "shaderc_combined.lib")
#endif

using namespace Sailor;

void ns::to_json(json& j, const Sailor::Shader& p)
{
	assert(false);
}

void ns::from_json(const json& j, Sailor::Shader& p)
{
	p.glslVertex = j["glslVertex"].get<std::string>();
	p.glslFragment = j["glslFragment"].get<std::string>();

	if (j.contains("glslCommon"))
	{
		p.glslCommon = j["glslCommon"].get<std::string>();
	}

	p.defines = j["defines"].get<std::vector<std::string>>();
	p.includes = j["includes"].get<std::vector<std::string>>();
}

void ShaderCompiler::Initialize()
{
	instance = new ShaderCompiler();
	instance->shaderCache.Load();
}

void ShaderCompiler::GeneratePrecompiledGLSL(Shader* shader, std::string& outGLSLCode, const std::vector<std::string>& defines)
{
	outGLSLCode.clear();

	std::string vertexGLSL;
	std::string fragmentGLSL;
	std::string commonGLSL;

	ConvertFromJsonToGLSLCode(shader->glslVertex, vertexGLSL);
	ConvertFromJsonToGLSLCode(shader->glslFragment, fragmentGLSL);
	ConvertFromJsonToGLSLCode(shader->glslCommon, commonGLSL);

	for (const auto& define : defines)
	{
		outGLSLCode += "#DEFINE " + define + "\n";
	}

	outGLSLCode += "\n" + commonGLSL + "\n";
	outGLSLCode += "\n #ifdef VERTEX \n" + vertexGLSL + " \n #endif \n";
	outGLSLCode += "\n #ifdef FRAGMENT \n" + fragmentGLSL + "\n #endif \n";
}

void ShaderCompiler::ConvertRawShaderToJSON(const std::string& shaderText, std::string& outCodeInJSON)
{
	outCodeInJSON = shaderText;

	Utils::ReplaceAll(outCodeInJSON, std::string{ '\r' }, std::string{ ' ' });

	vector<size_t> beginCodeTagLocations;
	vector<size_t> endCodeTagLocations;

	Utils::FindAllOccurances(outCodeInJSON, std::string(beginCodeTag), beginCodeTagLocations);
	Utils::FindAllOccurances(outCodeInJSON, std::string(endCodeTag), endCodeTagLocations);

	if (beginCodeTagLocations.size() != endCodeTagLocations.size())
	{
		//assert(beginCodeTagLocations.size() == endCodeTagLocations.size());
		SAILOR_LOG("Cannot convert from JSON to GLSL shader's code (doesn't match num of begin/end tags): %s", shaderText);
		return;
	}
	
	int shift = 0;
	for (size_t i = 0; i < beginCodeTagLocations.size(); i++)
	{
		size_t beginLocation = beginCodeTagLocations[i] + shift;
		size_t endLocation = endCodeTagLocations[i] + shift;

		std::vector<size_t> endls;
		Utils::FindAllOccurances(outCodeInJSON, std::string{ '\n' }, endls, beginLocation, endLocation);
		shift += endls.size() * (strlen(endLineTag) - 1);

		Utils::ReplaceAll(outCodeInJSON, std::string{ '\n' }, endLineTag, beginLocation, endLocation);
	}

	Utils::ReplaceAll(outCodeInJSON, beginCodeTag, std::string{ '\"' } + beginCodeTag);
	Utils::ReplaceAll(outCodeInJSON, endCodeTag, endCodeTag + std::string{ '\"' });
	Utils::ReplaceAll(outCodeInJSON, std::string{ '\t' }, std::string{ ' ' });
}

bool ShaderCompiler::ConvertFromJsonToGLSLCode(const std::string& shaderText, std::string& outPureGLSL)
{
	outPureGLSL = shaderText;

	Utils::ReplaceAll(outPureGLSL, endLineTag, std::string{ '\n' });
	Utils::ReplaceAll(outPureGLSL, beginCodeTag, std::string{ ' ' });
	Utils::ReplaceAll(outPureGLSL, endCodeTag, std::string{ ' ' });

	return true;
}

void ShaderCompiler::GeneratePrecompiledGLSLPermutations(const UID& assetUID, std::vector<std::string>& outPrecompiledShadersCode)
{
	std::weak_ptr<Shader> shader = instance->LoadShader(assetUID);

	std::string res;
	GeneratePrecompiledGLSL(shader.lock().get(), res, {"VERTEX", "TEST_DEFINE1"});
}

std::weak_ptr<Shader> ShaderCompiler::LoadShader(const UID& uid)
{
	if (ShaderAssetInfo* shaderAssetInfo = dynamic_cast<ShaderAssetInfo*>(AssetRegistry::GetInstance()->GetAssetInfo(uid)))
	{
		const auto& loadedShader = loadedShaders.find(uid);
		if (loadedShader != loadedShaders.end())
		{
			return loadedShader->second;
		}

		const std::string& filepath = shaderAssetInfo->GetAssetFilepath();

		std::string shaderText;
		std::string codeInJSON;

		AssetRegistry::ReadFile(filepath, shaderText);

		ConvertRawShaderToJSON(shaderText, codeInJSON);				

		json j_shader;
		if (j_shader.parse(codeInJSON.c_str()) == detail::value_t::discarded)
		{
			SAILOR_LOG("Cannot parse shader asset file: %s", filepath.c_str());
			return weak_ptr<Shader>();
		}

		j_shader = json::parse(codeInJSON);

		Shader* shader = new Shader();
		ns::from_json(j_shader, *shader);

		return loadedShaders[uid] = shared_ptr<Shader>(shader);
	}
	else
	{
		SAILOR_LOG("Cannot find shader asset info with UID: %s", uid.ToString().c_str());
		return std::weak_ptr<Shader>();
	}
}

bool ShaderCompiler::CompileGLSLToSPIRV(const string& source, const vector<string>& defines, const vector<string>& includes, vector<uint32_t>& outByteCode)
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
