#include "AssetRegistry/Material/MaterialImporter.h"

#include "AssetRegistry/FileId.h"
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

	auto updateRHI = Tasks::CreateTask("Update material RHI resource", [=]
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
		m_samplers.At_Lock(name) = value;
		m_samplers.Unlock(name);

		m_bIsDirty = true;
	}
}

void Material::SetUniform(const std::string& name, float value)
{
	SAILOR_PROFILE_FUNCTION();

	m_uniformsFloat.At_Lock(name) = value;
	m_uniformsFloat.Unlock(name);

	m_bIsDirty = true;
}

void Material::SetUniform(const std::string& name, glm::vec4 value)
{
	SAILOR_PROFILE_FUNCTION();

	m_uniformsVec4.At_Lock(name) = value;
	m_uniformsVec4.Unlock(name);

	m_bIsDirty = true;
}

RHI::RHIMaterialPtr Material::GetOrAddRHI(RHI::RHIVertexDescriptionPtr vertexDescription)
{
	SAILOR_PROFILE_FUNCTION();

	SAILOR_PROFILE_BLOCK("Achieve exclusive access to rhi");

	// TODO: Resolve collisions of VertexAttributeBits
	RHI::RHIMaterialPtr& material = m_rhiMaterials.At_Lock(vertexDescription->GetVertexAttributeBits());
	m_rhiMaterials.Unlock(vertexDescription->GetVertexAttributeBits());
	SAILOR_PROFILE_END_BLOCK();

	if (!material)
	{
		SAILOR_PROFILE_BLOCK("Create RHI material for resource");

		if (!m_commonShaderBindings)
		{
			if (material = RHI::Renderer::GetDriver()->CreateMaterial(vertexDescription, RHI::EPrimitiveTopology::TriangleList, m_renderState, m_shader))
			{
				m_commonShaderBindings = material->GetBindings();
			}
			else
			{
				// We cannot create RHI material
				return nullptr;
			}
		}
		else
		{
			material = RHI::Renderer::GetDriver()->CreateMaterial(vertexDescription, RHI::EPrimitiveTopology::TriangleList, m_renderState, m_shader, m_commonShaderBindings);
		}

		SAILOR_PROFILE_END_BLOCK();

		m_commonShaderBindings->RecalculateCompatibility();
	}

	return material;
}

void Material::UpdateRHIResource()
{
	//SAILOR_LOG("Update material RHI resource: %s", GetFileId().ToString().c_str());

	// All RHI materials are outdated now
	m_rhiMaterials.Clear();
	m_commonShaderBindings.Clear();

	// Create base material
	GetOrAddRHI(RHI::Renderer::GetDriver()->GetOrAddVertexDescription<RHI::VertexP3N3T3B3UV2C4>());

	SAILOR_PROFILE_BLOCK("Update samplers");
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
			RHI::RHIShaderBindingPtr& binding = m_commonShaderBindings->GetOrAddShaderBinding(outBinding);
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

	for (auto& sampler : m_samplers)
	{
		const std::string parameterName = "material." + sampler.m_first;

		if (m_commonShaderBindings->HasParameter(parameterName))
		{
			std::string outBinding;
			std::string outVariable;

			RHI::RHIShaderBindingSet::ParseParameter(parameterName, outBinding, outVariable);
			RHI::RHIShaderBindingPtr& binding = m_commonShaderBindings->GetOrAddShaderBinding(outBinding);

			const uint32_t value = (uint32_t)App::GetSubmodule<TextureImporter>()->GetTextureIndex(sampler.m_second->GetFileId());
			RHI::Renderer::GetDriverCommands()->UpdateShaderBindingVariable(cmdList, binding, outVariable, &value, sizeof(value));
		}
	}
}

void Material::ForcelyUpdateUniforms()
{
	SAILOR_PROFILE_BLOCK("Create command list");
	RHI::RHICommandListPtr cmdList = RHI::Renderer::GetDriver()->CreateCommandList(false, RHI::ECommandListQueue::Transfer);
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
		m_pData->m_samplers = outData["samplers"].as<TMap<std::string, FileId>>();
	}

	if (outData["uniformsVec4"])
	{
		m_pData->m_uniformsVec4 = outData["uniformsVec4"].as<TMap<std::string, glm::vec4>>();
	}

	if (outData["uniformsFloat"])
	{
		m_pData->m_uniformsFloat = outData["uniformsFloat"].as<TMap<std::string, float>>();
	}

	if (outData["shaderUid"])
	{
		m_pData->m_shader = outData["shaderUid"].as<FileId>();
	}

	const size_t tag = GetHash(renderQueue);
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
						}, Tasks::EThreadType::Render);

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
								}, "Set material texture binding", Tasks::EThreadType::Render);

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
		const std::string& filepath = materialAssetInfo->GetAssetFilepath();

		std::string materialYaml;

		AssetRegistry::ReadAllTextFile(filepath, materialYaml);

		YAML::Node yamlNode = YAML::Load(materialYaml);

		MaterialAsset* material = new MaterialAsset();
		material->Deserialize(yamlNode);

		return TSharedPtr<MaterialAsset>(material);
	}

	SAILOR_LOG("Cannot find material asset info with FileId: %s", uid.ToString().c_str());
	return TSharedPtr<MaterialAsset>();
}

const FileId& MaterialImporter::CreateMaterialAsset(const std::string& assetFilepath, MaterialAsset::Data data)
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

		promise = Tasks::CreateTaskWithResult<MaterialPtr>("Load material",
			[pMaterial, pMaterialAsset, assetFilename = assetFilename]() mutable
			{
				// We're updating rhi on worker thread during load since we have no deps
				auto updateRHI = Tasks::CreateTask("Update material RHI resource", [=]() mutable
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
							}, "Set material texture binding", Tasks::EThreadType::Render);

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

				updateRHI->Run();

				return pMaterial;
			});

		outMaterial = loadedMaterial = pMaterial;

		promise->Join(pLoadShader);
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