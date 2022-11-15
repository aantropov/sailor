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

#include "Tasks/Scheduler.h"
#include "RHI/Renderer.h"
#include "RHI/Texture.h"
#include "RHI/Surface.h"
#include "AssetRegistry/Texture/TextureImporter.h"
#include "Core/YamlSerializable.h"

using namespace Sailor;

void FrameGraphAsset::Deserialize(const YAML::Node& inData)
{
	if (inData["samplers"])
	{
		auto samplers = inData["samplers"].as<TVector<FrameGraphAsset::Resource>>();
		
		for (auto& sampler : samplers)
		{
			m_samplers[sampler.m_name] = std::move(sampler);
		}
	}

	if (inData["float"])
	{
		for (const auto& p : inData["float"])
		{
			for (const auto& keyValue : p)
			{
				const std::string key = keyValue.first.as<std::string>();
				m_values[key] = Value(keyValue.second.as<float>());
			}
		}
	}

	if (inData["vec4"])
	{
		for (const auto& p : inData["vec4"])
		{
			for (const auto& keyValue : p)
			{
				const std::string key = keyValue.first.as<std::string>();
				m_values[key] = Value(keyValue.second.as<glm::vec4>());
			}
		}
	}

	if (inData["renderTargets"])
	{
		auto targets = inData["renderTargets"].as<TVector<RenderTarget>>();

		for (auto& target : targets)
		{
			m_renderTargets[target.m_name] = std::move(target);
		}
	}

	if (inData["frame"])
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
		if (renderTarget.m_second.m_bIsSurface)
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
		pRhiFrameGraph->SetValue(value.m_first, value.m_second.GetFloat());
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
				pNewNode->SetVec4(param.m_first, param.m_second.GetVec4());
			}
			else if (param.m_second.IsFloat())
			{
				pNewNode->SetVec4(param.m_first, glm::vec4(param.m_second.GetFloat()));
			}
			else if (param.m_second.IsString())
			{
				pNewNode->SetString(param.m_first, param.m_second.GetString());
			}
		}

		for (const auto& param : node.m_renderTargets)
		{
			if (auto pSurface = pRhiFrameGraph->GetSurface(param.m_second))
			{
				pNewNode->SetRHIResource(param.m_first, pSurface);
			}
			else if (auto pRenderTarget = pRhiFrameGraph->GetRenderTarget(param.m_second))
			{
				pNewNode->SetRHIResource(param.m_first, pRenderTarget);
			}
			else if (auto pTextureTarget = pRhiFrameGraph->GetSampler(param.m_second))
			{
				pNewNode->SetRHIResource(param.m_first, pTextureTarget->GetRHI());
			}
		}
		// TODO: Build params
		graph.Add(pNewNode);
	}

	pFrameGraph->m_frameGraph = pRhiFrameGraph;

	return pFrameGraph;
}