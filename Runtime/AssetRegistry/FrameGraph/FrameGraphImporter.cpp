#include "AssetRegistry/FrameGraph/FrameGraphImporter.h"
#include "AssetRegistry/Texture/TextureImporter.h"
#include "RHI/Texture.h"
#include "AssetRegistry/UID.h"
#include "AssetRegistry/AssetRegistry.h"
#include "FrameGraphAssetInfo.h"
#include "Core/Utils.h"
#include <filesystem>
#include <fstream>
#include <algorithm>
#include <iostream>
#include "FrameGraph/FrameGraphNode.h"

#include "nlohmann_json/include/nlohmann/json.hpp"
#include "JobSystem/JobSystem.h"

using namespace Sailor;

TUniquePtr<TMap<std::string, std::function<FrameGraphNodePtr(void)>>> FrameGraphImporter::s_pNodeFactoryMethods;

void FrameGraphImporter::RegisterFrameGraphNode(const std::string& nodeName, std::function<FrameGraphNodePtr(void)> factoryMethod)
{
	if (!s_pNodeFactoryMethods)
	{
		s_pNodeFactoryMethods = TUniquePtr<TMap<std::string, std::function<FrameGraphNodePtr(void)>>>::Make();
	}

	(*s_pNodeFactoryMethods)[nodeName] = factoryMethod;
}

FrameGraphNodePtr FrameGraphImporter::CreateNode(const std::string& nodeName) const
{
	return (*s_pNodeFactoryMethods)[nodeName]();
}

void FrameGraphAsset::Deserialize(const nlohmann::json& inData)
{
	if (inData.contains("samplers"))
	{
		TVector<FrameGraphAsset::Resource> samplers;
		DeserializeArray<FrameGraphAsset::Resource>(samplers, inData["samplers"]);

		for (auto& sampler : samplers)
		{
			m_samplers[sampler.m_name] = std::move(sampler);
		}
	}

	if (inData.contains("values"))
	{
		TVector<Value> values;
		DeserializeArray<Value>(values, inData["values"]);

		for (auto& value : values)
		{
			m_values[value.m_name] = std::move(value);
		}
	}

	if (inData.contains("renderTargets"))
	{
		TVector<RenderTarget> targets;
		DeserializeArray<RenderTarget>(targets, inData["renderTargets"]);

		for (auto& target : targets)
		{
			m_renderTargets[target.m_name] = std::move(target);
		}
	}

	if (inData.contains("frame"))
	{
		for (const auto& el : inData["frame"])
		{
			FrameGraphAsset::Node node;
			node.Deserialize(el);
			m_nodes.Add(std::move(node));
		}
	}
}

FrameGraphImporter::FrameGraphImporter(FrameGraphAssetInfoHandler* infoHandler)
{
	SAILOR_PROFILE_FUNCTION();

	infoHandler->Subscribe(this);
}

FrameGraphImporter::~FrameGraphImporter()
{
}

void FrameGraphImporter::OnImportAsset(AssetInfoPtr assetInfo)
{
}

void FrameGraphImporter::OnUpdateAssetInfo(AssetInfoPtr assetInfo, bool bWasExpired)
{
}

FrameGraphAssetPtr FrameGraphImporter::LoadFrameGraphAsset(UID uid)
{
	SAILOR_PROFILE_FUNCTION();

	if (FrameGraphAssetInfoPtr assetInfo = dynamic_cast<FrameGraphAssetInfoPtr>(App::GetSubmodule<AssetRegistry>()->GetAssetInfoPtr(uid)))
	{
		const std::string& filepath = assetInfo->GetAssetFilepath();

		std::string json;

		AssetRegistry::ReadAllTextFile(filepath, json);

		nlohmann::json j_frameGraph;
		if (j_frameGraph.parse(json.c_str()) == nlohmann::detail::value_t::discarded)
		{
			SAILOR_LOG("Cannot parse frameGraph asset file: %s", filepath.c_str());
			return FrameGraphAssetPtr();
		}

		j_frameGraph = json::parse(json);

		FrameGraphAsset* frameGraphAsset = new FrameGraphAsset();
		frameGraphAsset->Deserialize(j_frameGraph);

		return FrameGraphAssetPtr(frameGraphAsset);
	}

	SAILOR_LOG("Cannot find frameGraph asset info with UID: %s", uid.ToString().c_str());
	return FrameGraphAssetPtr();
}

bool FrameGraphImporter::LoadFrameGraph_Immediate(UID uid, FrameGraphPtr& outFrameGraph)
{
	auto it = m_loadedFrameGraphs.Find(uid);
	if (it != m_loadedFrameGraphs.end())
	{
		outFrameGraph = (*it).m_second;
		return true;
	}

	if (auto pFrameGraphAsset = LoadFrameGraphAsset(uid))
	{
		FrameGraphPtr pFrameGraph = BuildFrameGraph(uid, pFrameGraphAsset);
		m_loadedFrameGraphs[uid] = pFrameGraph;

		return true;
	}

	return false;
}

FrameGraphPtr FrameGraphImporter::BuildFrameGraph(const UID& uid, const FrameGraphAssetPtr& frameGraphAsset) const
{
	FrameGraphPtr pFrameGraph = FrameGraphPtr::Make(m_allocator, uid);
	RHIFrameGraphPtr pRhiFrameGraph = RHIFrameGraphPtr::Make();

	auto& graph = pRhiFrameGraph->GetGraph();

	for (auto& renderTarget : frameGraphAsset->m_renderTargets)
	{
		RHI::RHITexturePtr rhiRenderTarget = RHI::Renderer::GetDriver()->CreateRenderTarget(glm::vec3(renderTarget.m_second.m_width, renderTarget.m_second.m_height, 1.0f),
			1, RHI::ETextureType::Texture2D, RHI::ETextureFormat::R8G8B8A8_SRGB, RHI::ETextureFiltration::Linear, RHI::ETextureClamping::Clamp);

		pRhiFrameGraph->SetRenderTarget(renderTarget.m_first, rhiRenderTarget);
	}

	for (auto& value : frameGraphAsset->m_values)
	{
		pRhiFrameGraph->SetValue(value.m_first, value.m_second.m_float);
	}

	for (auto& sampler : frameGraphAsset->m_samplers)
	{
		TexturePtr texture;
		App::GetSubmodule<TextureImporter>()->LoadTexture_Immediate(sampler.m_second.m_uid, texture);
		pRhiFrameGraph->SetSampler(sampler.m_first, texture);

		texture->AddHotReloadDependentObject(pFrameGraph);
	}

	for (auto& node : frameGraphAsset->m_nodes)
	{
		auto pNewNode = CreateNode(node.m_name);

		for (const auto& param : node.m_values)
		{
			if (param.m_second.IsVec4())
			{
				pNewNode->SetVectorParam(param.m_first, param.m_second.m_vec4);
			}
			else if (param.m_second.IsFloat())
			{
				pNewNode->SetVectorParam(param.m_first, glm::vec4(param.m_second.m_float));
			}
		}

		for (const auto& param : node.m_renderTargets)
		{
			if (auto pRenderTarget = pRhiFrameGraph->GetRenderTarget(param.m_first))
			{
				pNewNode->SetResourceParam(param.m_first, pRenderTarget);
			}
			else if (auto pTextureTarget = pRhiFrameGraph->GetSampler(param.m_first))
			{
				pNewNode->SetResourceParam(param.m_first, pTextureTarget->GetRHI());
			}
		}
		// TODO: Build params
		graph.Add(pNewNode);
	}

	pFrameGraph->m_frameGraph = pRhiFrameGraph;

	return pFrameGraph;
}