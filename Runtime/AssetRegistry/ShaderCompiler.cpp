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

	Utils::FindAllOccurances(outCodeInJSON, std::string(BeginCodeTag), beginCodeTagLocations);
	Utils::FindAllOccurances(outCodeInJSON, std::string(EndCodeTag), endCodeTagLocations);

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
		shift += endls.size() * size_t(strlen(EndLineTag) - 1);

		Utils::ReplaceAll(outCodeInJSON, std::string{ '\n' }, EndLineTag, beginLocation, endLocation);
	}

	Utils::ReplaceAll(outCodeInJSON, BeginCodeTag, std::string{ '\"' } + BeginCodeTag);
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

void ShaderCompiler::CompileAllPermutations(const UID& assetUID)
{
	if (!m_pInstance->m_shaderCache.IsExpired(assetUID))
	{
		return;
	}

	std::shared_ptr<Shader> pShader = m_pInstance->LoadShader(assetUID).lock();

	const unsigned int NumPermutations = (unsigned int)std::pow(2, pShader->m_defines.size());

	AssetInfo* assetInfo = AssetRegistry::GetInstance()->GetAssetInfo(assetUID);

	if (m_pInstance->m_shaderCache.Contains(assetUID))
	{
		m_pInstance->m_shaderCache.Remove(assetUID);
	}

	const unsigned int MaxThreads = 12;
	const unsigned int NumActiveThreads = min(NumPermutations, MaxThreads);
	const unsigned int PermutationsPerThread = min(NumPermutations, NumPermutations / NumActiveThreads);

	std::mutex logMutex;
	std::mutex saveToCacheMutex;
	std::array<std::thread, MaxThreads> threadsPool;

	SAILOR_LOG("Compiling shader: %s Num threads: %d Num permutations: %d", assetInfo->GetAssetFilepath().c_str(), NumActiveThreads, NumPermutations);

	for (unsigned int i = 0; i < NumActiveThreads; i++)
	{
		const unsigned int start = i * PermutationsPerThread;
		const unsigned int end = i == NumActiveThreads - 1 ? NumPermutations : min(NumPermutations, start + PermutationsPerThread);

		threadsPool[i] = std::thread([&logMutex, start, end, &pShader, &assetUID, &saveToCacheMutex]()
		{
			logMutex.lock();
			std::cout << "Start compiling shaders from " << start << " to " << end << endl;
			logMutex.unlock();

			for (unsigned int permutation = start; permutation < end; permutation++)
			{
				std::vector<std::string> defines;

				for (int define = 0; define < pShader->m_defines.size(); define++)
				{
					if ((permutation >> define) & 1)
					{
						defines.push_back(pShader->m_defines[define]);
					}
				}

				std::vector<std::string> vertexDefines = defines;
				vertexDefines.push_back("VERTEX");

				std::vector<std::string> fragmentDefines = defines;
				fragmentDefines.push_back("FRAGMENT");

				std::string vertexGlsl;
				std::string fragmentGlsl;
				GeneratePrecompiledGlsl(pShader.get(), vertexGlsl, vertexDefines);
				GeneratePrecompiledGlsl(pShader.get(), fragmentGlsl, fragmentDefines);

				m_pInstance->m_shaderCache.SavePrecompiledGlsl(assetUID, permutation, vertexGlsl, fragmentGlsl);

				std::vector<uint32_t> spirvVertexByteCode;
				std::vector<uint32_t> spirvFragmentByteCode;

				const bool bResultCompileVertexShader = CompileGlslToSpirv(vertexGlsl, ShaderCache::GetCachedShaderFilepath(assetUID, permutation, "VERTEX", false), EShaderKind::Vertex, {}, {}, spirvVertexByteCode);
				const bool bResultCompileFragmentShader = CompileGlslToSpirv(fragmentGlsl, ShaderCache::GetCachedShaderFilepath(assetUID, permutation, "FRAGMENT", false), EShaderKind::Fragment, {}, {}, spirvFragmentByteCode);

				if (bResultCompileVertexShader && bResultCompileFragmentShader)
				{
					saveToCacheMutex.lock();
					m_pInstance->m_shaderCache.CacheSpirv(assetUID, permutation, spirvVertexByteCode, spirvFragmentByteCode);
					saveToCacheMutex.unlock();
				}
			}

			logMutex.lock();
			std::cout << "Compiled " << start << " to " << end << endl;
			logMutex.unlock();
		});
	}

	for (unsigned int i = 0; i < NumActiveThreads; i++)
	{
		threadsPool[i].join();
	}

	SAILOR_LOG("Shader compiled %s", assetInfo->GetAssetFilepath().c_str());

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

	SAILOR_LOG("Cannot find shader asset info with UID: %s", uid.ToString().c_str());
	return std::weak_ptr<Shader>();
}

bool ShaderCompiler::CompileGlslToSpirv(const std::string& source, const std::string& filename, EShaderKind shaderKind, const std::vector<string>& defines, const std::vector<string>& includes, std::vector<uint32_t>& outByteCode)
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
