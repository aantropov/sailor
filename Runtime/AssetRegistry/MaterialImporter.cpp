#include "MaterialImporter.h"

#include "UID.h"
#include "AssetRegistry.h"
#include "MaterialAssetInfo.h"
#include "Utils.h"
#include <filesystem>
#include <fstream>
#include <algorithm>
#include <iostream>

#include "nlohmann_json/include/nlohmann/json.hpp"
#include "JobSystem/JobSystem.h"
#include "RHI/Material.h"
#include "AssetRegistry/TextureImporter.h"

using namespace Sailor;

void MaterialAsset::Serialize(nlohmann::json& outData) const
{
	outData["enable_depth_test"] = m_renderState.IsDepthTestEnabled();
	outData["enable_z_write"] = m_renderState.IsEnabledZWrite();
	outData["depth_bias"] = m_renderState.GetDepthBias();
	outData["cull_mode"] = m_renderState.GetCullMode();
	outData["render_queue"] = GetRenderQueue();
	outData["is_transparent"] = IsTransparent();
	outData["blend_mode"] = m_renderState.GetBlendMode();
	outData["fill_mode"] = m_renderState.GetFillMode();
	outData["defines"] = m_shaderDefines;

	auto jsonSamplers = json::array();
	for (auto& sampler : m_samplers)
	{
		nlohmann::json o;
		sampler.Serialize(o);
		jsonSamplers.push_back(o);
	}

	outData["samplers"] = jsonSamplers;
	outData["uniforms"] = m_uniforms;

	m_shader.Serialize(outData["shader"]);
}

void MaterialAsset::Deserialize(const nlohmann::json& outData)
{
	/*MaterialAsset a;
	a.m_samplers.push_back(SamplerEntry());
	a.m_uniforms.push_back({ "color", 5.0f });
	nlohmann::json j;
	a.Serialize(j);
	std::string s = j.dump();
	*/

	bool bEnableDepthTest = true;
	bool bEnableZWrite = true;
	float depthBias = 0.0f;
	ECullMode cullMode = ECullMode::Back;
	EBlendMode blendMode = EBlendMode::None;
	EFillMode fillMode = EFillMode::Fill;
	std::string renderQueue = "Opaque";

	m_shaderDefines.clear();
	m_uniforms.clear();

	if (outData.contains("enable_depth_test"))
	{
		bEnableDepthTest = outData["enable_depth_test"].get<bool>();
	}

	if (outData.contains("enable_z_write"))
	{
		bEnableZWrite = outData["enable_z_write"].get<bool>();
	}

	if (outData.contains("depth_bias"))
	{
		depthBias = outData["depth_bias"].get<float>();
	}

	if (outData.contains("cull_mode"))
	{
		cullMode = (ECullMode)outData["cull_mode"].get<uint8_t>();
	}

	if (outData.contains("fill_mode"))
	{
		fillMode = (EFillMode)outData["fill_mode"].get<uint8_t>();
	}

	if (outData.contains("render_queue"))
	{
		renderQueue = outData["render_queue"].get<std::string>();
	}

	if (outData.contains("is_transparent"))
	{
		m_bIsTransparent = outData["is_transparent"].get<bool>();
	}

	if (outData.contains("blend_mode"))
	{
		blendMode = (EBlendMode)outData["blend_mode"].get<uint8_t>();
	}

	if (outData.contains("defines"))
	{
		for (auto& elem : outData["defines"])
		{
			m_shaderDefines.push_back(elem.get<std::string>());
		}
	}

	if (outData.contains("samplers"))
	{
		for (auto& elem : outData["samplers"])
		{
			SamplerEntry entry;
			entry.Deserialize(elem);

			m_samplers.emplace_back(std::move(entry));
		}
	}

	if (outData.contains("uniforms"))
	{
		for (auto& elem : outData["uniforms"])
		{
			auto first = elem[0].get<std::string>();
			auto second = elem[1].get<float>();

			m_uniforms.push_back({ first, second });
		}
	}

	if (outData.contains("shader"))
	{
		m_shader.Deserialize(outData["shader"]);
	}

	m_renderState = RenderState(bEnableDepthTest, bEnableZWrite, depthBias, cullMode, blendMode, fillMode);
}

void MaterialImporter::Initialize()
{
	SAILOR_PROFILE_FUNCTION();

	m_pInstance = new MaterialImporter();
	MaterialAssetInfoHandler::GetInstance()->Subscribe(m_pInstance);
}

MaterialImporter::~MaterialImporter()
{
}

void MaterialImporter::OnAssetInfoUpdated(AssetInfoPtr assetInfo)
{
}

TWeakPtr<MaterialAsset> MaterialImporter::LoadMaterialAsset(UID uid)
{
	SAILOR_PROFILE_FUNCTION();

	if (MaterialAssetInfoPtr materialAssetInfo = dynamic_cast<MaterialAssetInfoPtr>(AssetRegistry::GetInstance()->GetAssetInfoPtr(uid)))
	{
		if (const auto& loadedMaterial = m_pInstance->m_loadedMaterials.find(uid); loadedMaterial != m_pInstance->m_loadedMaterials.end())
		{
			return loadedMaterial->second;
		}

		const std::string& filepath = materialAssetInfo->GetAssetFilepath();

		std::string materialJson;

		AssetRegistry::ReadAllTextFile(filepath, materialJson);

		json j_material;
		if (j_material.parse(materialJson.c_str()) == nlohmann::detail::value_t::discarded)
		{
			SAILOR_LOG("Cannot parse material asset file: %s", filepath.c_str());
			return TWeakPtr<MaterialAsset>();
		}

		j_material = json::parse(materialJson);

		MaterialAsset* material = new MaterialAsset();
		material->Deserialize(j_material);

		return m_pInstance->m_loadedMaterials[uid] = TSharedPtr<MaterialAsset>(material);
	}

	SAILOR_LOG("Cannot find material asset info with UID: %s", uid.ToString().c_str());
	return TWeakPtr<MaterialAsset>();
}

RHI::MaterialPtr MaterialImporter::LoadMaterial(UID uid)
{
	auto pMaterial = GetInstance()->LoadMaterialAsset(uid);

	if (pMaterial)
	{
		auto pSharedMaterial = pMaterial.Lock();
		MaterialPtr pMaterialPtr = Renderer::GetDriver()->CreateMaterial(pSharedMaterial->GetRenderState(), pSharedMaterial->GetShader(), pSharedMaterial->GetShaderDefines());

		for (auto& sampler : pSharedMaterial->GetSamplers())
		{
			if (pMaterialPtr->HasParameter(sampler.m_name))
			{
				TexturePtr texture = TextureImporter::LoadTexture(sampler.m_uid);
				Renderer::GetDriver()->SetMaterialParameter(pMaterialPtr, sampler.m_name, texture);
			}
		}

		for (auto& uniform : pSharedMaterial->GetUniforms())
		{
			if (pMaterialPtr->HasParameter(uniform.first))
			{
				Renderer::GetDriver()->SetMaterialParameter(pMaterialPtr, uniform.first, uniform.second);
			}
		}

		return pMaterialPtr;
	}

	return nullptr;
}
