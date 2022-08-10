#include "AssetRegistry/Material/MaterialImporter.h"

#include "AssetRegistry/UID.h"
#include "AssetRegistry/AssetRegistry.h"
#include "AssetRegistry/Texture/TextureImporter.h"
#include "MaterialAssetInfo.h"
#include "AssetRegistry/Shader/ShaderCompiler.h"
#include "Math/Math.h"
#include "Core/Utils.h"
#include "Memory/WeakPtr.hpp"
#include "Memory/ObjectAllocator.hpp"
#include <filesystem>
#include <fstream>
#include <algorithm>
#include <iostream>

#include "nlohmann_json/include/nlohmann/json.hpp"
#include "RHI/Renderer.h"
#include "RHI/Material.h"
#include "RHI/VertexDescription.h"
#include "RHI/Shader.h"
#include "RHI/Fence.h"
#include "RHI/CommandList.h"
#include "AssetRegistry/Texture/TextureImporter.h"

using namespace Sailor;

bool Material::IsReady() const
{
	return m_shader && m_shader->IsReady() && m_commonShaderBindings && m_commonShaderBindings->IsReady();
}

Tasks::ITaskPtr Material::OnHotReload()
{
	m_bIsDirty = true;

	auto updateRHI = Tasks::Scheduler::CreateTask("Update material RHI resource", [=]()
	{
		UpdateRHIResource();
	}, Tasks::EThreadType::Render);

	return updateRHI;
}

void Material::ClearSamplers()
{
	SAILOR_PROFILE_FUNCTION();

	for (auto& sampler : m_samplers)
	{
		sampler.m_second->RemoveHotReloadDependentObject(sampler.m_second);
	}
	m_samplers.Clear();
}

void Material::ClearUniforms()
{
	m_uniforms.Clear();
}

void Material::SetSampler(const std::string& name, TexturePtr value)
{
	SAILOR_PROFILE_FUNCTION();

	if (value)
	{
		m_samplers[name] = value;
		m_bIsDirty = true;
	}
}

void Material::SetUniform(const std::string& name, glm::vec4 value)
{
	SAILOR_PROFILE_FUNCTION();

	m_uniforms[name] = value;
	m_bIsDirty = true;
}

RHI::RHIMaterialPtr& Material::GetOrAddRHI(RHI::RHIVertexDescriptionPtr vertexDescription)
{
	SAILOR_PROFILE_FUNCTION();

	SAILOR_PROFILE_BLOCK("Achieve exclusive access to rhi");
	// TODO: Resolve collisions of VertexAttributeBits
	auto& material = m_rhiMaterials.At_Lock(vertexDescription->GetVertexAttributeBits());
	m_rhiMaterials.Unlock(vertexDescription->GetVertexAttributeBits());
	SAILOR_PROFILE_END_BLOCK();

	if (!material)
	{
		SAILOR_LOG("Create RHI material for resource: %s, vertex attribute bits: %llu", GetUID().ToString().c_str(), vertexDescription->GetVertexAttributeBits());

		if (!m_commonShaderBindings)
		{
			material = RHI::Renderer::GetDriver()->CreateMaterial(vertexDescription, RHI::EPrimitiveTopology::TriangleList, m_renderState, m_shader);
			m_commonShaderBindings = material->GetBindings();
		}
		else
		{
			material = RHI::Renderer::GetDriver()->CreateMaterial(vertexDescription, RHI::EPrimitiveTopology::TriangleList, m_renderState, m_shader, m_commonShaderBindings);
		}
	}

	return material;
}

void Material::UpdateRHIResource()
{
	SAILOR_LOG("Update material RHI resource: %s", GetUID().ToString().c_str());

	// All RHI materials are outdated now
	m_rhiMaterials.Clear();
	m_commonShaderBindings.Clear();

	// Create base material
	GetOrAddRHI(RHI::Renderer::GetDriver()->GetOrAddVertexDescription<RHI::VertexP3N3UV2C4>());

	SAILOR_PROFILE_BLOCK("Update samplers");
	for (auto& sampler : m_samplers)
	{
		if (m_commonShaderBindings->HasBinding(sampler.m_first))
		{
			RHI::Renderer::GetDriver()->UpdateShaderBinding(m_commonShaderBindings, sampler.m_first, sampler.m_second->GetRHI());
		}
	}
	SAILOR_PROFILE_END_BLOCK();

	SAILOR_PROFILE_BLOCK("Update shader bindings");
	// Create all bindings first
	for (auto& uniform : m_uniforms)
	{
		if (m_commonShaderBindings->HasParameter(uniform.m_first))
		{
			std::string outBinding;
			std::string outVariable;

			RHI::RHIShaderBindingSet::ParseParameter(uniform.m_first, outBinding, outVariable);
			RHI::RHIShaderBindingPtr& binding = m_commonShaderBindings->GetOrCreateShaderBinding(outBinding);
		}
	}
	SAILOR_PROFILE_END_BLOCK();

	SAILOR_PROFILE_BLOCK("Create command list");
	RHI::RHICommandListPtr cmdList = RHI::Renderer::GetDriver()->CreateCommandList(false, true);
	RHI::Renderer::GetDriverCommands()->BeginCommandList(cmdList);

	for (auto& uniform : m_uniforms)
	{
		if (m_commonShaderBindings->HasParameter(uniform.m_first))
		{
			std::string outBinding;
			std::string outVariable;

			RHI::RHIShaderBindingSet::ParseParameter(uniform.m_first, outBinding, outVariable);
			RHI::RHIShaderBindingPtr& binding = m_commonShaderBindings->GetOrCreateShaderBinding(outBinding);

			const glm::vec4 value = uniform.m_second;
			RHI::Renderer::GetDriverCommands()->UpdateShaderBindingVariable(cmdList, binding, outVariable, &value, sizeof(value));
		}
	}

	RHI::Renderer::GetDriverCommands()->EndCommandList(cmdList);
	SAILOR_PROFILE_END_BLOCK();

	// Create fences to track the state of material update
	RHI::RHIFencePtr fence = RHI::RHIFencePtr::Make();
	RHI::Renderer::GetDriver()->TrackDelayedInitialization(m_commonShaderBindings.GetRawPtr(), fence);

	// Submit cmd lists
	SAILOR_ENQUEUE_TASK_RENDER_THREAD("Update shader bindings set rhi",
		([this, cmdList, fence]()
	{
		RHI::Renderer::GetDriver()->SubmitCommandList(cmdList, fence);
	}));

	m_bIsDirty = false;
}

void MaterialAsset::Serialize(nlohmann::json& outData) const
{
	outData["bEnableDepthTest"] = m_pData->m_renderState.IsDepthTestEnabled();
	outData["bEnableZWrite"] = m_pData->m_renderState.IsEnabledZWrite();
	outData["bSupportMultisampling"] = m_pData->m_renderState.SupportMultisampling();
	outData["depthBias"] = m_pData->m_renderState.GetDepthBias();
	outData["renderQueue"] = GetRenderQueue();
	outData["defines"] = m_pData->m_shaderDefines;

	SerializeEnum<RHI::ECullMode>(outData["cullMode"], m_pData->m_renderState.GetCullMode());
	SerializeEnum<RHI::EBlendMode>(outData["blendMode"],m_pData->m_renderState.GetBlendMode());
	SerializeEnum<RHI::EFillMode>(outData["fillMode"], m_pData->m_renderState.GetFillMode());

	SerializeArray(m_pData->m_samplers, outData["samplers"]);
	outData["uniforms"] = m_pData->m_uniformsVec4;

	m_pData->m_shader.Serialize(outData["shader"]);
}

void MaterialAsset::Deserialize(const nlohmann::json& outData)
{
	m_pData = TUniquePtr<Data>::Make();

	bool bEnableDepthTest = true;
	bool bEnableZWrite = true;
	bool bSupportMultisampling = true;
	float depthBias = 0.0f;
	RHI::ECullMode cullMode = RHI::ECullMode::Back;
	RHI::EBlendMode blendMode = RHI::EBlendMode::None;
	RHI::EFillMode fillMode = RHI::EFillMode::Fill;
	std::string renderQueue = "Opaque";

	m_pData->m_shaderDefines.Clear();
	m_pData->m_uniformsVec4.Clear();

	if (outData.contains("bEnableDepthTest"))
	{
		bEnableDepthTest = outData["bEnableDepthTest"].get<bool>();
	}

	if (outData.contains("bEnableZWrite"))
	{
		bEnableZWrite = outData["bEnableZWrite"].get<bool>();
	}

	if (outData.contains("bSupportMultisampling"))
	{
		bSupportMultisampling = outData["bSupportMultisampling"].get<bool>();
	}

	if (outData.contains("depthBias"))
	{
		depthBias = outData["depthBias"].get<float>();
	}

	if (outData.contains("cullMode"))
	{
		DeserializeEnum<RHI::ECullMode>(outData["cullMode"], cullMode);
	}

	if (outData.contains("fillMode"))
	{
		DeserializeEnum<RHI::EFillMode>(outData["fillMode"], fillMode);
	}

	if (outData.contains("renderQueue"))
	{
		renderQueue = outData["renderQueue"].get<std::string>();
	}

	if (outData.contains("blendMode"))
	{
		DeserializeEnum<RHI::EBlendMode>(outData["blendMode"], blendMode);
	}

	if (outData.contains("defines"))
	{
		for (auto& elem : outData["defines"])
		{
			m_pData->m_shaderDefines.Add(elem.get<std::string>());
		}
	}

	if (outData.contains("samplers"))
	{
		Sailor::DeserializeArray(m_pData->m_samplers, outData["samplers"]);
	}

	if (outData.contains("uniforms"))
	{
		for (auto& elem : outData["uniforms"])
		{
			auto first = elem[0].get<std::string>();
			auto second = elem[1].get<glm::vec4>();

			m_pData->m_uniformsVec4.Add({ first, second });
		}
	}

	if (outData.contains("shader"))
	{
		m_pData->m_shader.Deserialize(outData["shader"]);
	}

	const size_t tag = GetHash(renderQueue);
	m_pData->m_renderState = RHI::RenderState(bEnableDepthTest, bEnableZWrite, depthBias, cullMode, blendMode, fillMode, tag, bSupportMultisampling);
}

MaterialImporter::MaterialImporter(MaterialAssetInfoHandler* infoHandler)
{
	SAILOR_PROFILE_FUNCTION();
	m_allocator = ObjectAllocatorPtr::Make();
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
	MaterialPtr material = GetLoadedMaterial(assetInfo->GetUID());
	if (bWasExpired && material)
	{
		// We need to start load the material
		if (auto pMaterialAsset = LoadMaterialAsset(assetInfo->GetUID()))
		{
			auto updateMaterial = Tasks::Scheduler::CreateTask("Update Material", [=]() {

				auto pMaterial = material;

				pMaterial->GetShader()->RemoveHotReloadDependentObject(material);
				pMaterial->ClearSamplers();
				pMaterial->ClearUniforms();

				ShaderSetPtr pShader;
				auto pLoadShader = App::GetSubmodule<ShaderCompiler>()->LoadShader(pMaterialAsset->GetShader(), pShader, pMaterialAsset->GetShaderDefines());

				pMaterial->SetRenderState(pMaterialAsset->GetRenderState());

				pMaterial->SetShader(pShader);
				pShader->AddHotReloadDependentObject(material);

				auto updateRHI = Tasks::Scheduler::CreateTask("Update material RHI resource", [=]()
				{
					pMaterial.GetRawPtr()->UpdateRHIResource();
					pMaterial.GetRawPtr()->TraceHotReload(nullptr);
				}, Tasks::EThreadType::Render);

				// Preload textures
				for (auto& sampler : pMaterialAsset->GetSamplers())
				{
					TexturePtr texture;
					updateRHI->Join(
						App::GetSubmodule<TextureImporter>()->LoadTexture(sampler.m_uid, texture)->Then<void, TexturePtr>(
							[=](TexturePtr pTexture)
					{
						if (pTexture)
						{
							pTexture.GetRawPtr()->AddHotReloadDependentObject(material);
							pMaterial.GetRawPtr()->SetSampler(sampler.m_name, texture);
						}
					}, "Set material texture binding", Tasks::EThreadType::Render));
				}

				for (auto& uniform : pMaterialAsset.GetRawPtr()->GetUniformValues())
				{
					pMaterial.GetRawPtr()->SetUniform(uniform.m_first, uniform.m_second);
				}

				updateRHI->Run();
			});

			if (auto promise = GetLoadPromise(assetInfo->GetUID()))
			{
				updateMaterial->Join(promise);
			}

			updateMaterial->Run();
		}
	}
}

bool MaterialImporter::IsMaterialLoaded(UID uid) const
{
	return m_loadedMaterials.ContainsKey(uid);
}

TSharedPtr<MaterialAsset> MaterialImporter::LoadMaterialAsset(UID uid)
{
	SAILOR_PROFILE_FUNCTION();

	if (MaterialAssetInfoPtr materialAssetInfo = dynamic_cast<MaterialAssetInfoPtr>(App::GetSubmodule<AssetRegistry>()->GetAssetInfoPtr(uid)))
	{
		const std::string& filepath = materialAssetInfo->GetAssetFilepath();

		std::string materialJson;

		AssetRegistry::ReadAllTextFile(filepath, materialJson);

		json j_material;
		if (j_material.parse(materialJson.c_str()) == nlohmann::detail::value_t::discarded)
		{
			SAILOR_LOG("Cannot parse material asset file: %s", filepath.c_str());
			return TSharedPtr<MaterialAsset>();
		}

		j_material = json::parse(materialJson);

		MaterialAsset* material = new MaterialAsset();
		material->Deserialize(j_material);

		return TSharedPtr<MaterialAsset>(material);
	}

	SAILOR_LOG("Cannot find material asset info with UID: %s", uid.ToString().c_str());
	return TSharedPtr<MaterialAsset>();
}

const UID& MaterialImporter::CreateMaterialAsset(const std::string& assetFilepath, MaterialAsset::Data data)
{
	json newMaterial;

	MaterialAsset asset;
	asset.m_pData = TUniquePtr<MaterialAsset::Data>::Make(std::move(data));
	asset.Serialize(newMaterial);

	std::ofstream assetFile(assetFilepath);

	assetFile << newMaterial.dump(Sailor::JsonDumpIndent);
	assetFile.close();

	return App::GetSubmodule<AssetRegistry>()->LoadAsset(assetFilepath);
}

bool MaterialImporter::LoadMaterial_Immediate(UID uid, MaterialPtr& outMaterial)
{
	SAILOR_PROFILE_FUNCTION();
	auto task = LoadMaterial(uid, outMaterial);
	task->Wait();

	return task->GetResult().IsValid();
}

MaterialPtr MaterialImporter::GetLoadedMaterial(UID uid)
{
	// Check loaded materials
	auto materialIt = m_loadedMaterials.Find(uid);
	if (materialIt != m_loadedMaterials.end())
	{
		return (*materialIt).m_second;
	}
	return MaterialPtr();
}

Tasks::TaskPtr<MaterialPtr> MaterialImporter::GetLoadPromise(UID uid)
{
	auto it = m_promises.Find(uid);
	if (it != m_promises.end())
	{
		return (*it).m_second;
	}

	return Tasks::TaskPtr<MaterialPtr>();
}

Tasks::TaskPtr<MaterialPtr> MaterialImporter::LoadMaterial(UID uid, MaterialPtr& outMaterial)
{
	SAILOR_PROFILE_FUNCTION();

	Tasks::TaskPtr<MaterialPtr> newPromise;
	outMaterial = nullptr;

	// Check promises first
	auto it = m_promises.Find(uid);
	if (it != m_promises.end())
	{
		newPromise = (*it).m_second;
	}

	// Check loaded materials
	auto materialIt = m_loadedMaterials.Find(uid);
	if (materialIt != m_loadedMaterials.end())
	{
		outMaterial = (*materialIt).m_second;

		if (newPromise != nullptr)
		{
			if (!newPromise)
			{
				return Tasks::TaskPtr<MaterialPtr>::Make(outMaterial);
			}

			return newPromise;
		}
	}

	auto& promise = m_promises.At_Lock(uid, nullptr);

	// We have promise
	if (promise)
	{
		m_promises.Unlock(uid);
		outMaterial = m_loadedMaterials[uid];
		return promise;
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

		newPromise = Tasks::Scheduler::CreateTaskWithResult<MaterialPtr>("Load material",
			[pMaterial, pMaterialAsset]()
		{
			// We're updating rhi on worker thread during load since we have no deps
			auto updateRHI = Tasks::Scheduler::CreateTask("Update material RHI resource", [=]()
			{
				pMaterial.GetRawPtr()->UpdateRHIResource();
			}, Tasks::EThreadType::RHI);

			// Preload textures
			for (auto& sampler : pMaterialAsset->GetSamplers())
			{
				TexturePtr pTexture;
				updateRHI->Join(
					App::GetSubmodule<TextureImporter>()->LoadTexture(sampler.m_uid, pTexture)->Then<void, TexturePtr>(
						[=](TexturePtr texture)
				{
					if (texture)
					{
						texture.GetRawPtr()->AddHotReloadDependentObject(pMaterial);
						pMaterial.GetRawPtr()->SetSampler(sampler.m_name, texture);
					}
				}, "Set material texture binding", Tasks::EThreadType::Render));
			}

			for (auto& uniform : pMaterialAsset.GetRawPtr()->GetUniformValues())
			{
				pMaterial.GetRawPtr()->SetUniform(uniform.m_first, uniform.m_second);
			}

			updateRHI->Run();

			return pMaterial;
		});

		outMaterial = m_loadedMaterials[uid] = pMaterial;

		newPromise->Join(pLoadShader);
		App::GetSubmodule<Tasks::Scheduler>()->Run(newPromise);

		promise = newPromise;
		m_promises.Unlock(uid);

		return promise;
	}

	m_promises.Unlock(uid);

	return Tasks::TaskPtr<MaterialPtr>();
}

void MaterialImporter::CollectGarbage()
{
	TVector<UID> uidsToRemove;
	for (auto& promise : m_promises)
	{
		if (promise.m_second->IsFinished())
		{
			UID uid = promise.m_first;
			uidsToRemove.Emplace(uid);
		}
	}

	for (auto& uid : uidsToRemove)
	{
		m_promises.Remove(uid);
	}
}