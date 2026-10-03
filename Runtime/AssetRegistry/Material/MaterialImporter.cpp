#include "AssetRegistry/Material/MaterialImporter.h"
#include "Memory/ObjectPtr.hpp"
#include "AssetRegistry/FileId.h"
#include "AssetRegistry/AssetRegistry.h"
#include "AssetRegistry/Texture/TextureImporter.h"
#include "MaterialAssetInfo.h"
#include "AssetRegistry/Shader/ShaderCompiler.h"
#include "Core/StringHash.h"
#include "Math/Math.h"
#include "Core/Utils.h"
#include "YamlExceptionBoundary.h"
#include "Engine/World.h"
#include "Memory/WeakPtr.hpp"
#include "Memory/ObjectAllocator.hpp"
#include <filesystem>
#include <fstream>
#include <algorithm>
#include <cstring>
#include <iostream>

#include "RHI/Renderer.h"
#include "RHI/Material.h"
#include "RHI/VertexDescription.h"
#include "RHI/Shader.h"
#include "RHI/Fence.h"
#include "RHI/CommandList.h"
#include "AssetRegistry/Texture/TextureImporter.h"

using namespace Sailor;

namespace
{
	std::atomic<uint64_t> g_materialContentRevision{};

	TVector<std::string> ResolveForwardDefines(const MaterialAsset& material)
	{
		auto defines = material.GetShaderDefines();
		auto shader = App::GetSubmodule<ShaderCompiler>()->LoadShaderAsset(material.GetShader()).TryLock();
		const auto tag = material.GetRenderState().GetTag();
		const bool forward = tag == "Opaque"_h.GetHash() || tag == "Masked"_h.GetHash() || tag == "Transparent"_h.GetHash();
		if (forward && shader && shader->GetSupportedDefines().Contains("MOTIONS") && !defines.Contains("MOTIONS"))
			defines.Add("MOTIONS");
		return defines;
	}

	bool IsBaseColorMetadataUniform(const std::string& name)
	{
		return name == "material.baseColorFactor" ||
			name == "material.albedo";
	}

	bool IsEmissiveUniform(const std::string& name)
	{
		return name == "material.emissiveFactor" ||
			name == "material.emissive" || name == "material.emission";
	}
}

uint64_t Material::GetGlobalContentRevision()
{
	return g_materialContentRevision.load(std::memory_order_acquire);
}

void Material::AdvanceContentRevision(bool bSurfaceChanged)
{
	if (bSurfaceChanged)
	{
		m_surfaceRevision.fetch_add(1, std::memory_order_release);
	}
	m_contentRevision.fetch_add(1, std::memory_order_release);
	g_materialContentRevision.fetch_add(1, std::memory_order_release);
}

void Material::AdvanceRenderMetadataRevision()
{
	m_renderMetadataRevision.fetch_add(1, std::memory_order_release);
}

void Material::SetShader(ShaderSetPtr shader)
{
	m_shader = shader;
	AdvanceContentRevision();
	AdvanceRenderMetadataRevision();
}

void Material::SetRenderState(const RHI::RenderState& renderState)
{
	m_renderState = renderState;
	AdvanceContentRevision();
	AdvanceRenderMetadataRevision();
}

bool Material::IsReady() const
{
	const bool bReady = m_shader && m_shader->IsReady() &&
		m_commonShaderBindings.IsValid() && m_commonShaderBindings->IsReady();
	if (bReady)
	{
		for (const auto& entry : m_rhiMaterials)
		{
			auto material = entry.m_second;
			if (material)
			{
				material->TryPublishPendingBindings();
			}
		}
	}
	return bReady;
}

MaterialPtr Material::CreateInstance(WorldPtr world, const MaterialPtr& material)
{
	auto allocator = world->GetAllocator();

	MaterialPtr newMaterial = MaterialPtr::Make(allocator, material->GetFileId());

	newMaterial->m_uniformsVec4 = material->m_uniformsVec4;
	newMaterial->m_uniformsFloat = material->m_uniformsFloat;
	newMaterial->m_renderState = material->GetRenderState();
	newMaterial->m_samplers = material->GetSamplers();
	newMaterial->m_shader = material->GetShader();
	newMaterial->m_bIsDirty = true;

	newMaterial->UpdateRHIResource();
	newMaterial->UpdateUniforms(world->GetCommandList());

	return newMaterial;
}

Tasks::ITaskPtr Material::OnHotReload()
{
	m_bIsDirty = true;

	auto updateRHI = Tasks::CreateTask("Update material RHI resource", [=, this]
		{
			// Dependency hot reload tasks are joined before this task executes, so
			// publish the revision only after their material-visible data is ready.
			AdvanceContentRevision();
			AdvanceRenderMetadataRevision();
			UpdateRHIResource();
			ForcelyUpdateUniforms();
		}, EThreadType::Render);

	return updateRHI;
}

void Material::ClearSamplers()
{
	SAILOR_PROFILE_FUNCTION();
	m_samplers.Clear();
	AdvanceContentRevision();
	AdvanceRenderMetadataRevision();
}

void Material::ClearUniforms()
{
	m_uniformsVec4.Clear();
	m_uniformsFloat.Clear();
	AdvanceContentRevision();
	AdvanceRenderMetadataRevision();
}

void Material::SetSampler(const std::string& name, TexturePtr value)
{
	SAILOR_PROFILE_FUNCTION();

	if (value)
	{
		m_samplers.At_Lock(name) = value;
		m_samplers.Unlock(name);

		m_bIsDirty = true;
		AdvanceContentRevision();
		AdvanceRenderMetadataRevision();
	}
}

void Material::SetUniform(const std::string& name, float value)
{
	SAILOR_PROFILE_FUNCTION();

	float* currentValue = nullptr;
	if (m_uniformsFloat.Find(name, currentValue) && currentValue && *currentValue == value)
	{
		return;
	}

	bool bRenderMetadataChanged = false;
	if (name == "material.alphaCutoff")
	{
		constexpr float defaultAlphaCutoff = 0.5f;
		const float currentAlphaCutoff = currentValue ? *currentValue : defaultAlphaCutoff;
		bRenderMetadataChanged = currentAlphaCutoff != value;
	}

	m_uniformsFloat.At_Lock(name) = value;
	m_uniformsFloat.Unlock(name);

	m_bIsDirty = true;
	AdvanceContentRevision();
	if (bRenderMetadataChanged)
	{
		AdvanceRenderMetadataRevision();
	}
}

void Material::SetUniform(const std::string& name, glm::vec4 value)
{
	SAILOR_PROFILE_FUNCTION();

	glm::vec4* currentValue = nullptr;
	if (m_uniformsVec4.Find(name, currentValue) && currentValue && *currentValue == value)
	{
		return;
	}

	auto resolveBaseColorAlpha = [this]()
	{
		glm::vec4* baseColor = nullptr;
		if (m_uniformsVec4.Find("material.baseColorFactor", baseColor) && baseColor)
		{
			return baseColor->a;
		}

		if (m_uniformsVec4.Find("material.albedo", baseColor) && baseColor)
		{
			return baseColor->a;
		}

		return 1.0f;
	};

	const bool bBaseColorMetadataUniform = IsBaseColorMetadataUniform(name);
	const float currentBaseColorAlpha = bBaseColorMetadataUniform ? resolveBaseColorAlpha() : 1.0f;

	m_uniformsVec4.At_Lock(name) = value;
	m_uniformsVec4.Unlock(name);

	m_bIsDirty = true;
	AdvanceContentRevision(!IsEmissiveUniform(name));
	if (bBaseColorMetadataUniform && currentBaseColorAlpha != resolveBaseColorAlpha())
	{
		AdvanceRenderMetadataRevision();
	}
}

RHI::RHIMaterialPtr Material::GetOrAddRHI(RHI::RHIVertexDescriptionPtr vertexDescription)
{
	SAILOR_PROFILE_FUNCTION();

	SAILOR_PROFILE_BLOCK("Achieve exclusive access to rhi"_h);
	// TODO: Resolve collisions of VertexAttributeBits
	RHI::RHIMaterialPtr& material = m_rhiMaterials.At_Lock(vertexDescription->GetVertexAttributeBits());
	m_rhiMaterials.Unlock(vertexDescription->GetVertexAttributeBits());
	SAILOR_PROFILE_END_BLOCK("Achieve exclusive access to rhi"_h);

	if (!material)
	{
		SAILOR_PROFILE_SCOPE("Create RHI material for resource");

		if (!m_commonShaderBindings)
		{
			if ((material = RHI::Renderer::GetDriver()->CreateMaterial(vertexDescription, RHI::EPrimitiveTopology::TriangleList, m_renderState, m_shader)))
			{
				m_commonShaderBindings = material->GetBindings();
			}
			else
			{
				SAILOR_LOG_ERROR("Cannot create RHI material %s for vertex attribute identity %llu.",
					GetFileId().ToString().c_str(),
					static_cast<unsigned long long>(vertexDescription->GetVertexAttributeBits()));
				return nullptr;
			}
		}
		else
		{
			material = RHI::Renderer::GetDriver()->CreateMaterial(vertexDescription, RHI::EPrimitiveTopology::TriangleList, m_renderState, m_shader, m_commonShaderBindings);
		}

		m_commonShaderBindings->RecalculateCompatibility();
	}

	return material;
}

void Material::UpdateRHIResource()
{
	SAILOR_PROFILE_FUNCTION();

	//SAILOR_LOG("Update material RHI resource: %s", GetFileId().ToString().c_str());

	// All RHI materials are outdated now
	m_rhiMaterials.Clear();
	m_commonShaderBindings.Clear();

	// Create base material
	if (!GetOrAddRHI(RHI::Renderer::GetDriver()->GetOrAddVertexDescription<RHI::VertexP3N3T3B3UV2C4I4W4>()))
	{
		m_bIsDirty = true;
		SAILOR_LOG_ERROR("Cannot update RHI resource for material %s.", GetFileId().ToString().c_str());
		return;
	}

	{
		SAILOR_PROFILE_SCOPE("Update samplers");
		for (auto& sampler : m_samplers)
		{
			// The sampler could be bound directly to 'sampler2D' by its name
			if (m_commonShaderBindings->HasBinding(sampler.m_first))
			{
				RHI::Renderer::GetDriver()->UpdateShaderBinding(m_commonShaderBindings, sampler.m_first, sampler.m_second->GetRHI());
			}

			const std::string parameterName = "material." + sampler.m_first;

			// Also the sampler could be bound by texture array, by its name
			if (m_commonShaderBindings->HasParameter(parameterName))
			{
				std::string outBinding;
				std::string outVariable;

				RHI::RHIShaderBindingSet::ParseParameter(parameterName, outBinding, outVariable);
				m_commonShaderBindings->GetOrAddShaderBinding(outBinding);
			}
		}
	}

	{
		SAILOR_PROFILE_SCOPE("Update shader bindings");
		// Create all bindings first
		// TODO: Remove boilerplate
		for (auto& uniform : m_uniformsVec4)
		{
			if (m_commonShaderBindings->HasParameter(uniform.m_first))
			{
				std::string outBinding;
				std::string outVariable;

				RHI::RHIShaderBindingSet::ParseParameter(uniform.m_first, outBinding, outVariable);
				m_commonShaderBindings->GetOrAddShaderBinding(outBinding);
			}
		}

		for (auto& uniform : m_uniformsFloat)
		{
			if (m_commonShaderBindings->HasParameter(uniform.m_first))
			{
				std::string outBinding;
				std::string outVariable;

				RHI::RHIShaderBindingSet::ParseParameter(uniform.m_first, outBinding, outVariable);
				m_commonShaderBindings->GetOrAddShaderBinding(outBinding);
			}
		}
	}

	m_commonShaderBindings->RecalculateCompatibility();
	m_bIsDirty = false;
}

void Material::UpdateRHIResourceAndUniforms()
{
	UpdateRHIResource();
	ForcelyUpdateUniforms();
}

void Material::SynchronizeUniformValues(const Material& source)
{
	m_uniformsVec4 = source.m_uniformsVec4;
	m_uniformsFloat = source.m_uniformsFloat;
	m_bIsDirty = true;
	AdvanceContentRevision();
	ForcelyUpdateUniforms();
}

void Material::UpdateUniforms(RHI::RHICommandListPtr cmdList)
{
	if (!m_commonShaderBindings)
	{
		return;
	}

	TMap<std::string, TVector<uint8_t>> bindingData;

	auto writeParameter = [this, &bindingData](
		const std::string& parameterName,
		const void* value,
		size_t valueSize)
		{
			if (!m_commonShaderBindings->HasParameter(parameterName))
			{
				return;
			}

			std::string bindingName;
			std::string variableName;
			RHI::RHIShaderBindingSet::ParseParameter(
				parameterName,
				bindingName,
				variableName);

			RHI::RHIShaderBindingPtr& binding =
				m_commonShaderBindings->GetOrAddShaderBinding(bindingName);
			RHI::ShaderLayoutBindingMember memberLayout;
			if (!binding->FindVariableInUniformBuffer(
				variableName,
				memberLayout))
			{
				return;
			}

			const size_t bindingSize = (std::max)(
				static_cast<size_t>(binding->GetLayout().m_size),
				static_cast<size_t>(binding->GetLayout().m_paddedSize));
			if (bindingSize == 0 ||
				value == nullptr ||
				valueSize > memberLayout.m_size ||
				memberLayout.m_absoluteOffset > bindingSize ||
				valueSize > bindingSize - memberLayout.m_absoluteOffset)
			{
				ensure(false,
					"Cannot pack material parameter %s",
					parameterName.c_str());
				return;
			}

			TVector<uint8_t>& data = bindingData[bindingName];
			if (data.IsEmpty())
			{
				// Value-initialize the complete reflected block. Optional material
				// fields which are absent from the asset must remain deterministic
				// zeros instead of retaining data from a recycled SSBO allocation.
				data.Resize(bindingSize);
			}

			std::memcpy(
				data.GetData() + memberLayout.m_absoluteOffset,
				value,
				valueSize);
		};

	for (auto& uniform : m_uniformsVec4)
	{
		const glm::vec4 value = uniform.m_second;
		writeParameter(uniform.m_first, &value, sizeof(value));
	}

	for (auto& uniform : m_uniformsFloat)
	{
		const float value = uniform.m_second;
		writeParameter(uniform.m_first, &value, sizeof(value));
	}

	for (auto& sampler : m_samplers)
	{
		const std::string parameterName = "material." + sampler.m_first;
		const uint32_t value = static_cast<uint32_t>(
			App::GetSubmodule<TextureImporter>()->GetTextureIndex(
				sampler.m_second->GetFileId()));
		writeParameter(parameterName, &value, sizeof(value));
	}

	for (const auto& data : bindingData)
	{
		RHI::RHIShaderBindingPtr& binding =
			m_commonShaderBindings->GetOrAddShaderBinding(data.m_first);
		RHI::Renderer::GetDriverCommands()->UpdateShaderBinding(
			cmdList,
			binding,
			data.m_second->GetData(),
			data.m_second->Num());
	}
}

void Material::ForcelyUpdateUniforms()
{
	if (!m_commonShaderBindings)
	{
		return;
	}
	auto nextBindings = RHI::Renderer::GetDriver()->CloneMaterialShaderBindings(
		m_commonShaderBindings);
	if (!nextBindings)
	{
		m_bIsDirty = true;
		return;
	}
	m_commonShaderBindings = std::move(nextBindings);

	RHI::RHICommandListPtr cmdList;
	{
		SAILOR_PROFILE_SCOPE("Create command list");

		cmdList = RHI::Renderer::GetDriver()->CreateCommandList(false, RHI::ECommandListQueue::Transfer);
		RHI::Renderer::GetDriver()->SetDebugName(cmdList, "Forcely Update Uniforms");
		RHI::Renderer::GetDriverCommands()->BeginCommandList(cmdList, true);
		UpdateUniforms(cmdList);
		RHI::Renderer::GetDriverCommands()->EndCommandList(cmdList);
	}

	// Create fences to track the state of material update
	RHI::RHIFencePtr fence = RHI::RHIFencePtr::Make();
	RHI::Renderer::GetDriver()->SetDebugName(fence, std::format("Forcely update uniforms"));

	RHI::Renderer::GetDriver()->TrackDelayedInitialization(m_commonShaderBindings.GetRawPtr(), fence);
	for (const auto& entry : m_rhiMaterials)
	{
		auto material = entry.m_second;
		if (material)
		{
			material->StageBindings(m_commonShaderBindings);
		}
	}

	// Submit cmd lists
	SAILOR_ENQUEUE_TASK_RENDER_THREAD("Update shader bindings set rhi",
		([cmdList, fence]()
			{
				RHI::Renderer::GetDriver()->SubmitCommandList(cmdList, fence);
			}));
}

YAML::Node MaterialAsset::Serialize() const
{
	return Serialize(*m_pData);
}

YAML::Node MaterialAsset::Serialize(const Data& data)
{
	YAML::Node outData;

	::Serialize(outData, "bEnableDepthTest", data.m_renderState.IsDepthTestEnabled());
	::Serialize(outData, "bEnableZWrite", data.m_renderState.IsEnabledZWrite());
	::Serialize(outData, "bSupportMultisampling", data.m_renderState.SupportMultisampling());
	::Serialize(outData, "bCustomDepthShader", data.m_renderState.IsRequiredCustomDepthShader());
	::Serialize(outData, "depthBias", data.m_renderState.GetDepthBias());
	::Serialize(outData, "cullMode", data.m_renderState.GetCullMode());
	::Serialize(outData, "fillMode", data.m_renderState.GetFillMode());
	::Serialize(outData, "blendMode", data.m_renderState.GetBlendMode());
	::Serialize(outData, "defines", data.m_shaderDefines);
	::Serialize(outData, "samplers", data.m_samplers);
	::Serialize(outData, "uniformsVec4", data.m_uniformsVec4);
	::Serialize(outData, "uniformsFloat", data.m_uniformsFloat);
	::Serialize(outData, "shaderUid", data.m_shader);
	::Serialize(outData, "renderQueue", data.m_renderQueue);

	return outData;
}

void MaterialAsset::Deserialize(const YAML::Node& outData)
{
	m_pData = TUniquePtr<Data>::Make();

	bool bEnableDepthTest = true;
	bool bEnableZWrite = true;
	bool bSupportMultisampling = true;
	bool bCustomDepthShader = false;
	float depthBias = 0.0f;
	RHI::ECullMode cullMode = RHI::ECullMode::Back;
	RHI::EBlendMode blendMode = RHI::EBlendMode::None;
	RHI::EFillMode fillMode = RHI::EFillMode::Fill;
	std::string renderQueue = "Opaque";

	m_pData->m_shaderDefines.Clear();
	m_pData->m_uniformsVec4.Clear();
	m_pData->m_uniformsFloat.Clear();

	::Deserialize(outData, "bEnableDepthTest", bEnableDepthTest);
	::Deserialize(outData, "bEnableZWrite", bEnableZWrite);
	::Deserialize(outData, "bCustomDepthShader", bCustomDepthShader);
	::Deserialize(outData, "bSupportMultisampling", bSupportMultisampling);
	::Deserialize(outData, "depthBias", depthBias);
	::Deserialize(outData, "cullMode", cullMode);
	::Deserialize(outData, "fillMode", fillMode);
	::Deserialize(outData, "blendMode", blendMode);
	::Deserialize(outData, "defines", m_pData->m_shaderDefines);
	::Deserialize(outData, "samplers", m_pData->m_samplers);
	::Deserialize(outData, "uniformsVec4", m_pData->m_uniformsVec4);
	::Deserialize(outData, "uniformsFloat", m_pData->m_uniformsFloat);
	::Deserialize(outData, "shaderUid", m_pData->m_shader);
	::Deserialize(outData, "renderQueue", renderQueue);

	m_pData->m_renderQueue = renderQueue;
	const size_t tag = StringHash::Runtime(renderQueue).GetHash();
	m_pData->m_renderState = RHI::RenderState(bEnableDepthTest, bEnableZWrite, depthBias, bCustomDepthShader, cullMode, blendMode, fillMode, tag, bSupportMultisampling);
}

MaterialImporter::MaterialImporter(MaterialAssetInfoHandler* infoHandler)
{
	SAILOR_PROFILE_FUNCTION();
	m_allocator = ObjectAllocatorPtr::Make(EAllocationPolicy::SharedMemory_MultiThreaded);
	infoHandler->Subscribe(this);
}

MaterialImporter::~MaterialImporter()
{
	for (auto& instance : m_loadedMaterials)
	{
		instance.m_second.DestroyObject(m_allocator);
	}
}

void MaterialImporter::OnImportAsset(AssetInfoPtr assetInfo)
{
}

void MaterialImporter::OnUpdateAssetInfo(AssetInfoPtr assetInfo, bool bWasExpired)
{
	SAILOR_PROFILE_FUNCTION();
	if (!bWasExpired)
	{
		return;
	}

	const auto uid = assetInfo->GetFileId();
	auto material = GetLoadedMaterial(uid);
	if (!material)
	{
		return;
	}
	auto* registry = App::GetSubmodule<AssetRegistry>();
	const auto token = registry->BeginAssetProcessing(assetInfo);
	if (!token)
	{
		return;
	}
	auto asset = LoadMaterialAsset(uid);
	if (!asset)
	{
		registry->CompleteAssetProcessing(token, false);
		return;
	}

	auto& promise = m_promises.At_Lock(uid, nullptr);
	promise = CreateMaterialTask(material, asset, true, promise);
	auto task = promise;
	m_promises.Unlock(uid);
	auto acknowledge = Tasks::CreateTask<bool>("Acknowledge material reload", [registry, token, task]()
		{
			const bool succeeded = task->GetResult().IsValid();
			registry->CompleteAssetProcessing(token, succeeded);
			return succeeded;
		});
	acknowledge->Join(task);
	registry->TrackScanProcessingTask(acknowledge);
	task->Run();
	acknowledge->Run();
}

bool MaterialImporter::IsMaterialLoaded(FileId uid) const
{
	return m_loadedMaterials.ContainsKey(uid);
}

TSharedPtr<MaterialAsset> MaterialImporter::LoadMaterialAsset(FileId uid)
{
	SAILOR_PROFILE_FUNCTION();

	if (MaterialAssetInfoPtr materialAssetInfo = dynamic_cast<MaterialAssetInfoPtr>(App::GetSubmodule<AssetRegistry>()->GetAssetInfoPtr(uid)))
	{
		SAILOR_PROFILE_TEXT(materialAssetInfo->GetAssetFilepath().c_str());

		const std::string& filepath = materialAssetInfo->GetAssetFilepath();

		std::string materialYaml;
		if (!AssetRegistry::ReadAllTextFile(filepath, materialYaml))
		{
			SAILOR_LOG_ERROR("Cannot read material YAML '%s'.", filepath.c_str());
			return TSharedPtr<MaterialAsset>();
		}

		YAML::Node yamlNode;
		std::string yamlDiagnostic;
		if (!External::TryLoadYaml(materialYaml, yamlNode, yamlDiagnostic))
		{
			SAILOR_LOG_ERROR("Cannot parse material YAML '%s': %s", filepath.c_str(), yamlDiagnostic.c_str());
			return TSharedPtr<MaterialAsset>();
		}

		auto material = TSharedPtr<MaterialAsset>::Make();
		if (!External::GuardYamlExceptions(
				[&material, &yamlNode]()
				{
					material->Deserialize(yamlNode);
				},
				yamlDiagnostic))
		{
			SAILOR_LOG_ERROR("Cannot deserialize material YAML '%s': %s", filepath.c_str(), yamlDiagnostic.c_str());
			return TSharedPtr<MaterialAsset>();
		}

		return material;
	}

	SAILOR_LOG("Cannot find material asset info with FileId: %s", uid.ToString().c_str());
	return TSharedPtr<MaterialAsset>();
}

const FileId& MaterialImporter::CreateMaterialAsset(const std::string& assetFilepath, MaterialAsset::Data data)
{
	if (auto shader = App::GetSubmodule<ShaderCompiler>()->LoadShaderAsset(data.m_shader).TryLock())
	{
		for (const auto& entry : shader->GetDefaultUniformsVec4())
		{
			if (!data.m_uniformsVec4.ContainsKey(entry.m_first))
				data.m_uniformsVec4.Add(entry.m_first, *entry.m_second);
		}
		for (const auto& entry : shader->GetDefaultUniformsFloat())
		{
			if (!data.m_uniformsFloat.ContainsKey(entry.m_first))
				data.m_uniformsFloat.Add(entry.m_first, *entry.m_second);
		}
	}

	MaterialAsset asset;
	asset.m_pData = TUniquePtr<MaterialAsset::Data>::Make(std::move(data));

	YAML::Node newMaterial = asset.Serialize();

	std::ofstream assetFile(assetFilepath);

	assetFile << newMaterial;
	assetFile.close();

	return App::GetSubmodule<AssetRegistry>()->GetOrLoadFile(assetFilepath);
}

bool MaterialImporter::LoadMaterial_Immediate(FileId uid, MaterialPtr& outMaterial)
{
	SAILOR_PROFILE_FUNCTION();
	auto task = LoadMaterial(uid, outMaterial);
	if (!task)
	{
		return false;
	}
	task->Wait();

	return task->GetResult().IsValid();
}

MaterialPtr MaterialImporter::GetLoadedMaterial(FileId uid)
{
	MaterialPtr material;
	m_loadedMaterials.TryGet(uid, material);
	return material;
}

Tasks::TaskPtr<MaterialPtr> MaterialImporter::GetLoadPromise(FileId uid)
{
	Tasks::TaskPtr<MaterialPtr> promise;
	m_promises.TryGet(uid, promise);
	return promise;
}

Tasks::TaskPtr<MaterialPtr> MaterialImporter::CreateMaterialTask(
	MaterialPtr material, TSharedPtr<MaterialAsset> asset, bool bHotReload,
	const Tasks::ITaskPtr& previous)
{
	ShaderSetPtr shader;
	auto loadShader = App::GetSubmodule<ShaderCompiler>()->LoadShader(
		asset->GetShader(), shader, ResolveForwardDefines(*asset));
	TVector<TPair<std::string, Tasks::TaskPtr<TexturePtr>>> samplers;
	for (const auto& sampler : asset->GetSamplers())
	{
		if (*sampler.m_second)
		{
			TexturePtr texture;
			samplers.Emplace(sampler.m_first,
				App::GetSubmodule<TextureImporter>()->LoadTexture(*sampler.m_second, texture));
		}
	}
	const std::string filename = App::GetSubmodule<AssetRegistry>()->GetAssetInfoPtr(
		material->GetFileId())->GetAssetFilepath();

	auto publish = Tasks::CreateTask<MaterialPtr>("Publish material",
		[material, asset, loadShader, samplers, filename, bHotReload]() mutable
		{
			auto shader = loadShader ? loadShader->GetResult() : ShaderSetPtr{};
			if (!shader || !shader->IsReady())
			{
				SAILOR_LOG_ERROR("Cannot load material '%s': shader loading failed.", filename.c_str());
				return MaterialPtr{};
			}

			Material prepared(material->GetFileId());
			prepared.m_shader = shader;
			prepared.m_renderState = asset->GetRenderState();
			for (const auto& sampler : samplers)
			{
				auto texture = sampler.m_second ? sampler.m_second->GetResult() : TexturePtr{};
				if (!texture || !texture->GetRHI())
				{
					SAILOR_LOG_ERROR("Cannot load material '%s': texture '%s' failed.",
						filename.c_str(), sampler.m_first.c_str());
					return MaterialPtr{};
				}
				prepared.m_samplers.Insert(sampler.m_first, texture);
			}
			for (const auto& uniform : asset->GetUniformsVec4())
			{
				prepared.m_uniformsVec4.Insert(uniform.m_first, *uniform.m_second);
			}
			for (const auto& uniform : asset->GetUniformsFloat())
			{
				prepared.m_uniformsFloat.Insert(uniform.m_first, *uniform.m_second);
			}

			prepared.UpdateRHIResourceAndUniforms();
			if (prepared.IsDirty() || !prepared.m_commonShaderBindings)
			{
				SAILOR_LOG_ERROR("Cannot create material '%s'.", filename.c_str());
				return MaterialPtr{};
			}

			// Keep the last-good values and dependencies until the replacement is built.
			if (material->m_shader)
			{
				material->m_shader->RemoveHotReloadDependentObject(material);
			}
			for (auto& sampler : material->m_samplers)
			{
				sampler.m_second->RemoveHotReloadDependentObject(material);
			}
			material->m_shader = std::move(prepared.m_shader);
			material->m_renderState = prepared.m_renderState;
			material->m_samplers = std::move(prepared.m_samplers);
			material->m_uniformsVec4 = std::move(prepared.m_uniformsVec4);
			material->m_uniformsFloat = std::move(prepared.m_uniformsFloat);
			material->m_commonShaderBindings = std::move(prepared.m_commonShaderBindings);
			material->m_rhiMaterials = std::move(prepared.m_rhiMaterials);
			material->m_bIsDirty = false;
			material->AdvanceContentRevision();
			material->AdvanceRenderMetadataRevision();

			material->m_shader->AddHotReloadDependentObject(material);
			for (auto& sampler : material->m_samplers)
			{
				sampler.m_second->AddHotReloadDependentObject(material);
			}
			for (const auto& entry : material->m_rhiMaterials)
			{
				RHI::Renderer::GetDriver()->SetDebugName(entry.m_second, filename);
			}
			if (bHotReload)
			{
				material->TraceHotReloadDependents(nullptr);
			}
			return material;
		}, EThreadType::Render);
	publish->Join(loadShader);
	for (const auto& sampler : samplers)
	{
		publish->Join(sampler.m_second);
	}
	publish->Join(previous);
	return publish;
}

Tasks::TaskPtr<MaterialPtr> MaterialImporter::LoadMaterial(FileId uid, MaterialPtr& outMaterial)
{
	SAILOR_PROFILE_FUNCTION();
	auto& promise = m_promises.At_Lock(uid, nullptr);
	auto& material = m_loadedMaterials.At_Lock(uid, MaterialPtr{});
	if (promise && !promise->IsFinished())
	{
		outMaterial = material;
		auto task = promise;
		m_loadedMaterials.Unlock(uid);
		m_promises.Unlock(uid);
		return task;
	}
	if (material && material->GetShaderBindings())
	{
		outMaterial = material;
		auto task = Tasks::TaskPtr<MaterialPtr>::Make(material);
		m_loadedMaterials.Unlock(uid);
		m_promises.Unlock(uid);
		return task;
	}

	auto asset = LoadMaterialAsset(uid);
	if (!asset)
	{
		outMaterial = nullptr;
		m_loadedMaterials.Unlock(uid);
		m_promises.Unlock(uid);
		return {};
	}
	if (!material)
	{
		material = MaterialPtr::Make(m_allocator, uid);
	}
	promise = CreateMaterialTask(material, asset, false, promise);
	outMaterial = material;
	auto task = promise;
	m_loadedMaterials.Unlock(uid);
	m_promises.Unlock(uid);
	task->Run();
	return task;
}

bool MaterialImporter::LoadAsset(FileId uid, TObjectPtr<Object>& out, bool bImmediate)
{
	MaterialPtr outAsset;
	if (bImmediate)
	{
		bool bRes = LoadMaterial_Immediate(uid, outAsset);
		out = outAsset;
		return bRes;
	}

	auto task = LoadMaterial(uid, outAsset);
	out = outAsset;
	return task.IsValid();
}

void MaterialImporter::CollectGarbage()
{
	m_promises.LockAll();
	auto ids = m_promises.GetKeys();
	m_promises.UnlockAll();

	for (const auto& id : ids)
	{
		auto promise = m_promises.At_Lock(id);
		if (!promise || promise->IsFinished())
		{
			m_promises.ForcelyRemove(id);
		}
		m_promises.Unlock(id);
	}
}
