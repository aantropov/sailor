#include "AssetRegistry/Material/MaterialImporter.h"

#include "AssetRegistry/UID.h"
#include "AssetRegistry/AssetRegistry.h"
#include "AssetRegistry/Texture/TextureImporter.h"
#include "MaterialAssetInfo.h"
#include "AssetRegistry/Shader/ShaderCompiler.h"
#include "Math/Math.h"
#include "Core/Utils.h"
#include "Engine/World.h"
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
	return m_shader && m_shader->IsReady() && m_commonShaderBindings.IsValid() && m_commonShaderBindings->IsReady();
}

MaterialPtr Material::CreateInstance(WorldPtr world, const MaterialPtr& material)
{
	auto allocator = world->GetAllocator();

	MaterialPtr newMaterial = MaterialPtr::Make(allocator, material->GetUID());

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

	auto updateRHI = Tasks::Scheduler::CreateTask("Update material RHI resource", [=]
		{
			UpdateRHIResource();
	ForcelyUpdateUniforms();
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
	m_uniformsVec4.Clear();
	m_uniformsFloat.Clear();
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

void Material::SetUniform(const std::string& name, float value)
{
	SAILOR_PROFILE_FUNCTION();

	m_uniformsFloat[name] = value;
	m_bIsDirty = true;
}

void Material::SetUniform(const std::string& name, glm::vec4 value)
{
	SAILOR_PROFILE_FUNCTION();

	m_uniformsVec4[name] = value;
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

		m_commonShaderBindings->RecalculateCompatibility();
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
	GetOrAddRHI(RHI::Renderer::GetDriver()->GetOrAddVertexDescription<RHI::VertexP3N3T3B3UV2C4>());

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

	// TODO: Remove boilerplate
	for (auto& uniform : m_uniformsVec4)
	{
		if (m_commonShaderBindings->HasParameter(uniform.m_first))
		{
			std::string outBinding;
			std::string outVariable;

			RHI::RHIShaderBindingSet::ParseParameter(uniform.m_first, outBinding, outVariable);
			RHI::RHIShaderBindingPtr& binding = m_commonShaderBindings->GetOrAddShaderBinding(outBinding);
		}
	}

	for (auto& uniform : m_uniformsFloat)
	{
		if (m_commonShaderBindings->HasParameter(uniform.m_first))
		{
			std::string outBinding;
			std::string outVariable;

			RHI::RHIShaderBindingSet::ParseParameter(uniform.m_first, outBinding, outVariable);
			RHI::RHIShaderBindingPtr& binding = m_commonShaderBindings->GetOrAddShaderBinding(outBinding);
		}
	}
	SAILOR_PROFILE_END_BLOCK();

	m_commonShaderBindings->RecalculateCompatibility();
	m_bIsDirty = false;
}

void Material::UpdateUniforms(RHI::RHICommandListPtr cmdList)
{
	// TODO: Remove boilerplate
	for (auto& uniform : m_uniformsVec4)
	{
		if (m_commonShaderBindings->HasParameter(uniform.m_first))
		{
			std::string outBinding;
			std::string outVariable;

			RHI::RHIShaderBindingSet::ParseParameter(uniform.m_first, outBinding, outVariable);
			RHI::RHIShaderBindingPtr& binding = m_commonShaderBindings->GetOrAddShaderBinding(outBinding);

			const glm::vec4 value = uniform.m_second;
			RHI::Renderer::GetDriverCommands()->UpdateShaderBindingVariable(cmdList, binding, outVariable, &value, sizeof(value));
		}
	}

	for (auto& uniform : m_uniformsFloat)
	{
		if (m_commonShaderBindings->HasParameter(uniform.m_first))
		{
			std::string outBinding;
			std::string outVariable;

			RHI::RHIShaderBindingSet::ParseParameter(uniform.m_first, outBinding, outVariable);
			RHI::RHIShaderBindingPtr& binding = m_commonShaderBindings->GetOrAddShaderBinding(outBinding);

			const float value = uniform.m_second;
			RHI::Renderer::GetDriverCommands()->UpdateShaderBindingVariable(cmdList, binding, outVariable, &value, sizeof(value));
		}
	}
}

void Material::ForcelyUpdateUniforms()
{
	SAILOR_PROFILE_BLOCK("Create command list");
	RHI::RHICommandListPtr cmdList = RHI::Renderer::GetDriver()->CreateCommandList(false, true);
	RHI::Renderer::GetDriver()->SetDebugName(cmdList, "Forcely Update Uniforms");
	RHI::Renderer::GetDriverCommands()->BeginCommandList(cmdList, true);
	UpdateUniforms(cmdList);
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
}

YAML::Node MaterialAsset::Serialize() const
{
	YAML::Node outData;
	outData["bEnableDepthTest"] = m_pData->m_renderState.IsDepthTestEnabled();
	outData["bEnableZWrite"] = m_pData->m_renderState.IsEnabledZWrite();
	outData["bSupportMultisampling"] = m_pData->m_renderState.SupportMultisampling();
	outData["bCustomDepthShader"] = m_pData->m_renderState.IsRequiredCustomDepthShader();

	outData["depthBias"] = m_pData->m_renderState.GetDepthBias();
	outData["renderQueue"] = GetRenderQueue();
	outData["defines"] = m_pData->m_shaderDefines;

	outData["cullMode"] = SerializeEnum<RHI::ECullMode>(m_pData->m_renderState.GetCullMode());
	outData["blendMode"] = SerializeEnum<RHI::EBlendMode>(m_pData->m_renderState.GetBlendMode());
	outData["fillMode"] = SerializeEnum<RHI::EFillMode>(m_pData->m_renderState.GetFillMode());

	outData["shaderUid"] = m_pData->m_shader;
	outData["samplers"] = m_pData->m_samplers;
	outData["uniformsVec4"] = m_pData->m_uniformsVec4;
	outData["uniformsFloat"] = m_pData->m_uniformsFloat;

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

	if (outData["bEnableDepthTest"])
	{
		bEnableDepthTest = outData["bEnableDepthTest"].as<bool>();
	}

	if (outData["bEnableZWrite"])
	{
		bEnableZWrite = outData["bEnableZWrite"].as<bool>();
	}

	if (outData["bCustomDepthShader"])
	{
		bCustomDepthShader = outData["bCustomDepthShader"].as<bool>();
	}

	if (outData["bSupportMultisampling"])
	{
		bSupportMultisampling = outData["bSupportMultisampling"].as<bool>();
	}

	if (outData["depthBias"])
	{
		depthBias = outData["depthBias"].as<float>();
	}

	if (outData["cullMode"])
	{
		DeserializeEnum<RHI::ECullMode>(outData["cullMode"], cullMode);
	}

	if (outData["fillMode"])
	{
		DeserializeEnum<RHI::EFillMode>(outData["fillMode"], fillMode);
	}

	if (outData["renderQueue"])
	{
		renderQueue = outData["renderQueue"].as<std::string>();
	}

	if (outData["blendMode"])
	{
		DeserializeEnum<RHI::EBlendMode>(outData["blendMode"], blendMode);
	}

	if (outData["defines"])
	{
		m_pData->m_shaderDefines = outData["defines"].as<TVector<std::string>>();
	}

	if (outData["samplers"])
	{
		m_pData->m_samplers = outData["samplers"].as<TVector<SamplerEntry>>();
	}

	if (outData["uniformsVec4"])
	{
		m_pData->m_uniformsVec4 = outData["uniformsVec4"].as<TVector<TPair<std::string, glm::vec4>>>();
	}

	if (outData["uniformsFloat"])
	{
		m_pData->m_uniformsFloat = outData["uniformsFloat"].as<TVector<TPair<std::string, float>>>();
	}

	if (outData["shaderUid"])
	{
		m_pData->m_shader = outData["shaderUid"].as<UID>();
	}

	const size_t tag = GetHash(renderQueue);
	m_pData->m_renderState = RHI::RenderState(bEnableDepthTest, bEnableZWrite, depthBias, bCustomDepthShader, cullMode, blendMode, fillMode, tag, bSupportMultisampling);
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
			auto updateMaterial = Tasks::Scheduler::CreateTask("Update Material", [=]() mutable 
			{
				auto pMaterial = material;

				pMaterial->GetShader()->RemoveHotReloadDependentObject(material);
				pMaterial->ClearSamplers();
				pMaterial->ClearUniforms();

				ShaderSetPtr pShader;
				auto pLoadShader = App::GetSubmodule<ShaderCompiler>()->LoadShader(pMaterialAsset->GetShader(), pShader, pMaterialAsset->GetShaderDefines());

				pMaterial->SetRenderState(pMaterialAsset->GetRenderState());

				pMaterial->SetShader(pShader);
				pShader->AddHotReloadDependentObject(material);

				const UID uid = pMaterial->GetUID();
				const string assetFilename = App::GetSubmodule<AssetRegistry>()->GetAssetInfoPtr(uid)->GetAssetFilepath();

				auto updateRHI = Tasks::Scheduler::CreateTask("Update material RHI resource", [=]() mutable
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
				}, Tasks::EThreadType::Render);

				// Preload textures
				for (auto& sampler : pMaterialAsset->GetSamplers())
				{
					TexturePtr texture;

					if (auto loadTextureTask = App::GetSubmodule<TextureImporter>()->LoadTexture(sampler.m_uid, texture))
					{
						auto updateSampler = loadTextureTask->Then<void, TexturePtr>(
							[=](TexturePtr pTexture) mutable
							{
								if (pTexture)
								{
									pMaterial->SetSampler(sampler.m_name, texture);
									pTexture->AddHotReloadDependentObject(material);
								}
							}, "Set material texture binding", Tasks::EThreadType::Render);

						updateRHI->Join(updateSampler);
					}
				}

				for (auto& uniform : pMaterialAsset->GetUniformsVec4())
				{
					pMaterial->SetUniform(uniform.m_first, uniform.m_second);
				}

				for (auto& uniform : pMaterialAsset->GetUniformsFloat())
				{
					pMaterial->SetUniform(uniform.m_first, uniform.m_second);
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

		std::string materialYaml;

		AssetRegistry::ReadAllTextFile(filepath, materialYaml);

		YAML::Node yamlNode = YAML::Load(materialYaml);

		MaterialAsset* material = new MaterialAsset();
		material->Deserialize(yamlNode);

		return TSharedPtr<MaterialAsset>(material);
	}

	SAILOR_LOG("Cannot find material asset info with UID: %s", uid.ToString().c_str());
	return TSharedPtr<MaterialAsset>();
}

const UID& MaterialImporter::CreateMaterialAsset(const std::string& assetFilepath, MaterialAsset::Data data)
{
	YAML::Node newMaterial;

	MaterialAsset asset;
	asset.m_pData = TUniquePtr<MaterialAsset::Data>::Make(std::move(data));
	newMaterial = asset.Serialize();

	std::ofstream assetFile(assetFilepath);

	assetFile << newMaterial;
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

		const UID uid = pMaterial->GetUID();
		const string assetFilename = App::GetSubmodule<AssetRegistry>()->GetAssetInfoPtr(uid)->GetAssetFilepath();

		newPromise = Tasks::Scheduler::CreateTaskWithResult<MaterialPtr>("Load material",
			[pMaterial, pMaterialAsset, assetFilename = assetFilename]() mutable
			{
				// We're updating rhi on worker thread during load since we have no deps
				auto updateRHI = Tasks::Scheduler::CreateTask("Update material RHI resource", [=]() mutable
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

					}, Tasks::EThreadType::RHI);

				// Preload textures
				for (auto& sampler : pMaterialAsset->GetSamplers())
				{
					TexturePtr pTexture;

					if(auto loadTextureTask = App::GetSubmodule<TextureImporter>()->LoadTexture(sampler.m_uid, pTexture))
					{
						auto updateSampler = loadTextureTask->Then<void, TexturePtr>(
							[=](TexturePtr texture) mutable
							{
								if (texture)
								{
									pMaterial->SetSampler(sampler.m_name, texture);
									texture->AddHotReloadDependentObject(pMaterial);
								}
						
							}, "Set material texture binding", Tasks::EThreadType::Render);

						updateRHI->Join(updateSampler);
					}
				}

				for (auto& uniform : pMaterialAsset->GetUniformsVec4())
				{
					pMaterial->SetUniform(uniform.m_first, uniform.m_second);
				}

				for (auto& uniform : pMaterialAsset->GetUniformsFloat())
				{
					pMaterial->SetUniform(uniform.m_first, uniform.m_second);
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
		if (promise.m_second && promise.m_second->IsFinished())
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