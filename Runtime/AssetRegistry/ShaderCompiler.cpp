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

void Shader::Serialize(nlohmann::json& outData) const
{
	assert(false);
}

void Shader::Deserialize(const nlohmann::json& inData)
{
	m_glslVertex = inData["glslVertex"].get<std::string>();
	m_glslFragment = inData["glslFragment"].get<std::string>();

	if (inData.contains("glslCommon"))
	{
		m_glslCommon = inData["glslCommon"].get<std::string>();
	}

	m_defines = inData["defines"].get<std::vector<std::string>>();
	m_includes = inData["includes"].get<std::vector<std::string>>();
}

void ShaderCompiler::Initialize()
{
	m_pInstance = new ShaderCompiler();
	m_pInstance->m_shaderCache.Initialize();
	m_pInstance->m_shaderCache.LoadCache();
	m_pInstance->m_shaderCache.ClearExpired();
}

ShaderCompiler::~ShaderCompiler()
{
	m_pInstance->m_shaderCache.Shutdown();
}

void ShaderCompiler::GeneratePrecompiledGlsl(Shader* shader, std::string& outGLSLCode, const std::vector<std::string>& defines)
{
	outGLSLCode.clear();

	std::string vertexGlsl;
	std::string fragmentGlsl;
	std::string commonGlsl;

	ConvertFromJsonToGlslCode(shader->m_glslVertex, vertexGlsl);
	ConvertFromJsonToGlslCode(shader->m_glslFragment, fragmentGlsl);
	ConvertFromJsonToGlslCode(shader->m_glslCommon, commonGlsl);

	for (const auto& define : defines)
	{
		outGLSLCode += "#DEFINE " + define + "\n";
	}

	outGLSLCode += "\n" + commonGlsl + "\n";
	outGLSLCode += "\n #ifdef VERTEX \n" + vertexGlsl + " \n #endif \n";
	outGLSLCode += "\n #ifdef FRAGMENT \n" + fragmentGlsl + "\n #endif \n";
}

void ShaderCompiler::ConvertRawShaderToJson(const std::string& shaderText, std::string& outCodeInJSON)
{
	outCodeInJSON = shaderText;

	Utils::ReplaceAll(outCodeInJSON, std::string{ '\r' }, std::string{ ' ' });

	vector<size_t> beginCodeTagLocations;
	vector<size_t> endCodeTagLocations;

	Utils::FindAllOccurances(outCodeInJSON, std::string(BeginCodeTag), beginCodeTagLocations);
	Utils::FindAllOccurances(outCodeInJSON, std::string(EndCodeTag), endCodeTagLocations);

	if (beginCodeTagLocations.size() != endCodeTagLocations.size())
	{
		//assert(beginCodeTagLocations.size() == endCodeTagLocations.size());
		SAILOR_LOG("Cannot convert from JSON to GLSL shader's code (doesn't match num of begin/end tags): %s", shaderText.c_str());
		return;
	}

	int shift = 0;
	for (size_t i = 0; i < beginCodeTagLocations.size(); i++)
	{
		const size_t beginLocation = beginCodeTagLocations[i] + shift;
		const size_t endLocation = endCodeTagLocations[i] + shift;

		std::vector<size_t> endls;
		Utils::FindAllOccurances(outCodeInJSON, std::string{ '\n' }, endls, beginLocation, endLocation);
		shift += endls.size() * size_t(strlen(EndLineTag) - 1);

		Utils::ReplaceAll(outCodeInJSON, std::string{ '\n' }, EndLineTag, beginLocation, endLocation);
	}

	Utils::ReplaceAll(outCodeInJSON, BeginCodeTag, std::string{ '\"' } +BeginCodeTag);
	Utils::ReplaceAll(outCodeInJSON, EndCodeTag, EndCodeTag + std::string{ '\"' });
	Utils::ReplaceAll(outCodeInJSON, std::string{ '\t' }, std::string{ ' ' });
}

bool ShaderCompiler::ConvertFromJsonToGlslCode(const std::string& shaderText, std::string& outPureGLSL)
{
	outPureGLSL = shaderText;

	Utils::ReplaceAll(outPureGLSL, EndLineTag, std::string{ '\n' });
	Utils::ReplaceAll(outPureGLSL, BeginCodeTag, std::string{ ' ' });
	Utils::ReplaceAll(outPureGLSL, EndCodeTag, std::string{ ' ' });

	return true;
}

void ShaderCompiler::GeneratePrecompiledGlslPermutations(const UID& assetUID)
{	
	std::shared_ptr<Shader> pShader = m_pInstance->LoadShader(assetUID).lock();

	const unsigned int NumPermutations = std::pow(2, pShader->m_defines.size());

	AssetInfo* assetInfo = AssetRegistry::GetInstance()->GetAssetInfo(assetUID);

	SAILOR_LOG("Generate precompiled glsl code... %s %d", assetInfo->GetAssetFilepath(), NumPermutations);

	if (m_pInstance->m_shaderCache.Contains(assetUID))
	{
		m_pInstance->m_shaderCache.Remove(assetUID);
	}

	for (int i = 1; i < NumPermutations; i++)
	{
		std::vector<std::string> defines;

		for (int define = 0; define < pShader->m_defines.size(); define++)
		{
			if ((i >> define) & 1)
			{
				defines.push_back(pShader->m_defines[define]);
			}
		}

		std::string res;
		GeneratePrecompiledGlsl(pShader.get(), res, defines);

		m_pInstance->m_shaderCache.AddSpirv(assetUID, i, "spirv", res);
	}

	m_pInstance->m_shaderCache.SaveCache();
}

std::weak_ptr<Shader> ShaderCompiler::LoadShader(const UID& uid)
{
	if (ShaderAssetInfo* shaderAssetInfo = dynamic_cast<ShaderAssetInfo*>(AssetRegistry::GetInstance()->GetAssetInfo(uid)))
	{
		const auto& loadedShader = m_loadedShaders.find(uid);
		if (loadedShader != m_loadedShaders.end())
		{
			return loadedShader->second;
		}

		const std::string& filepath = shaderAssetInfo->GetAssetFilepath();

		std::string shaderText;
		std::string codeInJSON;

		AssetRegistry::ReadFile(filepath, shaderText);

		ConvertRawShaderToJson(shaderText, codeInJSON);

		json j_shader;
		if (j_shader.parse(codeInJSON.c_str()) == detail::value_t::discarded)
		{
			SAILOR_LOG("Cannot parse shader asset file: %s", filepath.c_str());
			return weak_ptr<Shader>();
		}

		j_shader = json::parse(codeInJSON);

		Shader* shader = new Shader();
		shader->Deserialize(j_shader);

		return m_loadedShaders[uid] = shared_ptr<Shader>(shader);
	}
	else
	{
		SAILOR_LOG("Cannot find shader asset info with UID: %s", uid.ToString().c_str());
		return std::weak_ptr<Shader>();
	}
}

bool ShaderCompiler::CompileGlslToSpirv(const string& source, const vector<string>& defines, const vector<string>& includes, vector<uint32_t>& outByteCode)
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
