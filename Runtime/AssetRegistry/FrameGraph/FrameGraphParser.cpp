#include "FrameGraphParser.h"
#include "FrameGraphAssetInfo.h"
#include "AssetRegistry/UID.h"
#include "AssetRegistry/AssetRegistry.h"
#include "Core/Utils.h"
#include <filesystem>
#include <fstream>
#include <algorithm>
#include <iostream>
#include "FrameGraph/FrameGraphNode.h"

#include "nlohmann_json/include/nlohmann/json.hpp"
#include "Tasks/Scheduler.h"
#include "RHI/Renderer.h"
#include "RHI/Texture.h"
#include "RHI/Surface.h"
#include "AssetRegistry/Texture/TextureImporter.h"

using namespace Sailor;

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

FrameGraphPtr FrameGraphImporter::BuildFrameGraph(const UID& uid, const FrameGraphAssetPtr& frameGraphAsset) const
{
	FrameGraphPtr pFrameGraph = FrameGraphPtr::Make(m_allocator, uid);
	RHI::RHIFrameGraphPtr pRhiFrameGraph = RHI::RHIFrameGraphPtr::Make();

	auto& graph = pRhiFrameGraph->GetGraph();

	for (auto& renderTarget : frameGraphAsset->m_renderTargets)
	{
		if (renderTarget.m_second.m_bIsSurface && App::GetSubmodule<RHI::Renderer>()->GetMsaaSamples() != RHI::EMsaaSamples::Samples_1)
		{
			RHI::RHISurfacePtr rhiSurface = RHI::Renderer::GetDriver()->CreateSurface(glm::vec3(renderTarget.m_second.m_width, renderTarget.m_second.m_height, 1.0f),
				1, RHI::ETextureType::Texture2D, renderTarget.m_second.m_format, RHI::ETextureFiltration::Linear, RHI::ETextureClamping::Clamp);

			pRhiFrameGraph->SetSurface(renderTarget.m_first, rhiSurface);
			pRhiFrameGraph->SetRenderTarget(renderTarget.m_first, rhiSurface->GetResolved());
		}
		else
		{
			RHI::RHITexturePtr rhiRenderTarget = RHI::Renderer::GetDriver()->CreateRenderTarget(glm::vec3(renderTarget.m_second.m_width, renderTarget.m_second.m_height, 1.0f),
				1, RHI::ETextureType::Texture2D, renderTarget.m_second.m_format, RHI::ETextureFiltration::Linear, RHI::ETextureClamping::Clamp);

			pRhiFrameGraph->SetRenderTarget(renderTarget.m_first, rhiRenderTarget);
		}
	}

	for (auto& value : frameGraphAsset->m_values)
	{
		pRhiFrameGraph->SetValue(value.m_first, value.m_second.m_float);
	}

	for (auto& sampler : frameGraphAsset->m_samplers)
	{
		TexturePtr texture;
		if (sampler.m_second.m_uid)
		{
			App::GetSubmodule<TextureImporter>()->LoadTexture_Immediate(sampler.m_second.m_uid, texture);
		}
		else
		{
			if (auto assetInfo = App::GetSubmodule<AssetRegistry>()->GetAssetInfoPtr(sampler.m_second.m_path))
			{
				App::GetSubmodule<TextureImporter>()->LoadTexture_Immediate(assetInfo->GetUID(), texture);
			}
		}
		pRhiFrameGraph->SetSampler(sampler.m_first, texture);
	}

	for (auto& node : frameGraphAsset->m_nodes)
	{
		auto pNewNode = App::GetSubmodule<FrameGraphBuilder>()->CreateNode(node.m_name);

		if (!pNewNode)
		{
			SAILOR_LOG("FrameGraph Node %s is not implemented!", node.m_name.c_str());
			continue;
		}

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
			else if (param.m_second.IsString())
			{
				pNewNode->SetStringParam(param.m_first, param.m_second.m_string);
			}
		}

		for (const auto& param : node.m_renderTargets)
		{
			if (auto pRenderTarget = pRhiFrameGraph->GetRenderTarget(param.m_second))
			{
				pNewNode->SetResourceParam(param.m_first, pRenderTarget);
			}
			else if (auto pTextureTarget = pRhiFrameGraph->GetSampler(param.m_second))
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