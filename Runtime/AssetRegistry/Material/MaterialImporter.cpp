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
#include <initializer_list>
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
	constexpr const char* StandardGltfShaderUid =
		"1A4BA353-FDA4-4F65-941F-D9FFEE4630A0";

	bool IsBaseColorMetadataUniform(const std::string& name)
	{
		return name == "material.baseColorFactor" ||
			name == "material.albedo";
	}

	template<typename TValue>
	void AddMaterialUniformAliasOrDefault(
		TMap<std::string, TValue>& uniforms,
		const char* canonicalName,
		std::initializer_list<const char*> aliases,
		const TValue& defaultValue)
	{
		if (uniforms.ContainsKey(canonicalName))
		{
			return;
		}

		for (const char* alias : aliases)
		{
			const TValue* value = nullptr;
			if (uniforms.Find(alias, value) && value)
			{
				uniforms.Add(canonicalName, *value);
				return;
			}
		}

		uniforms.Add(canonicalName, defaultValue);
	}

	void AddMaterialSamplerAlias(
		TMap<std::string, FileId>& samplers,
		const char* canonicalName,
		std::initializer_list<const char*> aliases)
	{
		if (samplers.ContainsKey(canonicalName))
		{
			return;
		}

		for (const char* alias : aliases)
		{
			const FileId* value = nullptr;
			if (samplers.Find(alias, value) && value && *value)
			{
				samplers.Add(canonicalName, *value);
				return;
			}
		}
	}

	void NormalizeStandardGltfMaterial(MaterialAsset::Data& data)
	{
		if (data.m_shader.ToString() != StandardGltfShaderUid)
		{
			return;
		}

		// Standard_glTF is the engine's default surface shader. Older engine
		// materials used the Standard names below, so silently zeroing the new
		// reflected block made otherwise valid textureless materials black and
		// perfectly smooth. Keep the authored values and provide neutral PBR
		// defaults for genuinely absent fields.
		AddMaterialUniformAliasOrDefault(
			data.m_uniformsVec4,
			"material.baseColorFactor",
			{ "material.albedo" },
			glm::vec4(1.0f));
		AddMaterialUniformAliasOrDefault(
			data.m_uniformsVec4,
			"material.emissiveFactor",
			{ "material.emissive", "material.emission" },
			glm::vec4(0.0f));
		AddMaterialUniformAliasOrDefault(
			data.m_uniformsFloat,
			"material.roughnessFactor",
			{ "material.roughness" },
			1.0f);
		AddMaterialUniformAliasOrDefault(
			data.m_uniformsFloat,
			"material.metallicFactor",
			{ "material.metallic" },
			0.0f);
		AddMaterialUniformAliasOrDefault(
			data.m_uniformsFloat,
			"material.normalScale",
			{},
			1.0f);
		AddMaterialUniformAliasOrDefault(
			data.m_uniformsFloat,
			"material.alphaCutoff",
			{},
			0.5f);
		AddMaterialUniformAliasOrDefault(
			data.m_uniformsFloat,
			"material.occlusionStrength",
			{},
			1.0f);
		AddMaterialSamplerAlias(
			data.m_samplers,
			"baseColorSampler",
			{ "albedoSampler" });
	}
}

uint64_t Material::GetGlobalContentRevision()
{
	return g_materialContentRevision.load(std::memory_order_acquire);
}

void Material::AdvanceContentRevision()
{
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
	AdvanceContentRevision();
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
	YAML::Node outData;

	::Serialize(outData, "bEnableDepthTest", m_pData->m_renderState.IsDepthTestEnabled());
	::Serialize(outData, "bEnableZWrite", m_pData->m_renderState.IsEnabledZWrite());
	::Serialize(outData, "bSupportMultisampling", m_pData->m_renderState.SupportMultisampling());
	::Serialize(outData, "bCustomDepthShader", m_pData->m_renderState.IsRequiredCustomDepthShader());
	::Serialize(outData, "depthBias", m_pData->m_renderState.GetDepthBias());
	::Serialize(outData, "cullMode", m_pData->m_renderState.GetCullMode());
	::Serialize(outData, "fillMode", m_pData->m_renderState.GetFillMode());
	::Serialize(outData, "blendMode", m_pData->m_renderState.GetBlendMode());
	::Serialize(outData, "defines", m_pData->m_shaderDefines);
	::Serialize(outData, "samplers", m_pData->m_samplers);
	::Serialize(outData, "uniformsVec4", m_pData->m_uniformsVec4);
	::Serialize(outData, "uniformsFloat", m_pData->m_uniformsFloat);
	::Serialize(outData, "shaderUid", m_pData->m_shader);
	::Serialize(outData, "renderQueue", GetRenderQueue());

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
	NormalizeStandardGltfMaterial(*m_pData);

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
	SAILOR_PROFILE_TEXT(assetInfo->GetAssetFilepath().c_str());

	MaterialPtr material = GetLoadedMaterial(assetInfo->GetFileId());
	if (bWasExpired && material)
	{
		// We need to start load the material
		if (auto pMaterialAsset = LoadMaterialAsset(assetInfo->GetFileId()))
		{
			auto updateMaterial = Tasks::CreateTask("Update Material", [=]() mutable
				{
					auto pMaterial = material;

					pMaterial->GetShader()->RemoveHotReloadDependentObject(material);
					for (auto& sampler : pMaterial->m_samplers)
					{
						if (sampler.m_second)
						{
							sampler.m_second->RemoveHotReloadDependentObject(material);
						}
					}
					pMaterial->ClearSamplers();
					pMaterial->ClearUniforms();

					ShaderSetPtr pShader;
					auto pLoadShader = App::GetSubmodule<ShaderCompiler>()->LoadShader(pMaterialAsset->GetShader(), pShader, pMaterialAsset->GetShaderDefines());

					pMaterial->SetRenderState(pMaterialAsset->GetRenderState());

					pMaterial->SetShader(pShader);
					pShader->AddHotReloadDependentObject(material);

					const FileId uid = pMaterial->GetFileId();
					const string assetFilename = App::GetSubmodule<AssetRegistry>()->GetAssetInfoPtr(uid)->GetAssetFilepath();

					auto updateRHI = Tasks::CreateTask("Update material RHI resource", [=]() mutable
						{
							if (pMaterial->GetShader()->IsReady())
							{
								pMaterial->UpdateRHIResource();
								pMaterial->ForcelyUpdateUniforms();
								pMaterial->TraceHotReload(nullptr);

								// TODO: Optimize
								auto rhiMaterials = pMaterial->GetRHIMaterials().GetValues();
								for (const auto& rhi : rhiMaterials)
								{
									RHI::Renderer::GetDriver()->SetDebugName(rhi, assetFilename);
								}
							}
						}, EThreadType::Render);

					// Preload textures
					for (const auto& sampler : pMaterialAsset->GetSamplers())
					{
						TexturePtr texture;

						if (auto loadTextureTask = App::GetSubmodule<TextureImporter>()->LoadTexture(*sampler.m_second, texture))
						{
							auto updateSampler = loadTextureTask->Then(
								[=](TexturePtr pTexture) mutable
								{
									if (pTexture)
									{
										pMaterial->SetSampler(sampler.m_first, texture);
										pTexture->AddHotReloadDependentObject(material);
									}
								}, "Set material texture binding", EThreadType::Render);

							updateRHI->Join(updateSampler);
						}
					}

					for (const auto& uniform : pMaterialAsset->GetUniformsVec4())
					{
						pMaterial->SetUniform(uniform.m_first, *uniform.m_second);
					}

					for (const auto& uniform : pMaterialAsset->GetUniformsFloat())
					{
						pMaterial->SetUniform(uniform.m_first, *uniform.m_second);
					}

					updateRHI->Join(pLoadShader);
					updateRHI->Run();
				});

			if (auto promise = GetLoadPromise(assetInfo->GetFileId()))
			{
				updateMaterial->Join(promise);
			}

			updateMaterial->Run();
		}
	}
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
	task->Wait();

	return task->GetResult().IsValid();
}

MaterialPtr MaterialImporter::GetLoadedMaterial(FileId uid)
{
	// Check loaded materials
	auto materialIt = m_loadedMaterials.Find(uid);
	if (materialIt != m_loadedMaterials.end())
	{
		return (*materialIt).m_second;
	}
	return MaterialPtr();
}

Tasks::TaskPtr<MaterialPtr> MaterialImporter::GetLoadPromise(FileId uid)
{
	auto it = m_promises.Find(uid);
	if (it != m_promises.end())
	{
		return (*it).m_second;
	}

	return Tasks::TaskPtr<MaterialPtr>();
}

Tasks::TaskPtr<MaterialPtr> MaterialImporter::LoadMaterial(FileId uid, MaterialPtr& outMaterial)
{
	SAILOR_PROFILE_FUNCTION();

	// Check promises first
	auto& promise = m_promises.At_Lock(uid, nullptr);
	auto& loadedMaterial = m_loadedMaterials.At_Lock(uid, MaterialPtr());

	// Check loaded assets
	if (loadedMaterial)
	{
		outMaterial = loadedMaterial;
		auto res = promise ? promise : Tasks::TaskPtr<MaterialPtr>::Make(outMaterial);

		m_loadedMaterials.Unlock(uid);
		m_promises.Unlock(uid);

		return res;
	}

	// We need to start load the material
	if (auto pMaterialAsset = LoadMaterialAsset(uid))
	{
		MaterialPtr pMaterial = MaterialPtr::Make(m_allocator, uid);

		ShaderSetPtr pShader;
		auto pLoadShader = App::GetSubmodule<ShaderCompiler>()->LoadShader(pMaterialAsset->GetShader(), pShader, pMaterialAsset->GetShaderDefines());

		pMaterial->SetRenderState(pMaterialAsset->GetRenderState());
		pMaterial->SetShader(pShader);
		pShader->AddHotReloadDependentObject(pMaterial);

		const FileId uid = pMaterial->GetFileId();
		const string assetFilename = App::GetSubmodule<AssetRegistry>()->GetAssetInfoPtr(uid)->GetAssetFilepath();

		promise = Tasks::CreateTaskWithResult<MaterialPtr>("Load material RHI resource",
			[pMaterial, assetFilename]() mutable
			{
				if (pMaterial->GetShader()->IsReady())
				{
					pMaterial->UpdateRHIResource();
					pMaterial->ForcelyUpdateUniforms();

					// TODO: Optimize
					auto rhiMaterials = pMaterial->GetRHIMaterials().GetValues();
					for (const auto& rhi : rhiMaterials)
					{
						RHI::Renderer::GetDriver()->SetDebugName(rhi, assetFilename);
					}
				}

				return pMaterial;
			}, EThreadType::RHI);

		// Preload textures
		for (const auto& sampler : pMaterialAsset->GetSamplers())
		{
			TexturePtr pTexture;

			if (auto loadTextureTask = App::GetSubmodule<TextureImporter>()->LoadTexture(*sampler.m_second, pTexture))
			{
				auto updateSampler = loadTextureTask->Then(
					[=](TexturePtr texture) mutable
					{
						if (texture)
						{
							pMaterial->SetSampler(sampler.m_first, texture);
							texture->AddHotReloadDependentObject(pMaterial);
						}
					}, "Set material texture binding", EThreadType::Render);

				promise->Join(updateSampler);
			}
		}

		for (const auto& uniform : pMaterialAsset->GetUniformsVec4())
		{
			pMaterial->SetUniform(uniform.m_first, *uniform.m_second);
		}

		for (const auto& uniform : pMaterialAsset->GetUniformsFloat())
		{
			pMaterial->SetUniform(uniform.m_first, *uniform.m_second);
		}

		promise->Join(pLoadShader);

		outMaterial = loadedMaterial = pMaterial;

		promise->Run();

		m_promises.Unlock(uid);
		m_loadedMaterials.Unlock(uid);

		return promise;
	}

	outMaterial = nullptr;
	m_promises.Unlock(uid);
	m_loadedMaterials.Unlock(uid);

	SAILOR_LOG("Cannot find material with uid: %s", uid.ToString().c_str());
	return Tasks::TaskPtr<MaterialPtr>();
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

	LoadMaterial(uid, outAsset);
	out = outAsset;
	return true;
}

void MaterialImporter::CollectGarbage()
{
	TVector<FileId> uidsToRemove;

	m_promises.LockAll();
	auto ids = m_promises.GetKeys();
	m_promises.UnlockAll();

	for (const auto& id : ids)
	{
		auto promise = m_promises.At_Lock(id);

		if (!promise.IsValid() || (promise.IsValid() && promise->IsFinished()))
		{
			FileId uid = id;
			uidsToRemove.Emplace(uid);
		}

		m_promises.Unlock(id);
	}

	for (auto& uid : uidsToRemove)
	{
		m_promises.Remove(uid);
	}
}
