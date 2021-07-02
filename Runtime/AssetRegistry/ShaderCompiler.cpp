#include "ShaderCompiler.h"

#include "UID.h"
#include "AssetRegistry.h"
#include "ShaderAssetInfo.h"
#include "ShaderCache.h"
#include "Utils.h"
#include <filesystem>
#include <fstream>
#include <algorithm>
#include <iostream>

#include "nlohmann_json/include/nlohmann/json.hpp"
#include <shaderc/shaderc.hpp>
#include <thread>
#include <mutex>

#include "JobSystem/JobSystem.h"

#ifdef _DEBUG
#pragma comment(lib, "shaderc_combinedd.lib")
#else
#pragma comment(lib, "shaderc_combined.lib")
#endif

using namespace Sailor;

void ShaderAsset::Serialize(nlohmann::json& outData) const
{
	assert(false);
}

void ShaderAsset::Deserialize(const nlohmann::json& inData)
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
}

ShaderCompiler::~ShaderCompiler()
{
	m_pInstance->m_shaderCache.Shutdown();
}

void ShaderCompiler::GeneratePrecompiledGlsl(ShaderAsset* shader, std::string& outGLSLCode, const std::vector<std::string>& defines)
{
	outGLSLCode.clear();

	std::string vertexGlsl;
	std::string fragmentGlsl;
	std::string commonGlsl;

	ConvertFromJsonToGlslCode(shader->m_glslVertex, vertexGlsl);
	ConvertFromJsonToGlslCode(shader->m_glslFragment, fragmentGlsl);
	ConvertFromJsonToGlslCode(shader->m_glslCommon, commonGlsl);

	outGLSLCode += commonGlsl + "\n";

	for (const auto& define : defines)
	{
		outGLSLCode += "#define " + define + "\n";
	}

	outGLSLCode += "\n #ifdef VERTEX \n" + vertexGlsl + " \n #endif \n";
	outGLSLCode += "\n #ifdef FRAGMENT \n" + fragmentGlsl + "\n #endif \n";
}

void ShaderCompiler::ConvertRawShaderToJson(const std::string& shaderText, std::string& outCodeInJSON)
{
	outCodeInJSON = shaderText;

	Utils::ReplaceAll(outCodeInJSON, std::string{ '\r' }, std::string{ ' ' });

	vector<size_t> beginCodeTagLocations;
	vector<size_t> endCodeTagLocations;

	Utils::FindAllOccurances(outCodeInJSON, std::string(JsonBeginCodeTag), beginCodeTagLocations);
	Utils::FindAllOccurances(outCodeInJSON, std::string(JsonEndCodeTag), endCodeTagLocations);

	if (beginCodeTagLocations.size() != endCodeTagLocations.size())
	{
		//assert(beginCodeTagLocations.size() == endCodeTagLocations.size());
		SAILOR_LOG("Cannot convert from JSON to GLSL shader's code (doesn't match num of begin/end tags): %s", shaderText.c_str());
		return;
	}

	size_t shift = 0;
	for (size_t i = 0; i < beginCodeTagLocations.size(); i++)
	{
		const size_t beginLocation = beginCodeTagLocations[i] + shift;
		const size_t endLocation = endCodeTagLocations[i] + shift;

		std::vector<size_t> endls;
		Utils::FindAllOccurances(outCodeInJSON, std::string{ '\n' }, endls, beginLocation, endLocation);
		shift += endls.size() * size_t(strlen(JsonEndLineTag) - 1);

		Utils::ReplaceAll(outCodeInJSON, std::string{ '\n' }, JsonEndLineTag, beginLocation, endLocation);
	}

	Utils::ReplaceAll(outCodeInJSON, JsonBeginCodeTag, std::string{ '\"' } + JsonBeginCodeTag);
	Utils::ReplaceAll(outCodeInJSON, JsonEndCodeTag, JsonEndCodeTag + std::string{ '\"' });
	Utils::ReplaceAll(outCodeInJSON, std::string{ '\t' }, std::string{ ' ' });
}

bool ShaderCompiler::ConvertFromJsonToGlslCode(const std::string& shaderText, std::string& outPureGLSL)
{
	outPureGLSL = shaderText;

	Utils::ReplaceAll(outPureGLSL, JsonEndLineTag, std::string{ '\n' });
	Utils::ReplaceAll(outPureGLSL, JsonBeginCodeTag, std::string{ ' ' });
	Utils::ReplaceAll(outPureGLSL, JsonEndCodeTag, std::string{ ' ' });

	return true;
}

void ShaderCompiler::ForceCompilePermutation(const UID& assetUID, uint32_t permutation)
{
	auto pShader = m_pInstance->LoadShaderAsset(assetUID).lock();
	const auto defines = GetDefines(pShader->m_defines, permutation);

	std::vector<std::string> vertexDefines = defines;
	vertexDefines.push_back("VERTEX");

	std::vector<std::string> fragmentDefines = defines;
	fragmentDefines.push_back("FRAGMENT");

	std::string vertexGlsl;
	std::string fragmentGlsl;
	GeneratePrecompiledGlsl(pShader.get(), vertexGlsl, vertexDefines);
	GeneratePrecompiledGlsl(pShader.get(), fragmentGlsl, fragmentDefines);

	m_pInstance->m_shaderCache.SavePrecompiledGlsl(assetUID, permutation, vertexGlsl, fragmentGlsl);

	std::vector<char> spirvVertexByteCode;
	std::vector<char> spirvFragmentByteCode;

	const bool bResultCompileVertexShader = CompileGlslToSpirv(vertexGlsl, ShaderCache::GetCachedShaderFilepath(assetUID, permutation, "VERTEX", false), EShaderKind::Vertex, {}, {}, spirvVertexByteCode);
	const bool bResultCompileFragmentShader = CompileGlslToSpirv(fragmentGlsl, ShaderCache::GetCachedShaderFilepath(assetUID, permutation, "FRAGMENT", false), EShaderKind::Fragment, {}, {}, spirvFragmentByteCode);

	if (bResultCompileVertexShader && bResultCompileFragmentShader)
	{
		m_pInstance->m_shaderCache.CacheSpirv_ThreadSafe(assetUID, permutation, spirvVertexByteCode, spirvFragmentByteCode);
	}
}

void ShaderCompiler::CompileAllPermutations(const UID& assetUID)
{
	std::shared_ptr<ShaderAsset> pShader = m_pInstance->LoadShaderAsset(assetUID).lock();

	const uint32_t NumPermutations = (uint32_t)std::pow(2, pShader->m_defines.size());

	AssetInfo* assetInfo = AssetRegistry::GetInstance()->GetAssetInfo(assetUID);

	std::vector<int> permutationsToCompile;

	for (uint32_t permutation = 0; permutation < NumPermutations; permutation++)
	{
		if (m_pInstance->m_shaderCache.IsExpired(assetUID, permutation))
		{
			permutationsToCompile.push_back(permutation);
		}
	}

	if (permutationsToCompile.empty())
	{
		return;
	}

	auto scheduler = JobSystem::Scheduler::GetInstance();

	SAILOR_LOG("Compiling shader: %s Num permutations: %zd", assetInfo->GetAssetFilepath().c_str(), permutationsToCompile.size());

	auto saveCacheJob = scheduler->CreateJob("Save Shader Cache", [=]()
	{
		SAILOR_LOG("Shader compiled %s", assetInfo->GetAssetFilepath().c_str());
		m_pInstance->m_shaderCache.SaveCache();
	});

	for (uint32_t i = 0; i < permutationsToCompile.size(); i++)
	{
		auto job = scheduler->CreateJob("Compile shader", [i, pShader, assetUID, permutationsToCompile]()
		{
			SAILOR_LOG("Start compiling shader %zd", permutationsToCompile[i]);
			ForceCompilePermutation(assetUID, permutationsToCompile[i]);
		});

		saveCacheJob->Wait(job);
		scheduler->Run(job);
	}
	scheduler->Run(saveCacheJob);
}

std::weak_ptr<ShaderAsset> ShaderCompiler::LoadShaderAsset(const UID& uid)
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
			return weak_ptr<ShaderAsset>();
		}

		j_shader = json::parse(codeInJSON);

		ShaderAsset* shader = new ShaderAsset();
		shader->Deserialize(j_shader);

		return m_loadedShaders[uid] = shared_ptr<ShaderAsset>(shader);
	}

	SAILOR_LOG("Cannot find shader asset info with UID: %s", uid.ToString().c_str());
	return std::weak_ptr<ShaderAsset>();
}

bool ShaderCompiler::CompileGlslToSpirv(const std::string& source, const std::string& filename, EShaderKind shaderKind, const std::vector<string>& defines, const std::vector<string>& includes, std::vector<char>& outByteCode)
{
	shaderc::Compiler compiler;
	shaderc::CompileOptions options;

	options.SetSourceLanguage(shaderc_source_language_glsl);

	shaderc_shader_kind kind = shaderc_glsl_default_vertex_shader;

	if (shaderKind == EShaderKind::Fragment)
	{
		shaderc_shader_kind kind = shaderc_glsl_default_fragment_shader;
	}

	shaderc::SpvCompilationResult module = compiler.CompileGlslToSpv(source, kind, filename.c_str(), options);

	if (module.GetCompilationStatus() != shaderc_compilation_status_success)
	{
		SAILOR_LOG("Failed to compile shader: %s", module.GetErrorMessage().c_str());
		return false;
	}

	outByteCode = { module.cbegin(), module.cend() };

	return true;
}

uint32_t ShaderCompiler::GetPermutation(const std::vector<std::string>& defines, const std::vector<std::string>& actualDefines)
{
	uint32_t res = 0;
	for (int32_t i = 0; i < defines.size(); i++)
	{
		if (defines[i] == actualDefines[i])
		{
			res += 1 << i;
		}
	}
	return res;
}

std::vector<std::string> ShaderCompiler::GetDefines(const std::vector<std::string>& defines, uint32_t permutation)
{
	std::vector<std::string> res;

	for (int32_t define = 0; define < defines.size(); define++)
	{
		if ((permutation >> define) & 1)
		{
			res.push_back(defines[define]);
		}
	}

	return res;
}

void ShaderCompiler::GetSpirvCode(const UID& assetUID, const std::vector<std::string>& defines, std::vector<char>& outVertexByteCode, std::vector<char>& outFragmentByteCode)
{
	if (auto pShader = m_pInstance->LoadShaderAsset(assetUID).lock())
	{
		uint32_t permutation = GetPermutation(pShader->m_defines, defines);

		if (m_pInstance->m_shaderCache.IsExpired(assetUID, permutation))
		{
			ForceCompilePermutation(assetUID, permutation);
		}

		m_pInstance->m_shaderCache.GetSpirvCode(assetUID, permutation, outVertexByteCode, outFragmentByteCode);
	}
}

