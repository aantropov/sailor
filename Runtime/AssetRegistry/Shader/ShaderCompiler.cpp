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

#include "Core/YamlSerializable.h"
#include <shaderc/shaderc.hpp>
#include <thread>
#include <mutex>

#include "Containers/Set.h"
#include "Tasks/Tasks.h"
#include "Tasks/Scheduler.h"

using namespace Sailor;

class ShaderIncluder : public shaderc::CompileOptions::IncluderInterface
{
	shaderc_include_result* GetInclude(const char* requestedSource, shaderc_include_type type, const char* requestingSource, size_t includeDepth)
	{
		string contents = ReadIncludeFile(requestedSource);

		auto container = new std::array<std::string, 2>;
		(*container)[0] = requestedSource;
		(*container)[1] = contents;

		shaderc_include_result* data = new shaderc_include_result();

		data->user_data = container;

		data->source_name = (*container)[0].data();
		data->source_name_length = (*container)[0].size() + 1;

		data->content = (*container)[1].data();
		data->content_length = (*container)[1].size() + 1;

		return data;
	};

	void ReleaseInclude(shaderc_include_result* data) override
	{
		delete static_cast<std::array<std::string, 2>*>(data->user_data);
		delete data;
	};

public:

	static std::string ReadIncludeFile(const std::string& requestedSource)
	{
		const string filepath = AssetRegistry::ContentRootFolder + string(requestedSource);
		string contents = "";
		AssetRegistry::ReadAllTextFile(filepath, contents);
		return contents;
	}
};

bool ShaderSet::IsReady() const
{
	return (m_rhiVertexShader && m_rhiFragmentShader) || m_rhiComputeShader;
}

void ShaderAsset::Deserialize(const YAML::Node& inData)
{
	if (inData["glslVertex"])
	{
		m_glslVertex = inData["glslVertex"].as<std::string>();
	}

	if (inData["glslFragment"])
	{
		m_glslFragment = inData["glslFragment"].as<std::string>();
	}

	if (inData["glslCompute"])
	{
		m_glslCompute = inData["glslCompute"].as<std::string>();
	}

	if (inData["glslCommon"])
	{
		m_glslCommon = inData["glslCommon"].as<std::string>();
	}

	if (inData["defines"])
	{
		m_defines = inData["defines"].as<TVector<std::string>>();
	}

	if (inData["includes"])
	{
		m_includes = inData["includes"].as<TVector<std::string>>();
	}

	if (inData["colorAttachments"])
	{
		for (const auto& str : inData["colorAttachments"])
		{
			RHI::EFormat format;
			DeserializeEnum<RHI::EFormat>(str, format);
			m_colorAttachments.Add(format);
		}
	}

	if (inData["depthStencilAttachment"])
	{
		DeserializeEnum<RHI::EFormat>(inData["depthStencilAttachment"], m_depthStencilAttachment);
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

void ShaderCompiler::GeneratePrecompiledGlsl(ShaderAsset* shader, std::string& outGLSLCode, const TVector<std::string>& includes, const TVector<std::string>& defines)
{
	SAILOR_PROFILE_FUNCTION();

	outGLSLCode.clear();

	if (shader->ContainsCommon())
	{
		outGLSLCode += shader->GetGlslCommonCode() + "\n";
	}

	for (const auto& define : defines)
	{
		outGLSLCode += "#define " + define + "\n";
	}

	for (const auto& include : includes)
	{
		outGLSLCode += ShaderIncluder::ReadIncludeFile(include) + "\n";
	}

	if (shader->ContainsVertex() && defines.Contains("VERTEX"))
	{
		outGLSLCode += "\n#ifdef VERTEX\n" + shader->GetGlslVertexCode() + "\n#endif\n";
	}

	if (shader->ContainsFragment() && defines.Contains("FRAGMENT"))
	{
		outGLSLCode += "\n#ifdef FRAGMENT\n" + shader->GetGlslFragmentCode() + "\n#endif\n";
	}

	if (shader->ContainsCompute())
	{
		outGLSLCode += "\n#ifdef COMPUTE\n" + shader->GetGlslComputeCode() + "\n#endif\n";
	}
}

bool ShaderCompiler::ForceCompilePermutation(ShaderAssetInfoPtr assetInfo, uint32_t permutation)
{
	SAILOR_PROFILE_FUNCTION();

	auto pShader = LoadShaderAsset(assetInfo).Lock();
	const auto defines = GetDefines(pShader->GetSupportedDefines(), permutation);

	std::string vertexGlsl;
	std::string fragmentGlsl;
	std::string computeGlsl;

	TVector<std::string> vertexDefines = defines;
	TVector<std::string> fragmentDefines = defines;
	TVector<std::string> computeDefines = defines;

	vertexDefines.Add("VERTEX");
	fragmentDefines.Add("FRAGMENT");
	computeDefines.Add("COMPUTE");

	if (pShader->ContainsVertex())
	{
		GeneratePrecompiledGlsl(pShader.GetRawPtr(), vertexGlsl, pShader->GetIncludes(), vertexDefines);
	}

	if (pShader->ContainsFragment())
	{
		GeneratePrecompiledGlsl(pShader.GetRawPtr(), fragmentGlsl, pShader->GetIncludes(), fragmentDefines);
	}

	if (pShader->ContainsCompute())
	{
		GeneratePrecompiledGlsl(pShader.GetRawPtr(), computeGlsl, pShader->GetIncludes(), computeDefines);
	}

	m_shaderCache.CachePrecompiledGlsl(assetInfo->GetUID(), permutation, vertexGlsl, fragmentGlsl, computeGlsl);

	RHI::ShaderByteCode spirvVertexByteCode;
	RHI::ShaderByteCode spirvFragmentByteCode;
	RHI::ShaderByteCode spirvComputeByteCode;

	const std::string filename = assetInfo->GetAssetFilepath();

	const bool bResultCompileVertexShader = pShader->ContainsVertex() && CompileGlslToSpirv(filename, vertexGlsl, RHI::EShaderStage::Vertex, spirvVertexByteCode, false);
	const bool bResultCompileFragmentShader = pShader->ContainsFragment() && CompileGlslToSpirv(filename, fragmentGlsl, RHI::EShaderStage::Fragment, spirvFragmentByteCode, false);
	const bool bResultCompileComputeShader = pShader->ContainsCompute() && CompileGlslToSpirv(filename, computeGlsl, RHI::EShaderStage::Compute, spirvComputeByteCode, false);

	if ((bResultCompileVertexShader && bResultCompileFragmentShader) || bResultCompileComputeShader)
	{
		m_shaderCache.CacheSpirv_ThreadSafe(assetInfo->GetUID(), permutation, spirvVertexByteCode, spirvFragmentByteCode, spirvComputeByteCode);
	}

	RHI::ShaderByteCode spirvVertexByteCodeDebug;
	RHI::ShaderByteCode spirvFragmentByteCodeDebug;
	RHI::ShaderByteCode spirvComputeByteCodeDebug;

	const bool bResultCompileVertexShaderDebug = pShader->ContainsVertex() && CompileGlslToSpirv(filename, vertexGlsl, RHI::EShaderStage::Vertex, spirvVertexByteCodeDebug, true);
	const bool bResultCompileFragmentShaderDebug = pShader->ContainsFragment() && CompileGlslToSpirv(filename, fragmentGlsl, RHI::EShaderStage::Fragment, spirvFragmentByteCodeDebug, true);
	const bool bResultCompileComputeShaderDebug = pShader->ContainsCompute() && CompileGlslToSpirv(filename, computeGlsl, RHI::EShaderStage::Compute, spirvComputeByteCodeDebug, true);

	if ((bResultCompileVertexShaderDebug && bResultCompileFragmentShaderDebug) || bResultCompileComputeShaderDebug)
	{
		m_shaderCache.CacheSpirvWithDebugInfo(assetInfo->GetUID(), permutation, spirvVertexByteCodeDebug, spirvFragmentByteCodeDebug, spirvComputeByteCodeDebug);
	}

	return (bResultCompileVertexShader && bResultCompileFragmentShader) || bResultCompileComputeShader;
}

Tasks::TaskPtr<bool> ShaderCompiler::CompileAllPermutations(const UID& uid)
{
	return CompileAllPermutations(dynamic_cast<ShaderAssetInfoPtr>(App::GetSubmodule<AssetRegistry>()->GetAssetInfoPtr(uid)));
}

TWeakPtr<ShaderAsset> ShaderCompiler::LoadShaderAsset(const UID& uid)
{
	return LoadShaderAsset(dynamic_cast<ShaderAssetInfoPtr>(App::GetSubmodule<AssetRegistry>()->GetAssetInfoPtr(uid)));
}

Tasks::TaskPtr<bool> ShaderCompiler::CompileAllPermutations(ShaderAssetInfoPtr assetInfo)
{
	SAILOR_PROFILE_FUNCTION();
	if (TWeakPtr<ShaderAsset> pWeakShader = LoadShaderAsset(assetInfo))
	{
		UID assetUID = assetInfo->GetUID();

		TSharedPtr<ShaderAsset> pShader = pWeakShader.Lock();

		if ((!pShader->ContainsFragment() || !pShader->ContainsVertex()) && !pShader->ContainsCompute())
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
			Tasks::ITaskPtr job = scheduler->CreateTask("Compile shader", [i, pShader, assetInfo, permutationsToCompile]()
				{
					SAILOR_LOG("Start compiling shader %d", permutationsToCompile[i]);
			App::GetSubmodule<ShaderCompiler>()->ForceCompilePermutation(assetInfo, permutationsToCompile[i]);
				});

			saveCacheJob->Join(job);
			scheduler->Run(job);
		}
		scheduler->Run(saveCacheJob);

		return saveCacheJob;
	}
	else
	{
		SAILOR_LOG("Cannot find shader asset");
	}

	return Tasks::TaskPtr<bool>::Make(false);
}

TWeakPtr<ShaderAsset> ShaderCompiler::LoadShaderAsset(ShaderAssetInfoPtr shaderAssetInfo)
{
	SAILOR_PROFILE_FUNCTION();

	if (shaderAssetInfo)
	{
		UID uid = shaderAssetInfo->GetUID();
		if (const auto& loadedShader = m_shaderAssetsCache.Find(uid); loadedShader != m_shaderAssetsCache.end())
		{
			return loadedShader->m_second;
		}

		const std::string& filepath = shaderAssetInfo->GetAssetFilepath();

		std::string shaderText;
		AssetRegistry::ReadAllTextFile(filepath, shaderText);

		YAML::Node yamlNode = YAML::Load(shaderText);

		ShaderAsset* shader = new ShaderAsset();
		shader->Deserialize(yamlNode);

		return m_shaderAssetsCache[uid] = TSharedPtr<ShaderAsset>(shader);
	}

	SAILOR_LOG("Cannot find shader asset info!");
	return TWeakPtr<ShaderAsset>();
}

void ShaderCompiler::OnUpdateAssetInfo(AssetInfoPtr assetInfo, bool bWasExpired)
{
	if (bWasExpired)
	{
		const std::string extension = Utils::GetFileExtension(assetInfo->GetAssetFilepath());

		if (extension == "shader")
		{
			m_shaderAssetsCache.Remove(assetInfo->GetUID());

			auto compileTask = CompileAllPermutations(dynamic_cast<ShaderAssetInfoPtr>(assetInfo));
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
		else if (extension == "glsl")
		{
			TVector<AssetInfoPtr> shaderDeps;
			for (auto& shader : m_shaderAssetsCache)
			{
				if (shader.m_second->GetIncludes().Contains(assetInfo->GetRelativeAssetFilepath()))
				{
					shaderDeps.Add(App::GetSubmodule<AssetRegistry>()->GetAssetInfoPtr(shader.m_first));
				}
			}

			for (auto& shader : shaderDeps)
			{
				OnUpdateAssetInfo(shader, true);
			}
		}

		assetInfo->SaveMetaFile();
	}
}

void ShaderCompiler::OnImportAsset(AssetInfoPtr assetInfo)
{
	CompileAllPermutations(assetInfo->GetUID());
}

bool ShaderCompiler::CompileGlslToSpirv(const std::string& filename, const std::string& source, RHI::EShaderStage shaderStage, RHI::ShaderByteCode& outByteCode, bool bIsDebug)
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

	shaderc_shader_kind kind = shaderc_glsl_anyhit_shader;
	switch (shaderStage)
	{
	case RHI::EShaderStage::Fragment:
		kind = shaderc_glsl_fragment_shader;
		break;
	case RHI::EShaderStage::Vertex:
		kind = shaderc_glsl_vertex_shader;
		break;
	case RHI::EShaderStage::Compute:
		kind = shaderc_glsl_compute_shader;
		break;
	}

	/*
	for (const auto& define : defines)
	{
		options.AddMacroDefinition(define);
	}

	options.SetIncluder(std::make_unique<ShaderIncluder>());

	shaderc::PreprocessedSourceCompilationResult preCompilation = compiler.PreprocessGlsl(source, kind, source.c_str(), options);
	if (preCompilation.GetCompilationStatus() != shaderc_compilation_status_success)
	{
		const std::string& error = preCompilation.GetErrorMessage();
		std::cout << "Failed to precompile shader: " << error << std::endl;
		return false;
	}*/

	shaderc::SpvCompilationResult module = compiler.CompileGlslToSpv(source.c_str(), kind, filename.c_str(), "main", options);

	if (module.GetCompilationStatus() != shaderc_compilation_status::shaderc_compilation_status_success)
	{
		const size_t numErrors = module.GetNumErrors();
		const size_t numWarnings = module.GetNumWarnings();
		const std::string error = module.GetErrorMessage().c_str();
		
		SAILOR_LOG("Failed to compile shader:\n%s", error.c_str());
		
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

bool ShaderCompiler::GetSpirvCode(const UID& assetUID, const TVector<std::string>& defines, RHI::ShaderByteCode& outVertexByteCode, RHI::ShaderByteCode& outFragmentByteCode, RHI::ShaderByteCode& outComputeByteCode, bool bIsDebug)
{
	if (auto pShader = LoadShaderAsset(assetUID).Lock())
	{
		return GetSpirvCode(assetUID, GetPermutation(pShader->GetSupportedDefines(), defines), outVertexByteCode, outFragmentByteCode, outComputeByteCode, bIsDebug);
	}
	return false;
}

bool ShaderCompiler::GetSpirvCode(const UID& assetUID, uint32_t permutation, RHI::ShaderByteCode& outVertexByteCode, RHI::ShaderByteCode& outFragmentByteCode, RHI::ShaderByteCode& outComputeByteCode, bool bIsDebug)
{
	SAILOR_PROFILE_FUNCTION();

	if (ShaderAssetInfoPtr assetInfo = dynamic_cast<ShaderAssetInfoPtr>(App::GetSubmodule<AssetRegistry>()->GetAssetInfoPtr(assetUID)))
	{
		if (auto pShader = LoadShaderAsset(assetInfo).Lock())
		{
			bool bCompiledSuccesfully = true;
			if (m_shaderCache.IsExpired(assetUID, permutation))
			{
				bCompiledSuccesfully = ForceCompilePermutation(assetInfo, permutation);
			}

			return m_shaderCache.GetSpirvCode(assetUID, permutation, outVertexByteCode, outFragmentByteCode, outComputeByteCode, bIsDebug) && bCompiledSuccesfully;
		}
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
	return task->GetResult().IsValid();
}

bool ShaderCompiler::UpdateRHIResource(ShaderSetPtr pShader, uint32_t permutation)
{
	SAILOR_PROFILE_FUNCTION();

	auto pRaw = pShader.GetRawPtr();
	auto& pRhiDriver = App::GetSubmodule<RHI::Renderer>()->GetDriver();

	RHI::ShaderByteCode debugVertexSpirv;
	RHI::ShaderByteCode debugFragmentSpirv;
	RHI::ShaderByteCode debugComputeFragmentSpirv;
	if (!GetSpirvCode(pShader->GetUID(), permutation, debugVertexSpirv, debugFragmentSpirv, debugComputeFragmentSpirv, true))
	{
		return false;
	}

	RHI::ShaderByteCode vertexByteCode;
	RHI::ShaderByteCode fragmentByteCode;
	RHI::ShaderByteCode computeByteCode;
	if (!GetSpirvCode(pShader->GetUID(), permutation, vertexByteCode, fragmentByteCode, computeByteCode, false))
	{
		return false;
	}

	if (debugVertexSpirv.Num() > 0)
	{
		pRaw->m_rhiVertexShaderDebug = pRhiDriver->CreateShader(RHI::EShaderStage::Vertex, debugVertexSpirv);
	}
	if (debugFragmentSpirv.Num() > 0)
	{
		pRaw->m_rhiFragmentShaderDebug = pRhiDriver->CreateShader(RHI::EShaderStage::Fragment, debugFragmentSpirv);
	}
	if (debugComputeFragmentSpirv.Num() > 0)
	{
		pRaw->m_rhiComputeShaderDebug = pRhiDriver->CreateShader(RHI::EShaderStage::Compute, debugComputeFragmentSpirv);
	}

	if (vertexByteCode.Num() > 0)
	{
		pRaw->m_rhiVertexShader = pRhiDriver->CreateShader(RHI::EShaderStage::Vertex, vertexByteCode);
	}
	if (fragmentByteCode.Num() > 0)
	{
		pRaw->m_rhiFragmentShader = pRhiDriver->CreateShader(RHI::EShaderStage::Fragment, fragmentByteCode);
	}
	if (computeByteCode.Num() > 0)
	{
		pRaw->m_rhiComputeShader = pRhiDriver->CreateShader(RHI::EShaderStage::Compute, computeByteCode);
	}

	auto pShaderAsset = LoadShaderAsset(pShader->GetUID()).Lock();

	pRaw->m_colorAttachments = pShaderAsset->GetColorAttachments();
	pRaw->m_depthStencilAttachment = pShaderAsset->GetDepthStencilAttachment();

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
			if (!promise.m_second || !promise.m_second->IsFinished())
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