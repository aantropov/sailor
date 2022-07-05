#include "AssetRegistry/Shader/ShaderCompiler.h"

#include "AssetRegistry/UID.h"
#include "AssetRegistry/AssetRegistry.h"
#include "ShaderAssetInfo.h"
#include "ShaderCache.h"
#include "RHI/Shader.h"
#include "Core/Utils.h"
#include <filesystem>
#include <fstream>
#include <algorithm>
#include <iostream>

#include "nlohmann_json/include/nlohmann/json.hpp"
#include <shaderc/shaderc.hpp>
#include <thread>
#include <mutex>

#include "Containers/Set.h"
#include "Tasks/Tasks.h"
#include "Tasks/Scheduler.h"

using namespace Sailor;

bool ShaderSet::IsReady() const
{
	return m_rhiVertexShader && m_rhiFragmentShader;
}

void ShaderAsset::Serialize(nlohmann::json& outData) const
{
	assert(false);
}

void ShaderAsset::Deserialize(const nlohmann::json& inData)
{
	if (inData.contains("glslVertex"))
	{
		m_glslVertex = inData["glslVertex"].get<std::string>();
	}

	if (inData.contains("glslFragment"))
	{
		m_glslFragment = inData["glslFragment"].get<std::string>();
	}

	if (inData.contains("glslCommon"))
	{
		m_glslCommon = inData["glslCommon"].get<std::string>();
	}

	if (inData.contains("defines"))
	{
		m_defines = inData["defines"].get<TVector<std::string>>();
	}

	if (inData.contains("includes"))
	{
		m_includes = inData["includes"].get<TVector<std::string>>();
	}
}

ShaderCompiler::ShaderCompiler(ShaderAssetInfoHandler* infoHandler)
{
	SAILOR_PROFILE_FUNCTION();
	m_allocator = ObjectAllocatorPtr::Make();
	m_shaderCache.Initialize();
	infoHandler->Subscribe(this);

	TVector<UID> shaderAssetInfos;
	App::GetSubmodule<AssetRegistry>()->GetAllAssetInfos<ShaderAssetInfo>(shaderAssetInfos);

	for (const auto& uid : shaderAssetInfos)
	{
		CompileAllPermutations(uid);
	}
}

ShaderCompiler::~ShaderCompiler()
{
	for (auto& shaders : m_loadedShaders)
	{
		for (auto& shader : shaders.m_second)
		{
			shader.m_second.DestroyObject(m_allocator);
		}
	}

	m_shaderCache.Shutdown();
}

void ShaderCompiler::GeneratePrecompiledGlsl(ShaderAsset* shader, std::string& outGLSLCode, const TVector<std::string>& defines)
{
	SAILOR_PROFILE_FUNCTION();

	outGLSLCode.clear();

	std::string vertexGlsl;
	std::string fragmentGlsl;
	std::string commonGlsl;

	ConvertFromJsonToGlslCode(shader->GetGlslVertexCode(), vertexGlsl);
	ConvertFromJsonToGlslCode(shader->GetGlslFragmentCode(), fragmentGlsl);
	ConvertFromJsonToGlslCode(shader->GetGlslCommonCode(), commonGlsl);

	outGLSLCode += commonGlsl + "\n";

	for (const auto& define : defines)
	{
		outGLSLCode += "#define " + define + "\n";
	}

	outGLSLCode += "\n#ifdef VERTEX\n" + vertexGlsl + "\n#endif\n";
	outGLSLCode += "\n#ifdef FRAGMENT\n" + fragmentGlsl + "\n#endif\n";
}

void ShaderCompiler::ConvertRawShaderToJson(const std::string& shaderText, std::string& outCodeInJSON)
{
	SAILOR_PROFILE_FUNCTION();

	outCodeInJSON = shaderText;

	Utils::ReplaceAll(outCodeInJSON, std::string{ '\r' }, std::string{ ' ' });

	TVector<size_t> beginCodeTagLocations;
	TVector<size_t> endCodeTagLocations;

	Utils::FindAllOccurances(outCodeInJSON, std::string(JsonBeginCodeTag), beginCodeTagLocations);
	Utils::FindAllOccurances(outCodeInJSON, std::string(JsonEndCodeTag), endCodeTagLocations);

	if (beginCodeTagLocations.Num() != endCodeTagLocations.Num())
	{
		//assert(beginCodeTagLocations.Num() == endCodeTagLocations.Num());
		SAILOR_LOG("Cannot convert from JSON to GLSL shader's code (doesn't match num of begin/end tags): %s", shaderText.c_str());
		return;
	}

	size_t shift = 0;
	for (size_t i = 0; i < beginCodeTagLocations.Num(); i++)
	{
		const size_t beginLocation = beginCodeTagLocations[i] + shift;
		const size_t endLocation = endCodeTagLocations[i] + shift;

		TVector<size_t> endls;
		Utils::FindAllOccurances(outCodeInJSON, std::string{ '\n' }, endls, beginLocation, endLocation);
		shift += endls.Num() * size_t(strlen(JsonEndLineTag) - 1);

		Utils::ReplaceAll(outCodeInJSON, std::string{ '\n' }, JsonEndLineTag, beginLocation, endLocation);
	}

	Utils::ReplaceAll(outCodeInJSON, JsonBeginCodeTag, std::string{ '\"' } + JsonBeginCodeTag);
	Utils::ReplaceAll(outCodeInJSON, JsonEndCodeTag, JsonEndCodeTag + std::string{ '\"' });
	Utils::ReplaceAll(outCodeInJSON, std::string{ '\t' }, std::string{ ' ' });
}

bool ShaderCompiler::ConvertFromJsonToGlslCode(const std::string& shaderText, std::string& outPureGLSL)
{
	SAILOR_PROFILE_FUNCTION();

	outPureGLSL = shaderText;

	Utils::ReplaceAll(outPureGLSL, JsonEndLineTag, std::string{ '\n' });
	Utils::Erase(outPureGLSL, JsonBeginCodeTag);
	Utils::Erase(outPureGLSL, JsonEndCodeTag);

	Utils::Trim(outPureGLSL);

	return true;
}

bool ShaderCompiler::ForceCompilePermutation(const UID& assetUID, uint32_t permutation)
{
	SAILOR_PROFILE_FUNCTION();

	auto pShader = LoadShaderAsset(assetUID).Lock();
	const auto defines = GetDefines(pShader->GetSupportedDefines(), permutation);

	TVector<std::string> vertexDefines = defines;
	vertexDefines.Add("VERTEX");

	TVector<std::string> fragmentDefines = defines;
	fragmentDefines.Add("FRAGMENT");

	std::string vertexGlsl;
	std::string fragmentGlsl;
	GeneratePrecompiledGlsl(pShader.GetRawPtr(), vertexGlsl, vertexDefines);
	GeneratePrecompiledGlsl(pShader.GetRawPtr(), fragmentGlsl, fragmentDefines);

	m_shaderCache.CachePrecompiledGlsl(assetUID, permutation, vertexGlsl, fragmentGlsl);

	RHI::ShaderByteCode spirvVertexByteCode;
	RHI::ShaderByteCode spirvFragmentByteCode;

	const bool bResultCompileVertexShader = CompileGlslToSpirv(vertexGlsl, RHI::EShaderStage::Vertex, {}, {}, spirvVertexByteCode, false);
	const bool bResultCompileFragmentShader = CompileGlslToSpirv(fragmentGlsl, RHI::EShaderStage::Fragment, {}, {}, spirvFragmentByteCode, false);

	if (bResultCompileVertexShader && bResultCompileFragmentShader)
	{
		m_shaderCache.CacheSpirv_ThreadSafe(assetUID, permutation, spirvVertexByteCode, spirvFragmentByteCode);
	}

	RHI::ShaderByteCode spirvVertexByteCodeDebug;
	RHI::ShaderByteCode spirvFragmentByteCodeDebug;

	const bool bResultCompileVertexShaderDebug = CompileGlslToSpirv(vertexGlsl, RHI::EShaderStage::Vertex, {}, {}, spirvVertexByteCodeDebug, true);
	const bool bResultCompileFragmentShaderDebug = CompileGlslToSpirv(fragmentGlsl, RHI::EShaderStage::Fragment, {}, {}, spirvFragmentByteCodeDebug, true);

	if (bResultCompileVertexShaderDebug && bResultCompileFragmentShaderDebug)
	{
		m_shaderCache.CacheSpirvWithDebugInfo(assetUID, permutation, spirvVertexByteCodeDebug, spirvFragmentByteCodeDebug);
	}

	return bResultCompileVertexShader && bResultCompileFragmentShader;
}

Tasks::TaskPtr<bool> ShaderCompiler::CompileAllPermutations(const UID& assetUID)
{
	SAILOR_PROFILE_FUNCTION();
	if (TWeakPtr<ShaderAsset> pWeakShader = LoadShaderAsset(assetUID))
	{
		TSharedPtr<ShaderAsset> pShader = pWeakShader.Lock();
		AssetInfoPtr assetInfo = App::GetSubmodule<AssetRegistry>()->GetAssetInfoPtr(assetUID);

		if (!pShader->ContainsFragment() || !pShader->ContainsVertex())
		{
			SAILOR_LOG("Skip shader compilation (missing fragment/vertex module): %s", assetInfo->GetAssetFilepath().c_str());

			return Tasks::TaskPtr<bool>::Make(false);
		}

		const uint32_t NumPermutations = (uint32_t)std::pow(2, pShader->GetSupportedDefines().Num());

		TVector<uint32_t> permutationsToCompile;

		for (uint32_t permutation = 0; permutation < NumPermutations; permutation++)
		{
			if (m_shaderCache.IsExpired(assetUID, permutation))
			{
				permutationsToCompile.Add(permutation);
			}
		}

		if (permutationsToCompile.IsEmpty())
		{
			return Tasks::TaskPtr<bool>::Make(false);
		}

		auto scheduler = App::GetSubmodule<Tasks::Scheduler>();

		SAILOR_LOG("Compiling shader: %s Num permutations: %zd", assetInfo->GetAssetFilepath().c_str(), permutationsToCompile.Num());

		Tasks::TaskPtr<bool> saveCacheJob = scheduler->CreateTaskWithResult<bool>("Save Shader Cache", [=]()
		{
			SAILOR_LOG("Shader compiled %s", assetInfo->GetAssetFilepath().c_str());
			m_shaderCache.SaveCache();

			//Unload shader asset text
			m_shaderAssetsCache.Remove(assetInfo->GetUID());

			return true;
		});

		for (uint32_t i = 0; i < permutationsToCompile.Num(); i++)
		{
			Tasks::ITaskPtr job = scheduler->CreateTask("Compile shader", [i, pShader, assetUID, permutationsToCompile]()
			{
				SAILOR_LOG("Start compiling shader %d", permutationsToCompile[i]);
				App::GetSubmodule<ShaderCompiler>()->ForceCompilePermutation(assetUID, permutationsToCompile[i]);
			});

			saveCacheJob->Join(job);
			scheduler->Run(job);
		}
		scheduler->Run(saveCacheJob);

		return saveCacheJob;
	}
	else
	{
		SAILOR_LOG("Cannot find shader asset %s", assetUID.ToString().c_str());
	}

	return Tasks::TaskPtr<bool>::Make(false);
}

TWeakPtr<ShaderAsset> ShaderCompiler::LoadShaderAsset(const UID& uid)
{
	SAILOR_PROFILE_FUNCTION();

	if (ShaderAssetInfoPtr shaderAssetInfo = dynamic_cast<ShaderAssetInfoPtr>(App::GetSubmodule<AssetRegistry>()->GetAssetInfoPtr(uid)))
	{
		if (const auto& loadedShader = m_shaderAssetsCache.Find(uid); loadedShader != m_shaderAssetsCache.end())
		{
			return loadedShader->m_second;
		}

		const std::string& filepath = shaderAssetInfo->GetAssetFilepath();

		std::string shaderText;
		std::string codeInJSON;

		AssetRegistry::ReadAllTextFile(filepath, shaderText);

		ConvertRawShaderToJson(shaderText, codeInJSON);

		json j_shader;
		if (j_shader.parse(codeInJSON.c_str()) == nlohmann::detail::value_t::discarded)
		{
			SAILOR_LOG("Cannot parse shader asset file: %s", filepath.c_str());
			return TWeakPtr<ShaderAsset>();
		}

		j_shader = json::parse(codeInJSON);

		ShaderAsset* shader = new ShaderAsset();
		shader->Deserialize(j_shader);

		return m_shaderAssetsCache[uid] = TSharedPtr<ShaderAsset>(shader);
	}

	SAILOR_LOG("Cannot find shader asset info with UID: %s", uid.ToString().c_str());
	return TWeakPtr<ShaderAsset>();
}

void ShaderCompiler::OnUpdateAssetInfo(AssetInfoPtr assetInfo, bool bWasExpired)
{
	if (bWasExpired)
	{
		m_shaderAssetsCache.Remove(assetInfo->GetUID());

		auto compileTask = CompileAllPermutations(assetInfo->GetUID());
		if (compileTask)
		{
			compileTask->Then<void, bool>([=](bool bRes)
			{
				if (bRes)
				{
					for (auto& loadedShader : m_loadedShaders[assetInfo->GetUID()])
					{
						SAILOR_LOG("Update shader RHI resource: %s permutation: %lu", assetInfo->GetAssetFilepath().c_str(), loadedShader.m_first);

						if (UpdateRHIResource(loadedShader.m_second, loadedShader.m_first))
						{
							loadedShader.m_second->TraceHotReload(nullptr);
						}
					}
				}
			}
			);
		}
	}
}

void ShaderCompiler::OnImportAsset(AssetInfoPtr assetInfo)
{
	CompileAllPermutations(assetInfo->GetUID());
}

bool ShaderCompiler::CompileGlslToSpirv(const std::string& source, RHI::EShaderStage shaderStage, const TVector<string>& defines, const TVector<string>& includes, RHI::ShaderByteCode& outByteCode, bool bIsDebug)
{
	SAILOR_PROFILE_FUNCTION();

	shaderc::Compiler compiler;
	shaderc::CompileOptions options;

	options.SetSourceLanguage(shaderc_source_language_glsl);

	if (bIsDebug)
	{
		options.SetGenerateDebugInfo();
		options.SetOptimizationLevel(shaderc_optimization_level_zero);
	}
	else
	{
		options.SetOptimizationLevel(shaderc_optimization_level_performance);
	}

	shaderc_shader_kind kind = shaderStage == RHI::EShaderStage::Fragment ? shaderc_glsl_fragment_shader : shaderc_glsl_vertex_shader;
	shaderc::SpvCompilationResult module = compiler.CompileGlslToSpv(source, kind, source.c_str(), "main", options);

	if (module.GetCompilationStatus() != shaderc_compilation_status::shaderc_compilation_status_success)
	{
		SAILOR_LOG("Failed to compile shader: %s", module.GetErrorMessage().c_str());
		return false;
	}

	outByteCode = TVector(module.cbegin(), module.cend() - module.cbegin());
	return true;
}

uint32_t ShaderCompiler::GetPermutation(const TVector<std::string>& defines, const TVector<std::string>& actualDefines)
{
	SAILOR_PROFILE_FUNCTION();

	if (actualDefines.Num() == 0)
	{
		return 0;
	}

	TSet<std::string> requested;

	for (int32_t i = 0; i < actualDefines.Num(); i++)
	{
		requested.Insert(actualDefines[i]);
	}

	uint32_t res = 0;
	for (int32_t i = 0; i < defines.Num(); i++)
	{
		if (requested.Contains(defines[i]))
		{
			res += 1 << i;
		}
	}
	return res;
}

TVector<std::string> ShaderCompiler::GetDefines(const TVector<std::string>& defines, uint32_t permutation)
{
	SAILOR_PROFILE_FUNCTION();

	TVector<std::string> res;

	for (int32_t define = 0; define < defines.Num(); define++)
	{
		if ((permutation >> define) & 1)
		{
			res.Add(defines[define]);
		}
	}

	return res;
}

bool ShaderCompiler::GetSpirvCode(const UID& assetUID, const TVector<std::string>& defines, RHI::ShaderByteCode& outVertexByteCode, RHI::ShaderByteCode& outFragmentByteCode, bool bIsDebug)
{
	if (auto pShader = LoadShaderAsset(assetUID).Lock())
	{
		return GetSpirvCode(assetUID, GetPermutation(pShader->GetSupportedDefines(), defines), outVertexByteCode, outFragmentByteCode, bIsDebug);
	}
	return false;
}

bool ShaderCompiler::GetSpirvCode(const UID& assetUID, uint32_t permutation, RHI::ShaderByteCode& outVertexByteCode, RHI::ShaderByteCode& outFragmentByteCode, bool bIsDebug)
{
	SAILOR_PROFILE_FUNCTION();

	if (auto pShader = LoadShaderAsset(assetUID).Lock())
	{
		bool bCompiledSuccesfully = true;
		if (m_shaderCache.IsExpired(assetUID, permutation))
		{
			bCompiledSuccesfully = ForceCompilePermutation(assetUID, permutation);
		}

		return m_shaderCache.GetSpirvCode(assetUID, permutation, outVertexByteCode, outFragmentByteCode, bIsDebug) && bCompiledSuccesfully;
	}

	return false;
}

Tasks::TaskPtr<ShaderSetPtr> ShaderCompiler::LoadShader(UID uid, ShaderSetPtr& outShader, const TVector<string>& defines)
{
	SAILOR_PROFILE_FUNCTION();

	Tasks::TaskPtr<ShaderSetPtr> newPromise;
	outShader = nullptr;

	if (auto pShader = LoadShaderAsset(uid).TryLock())
	{
		const uint32_t permutation = GetPermutation(pShader->GetSupportedDefines(), defines);

		// Check promises first
		auto it = m_promises.Find(uid);
		if (it != m_promises.end())
		{
			auto allLoadedPermutations = (*it).m_second;
			auto shaderIt = std::find_if(allLoadedPermutations.begin(), allLoadedPermutations.end(), [=](const auto& p) { return p.m_first == permutation; });
			if (shaderIt != allLoadedPermutations.end())
			{
				newPromise = (*shaderIt).m_second;
			}
		}

		// Check loaded shaders then
		auto loadedShadersIt = m_loadedShaders.Find(uid);
		if (loadedShadersIt != m_loadedShaders.end())
		{
			auto allLoadedPermutations = (*loadedShadersIt).m_second;
			auto shaderIt = std::find_if(allLoadedPermutations.begin(), allLoadedPermutations.end(), [=](const auto& p) { return p.m_first == permutation; });
			if (shaderIt != allLoadedPermutations.end())
			{
				outShader = (*shaderIt).m_second;
				if (newPromise != nullptr)
				{
					if (!newPromise)
					{
						return Tasks::TaskPtr<ShaderSetPtr>::Make(outShader);
					}

					return newPromise;
				}
			}
		}

		auto& entry = m_promises.At_Lock(uid);

		// We have promise
		if (entry.Num() > 0)
		{
			const size_t index = entry.FindIf([&](const auto& el) { return el.m_first == permutation; });
			if (index != -1)
			{
				outShader = m_loadedShaders[uid][index].m_second;
				m_promises.Unlock(uid);

				return entry[index].m_second;
			}
		}

		if (ShaderAssetInfoPtr assetInfo = App::GetSubmodule<AssetRegistry>()->GetAssetInfoPtr<ShaderAssetInfoPtr>(uid))
		{
			auto pShader = ShaderSetPtr::Make(m_allocator, uid);

			newPromise = Tasks::Scheduler::CreateTaskWithResult<ShaderSetPtr>("Load shader",
				[pShader, assetInfo, defines, this, permutation]()
			{
				UpdateRHIResource(pShader, permutation);
				return pShader;
			});
						

			m_loadedShaders[uid].Add({ permutation, pShader });
			entry.Add({ permutation, newPromise });
			outShader = (*(m_loadedShaders[uid].end() - 1)).m_second;

			App::GetSubmodule<Tasks::Scheduler>()->Run(newPromise);
			m_promises.Unlock(uid);

			return (entry.end() - 1)->m_second;
		}
	}

	m_promises.Unlock(uid);

	SAILOR_LOG("Cannot find shader with uid: %s", uid.ToString().c_str());
	return Tasks::TaskPtr<ShaderSetPtr>();
}

bool ShaderCompiler::LoadShader_Immediate(UID uid, ShaderSetPtr& outShader, const TVector<string>& defines)
{
	SAILOR_PROFILE_FUNCTION();
	auto task = LoadShader(uid, outShader, defines);
	task->Wait();
	return task->GetResult();
}

bool ShaderCompiler::UpdateRHIResource(ShaderSetPtr pShader, uint32_t permutation)
{
	SAILOR_PROFILE_FUNCTION();

	auto pRaw = pShader.GetRawPtr();
	auto& pRhiDriver = App::GetSubmodule<RHI::Renderer>()->GetDriver();

	RHI::ShaderByteCode debugVertexSpirv;
	RHI::ShaderByteCode debugFragmentSpirv;
	if (!GetSpirvCode(pShader->GetUID(), permutation, debugVertexSpirv, debugFragmentSpirv, true))
	{
		return false;
	}

	RHI::ShaderByteCode vertexByteCode;
	RHI::ShaderByteCode fragmentByteCode;
	if (!GetSpirvCode(pShader->GetUID(), permutation, vertexByteCode, fragmentByteCode, false))
	{
		return false;
	}

	pRaw->m_rhiVertexShaderDebug = pRhiDriver->CreateShader(RHI::EShaderStage::Vertex, debugVertexSpirv);
	pRaw->m_rhiFragmentShaderDebug = pRhiDriver->CreateShader(RHI::EShaderStage::Fragment, debugFragmentSpirv);

	pRaw->m_rhiVertexShader = pRhiDriver->CreateShader(RHI::EShaderStage::Vertex, vertexByteCode);
	pRaw->m_rhiFragmentShader = pRhiDriver->CreateShader(RHI::EShaderStage::Fragment, fragmentByteCode);

	return true;
}

void ShaderCompiler::CollectGarbage()
{
	TVector<UID> uidsToRemove;
	for (auto& promiseSet : m_promises)
	{
		bool bFullyCompiled = true;
		for (auto& promise : promiseSet.m_second)
		{
			if (!promise.m_second->IsFinished())
			{
				bFullyCompiled = false;
				break;
			}
		}

		if (bFullyCompiled)
		{
			uidsToRemove.Emplace(promiseSet.m_first);
		}
	}

	for (auto& uid : uidsToRemove)
	{
		m_promises.Remove(uid);
	}
}