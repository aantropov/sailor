#include "FrameGraphImporter.h"
#include "AssetRegistry/AssetRegistry.h"
#include "AssetRegistry/Texture/TextureImporter.h"
#include "FrameGraph/FrameGraphNode.h"
#include "RHI/Renderer.h"
#include "RHI/RenderTarget.h"
#include "RHI/Surface.h"
#include "RHI/Texture.h"

#include <algorithm>
#include <cmath>

using namespace Sailor;

FrameGraphImporter::FrameGraphImporter(FrameGraphAssetInfoHandler* infoHandler)
{
	SAILOR_PROFILE_FUNCTION();
	m_allocator = ObjectAllocatorPtr::Make(EAllocationPolicy::SharedMemory_MultiThreaded);
	infoHandler->Subscribe(this);
}

FrameGraphImporter::~FrameGraphImporter()
{
	for (auto& instance : m_loadedFrameGraphs)
	{
		instance.m_second.DestroyObject(m_allocator);
	}
}

void FrameGraphImporter::OnImportAsset(AssetInfoPtr assetInfo)
{
}

void FrameGraphImporter::OnUpdateAssetInfo(AssetInfoPtr assetInfo, bool bWasExpired)
{
}

FrameGraphAssetPtr FrameGraphImporter::LoadFrameGraphAsset(FileId uid)
{
	SAILOR_PROFILE_FUNCTION();

	if (FrameGraphAssetInfoPtr assetInfo = dynamic_cast<FrameGraphAssetInfoPtr>(App::GetSubmodule<AssetRegistry>()->GetAssetInfoPtr(uid)))
	{
		SAILOR_PROFILE_TEXT(assetInfo->GetAssetFilepath().c_str());

		const std::string& filepath = assetInfo->GetAssetFilepath();

		std::string text;

		AssetRegistry::ReadAllTextFile(filepath, text);

		YAML::Node yamlNode = YAML::Load(text);

		FrameGraphAsset* frameGraphAsset = new FrameGraphAsset();
		frameGraphAsset->Deserialize(yamlNode);

		return FrameGraphAssetPtr(frameGraphAsset);
	}

	SAILOR_LOG("Cannot find frameGraph asset info with FileId: %s", uid.ToString().c_str());
	return FrameGraphAssetPtr();
}

bool FrameGraphImporter::LoadFrameGraph_Immediate(FileId uid, FrameGraphPtr& outFrameGraph)
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

		m_loadedFrameGraphs.At_Lock(uid) = outFrameGraph = pFrameGraph;
		m_loadedFrameGraphs.Unlock(uid);

		return true;
	}

	return false;
}

FrameGraphPtr FrameGraphImporter::BuildFrameGraph(const FileId& uid, const FrameGraphAssetPtr& frameGraphAsset) const
{
	FrameGraphPtr pFrameGraph = FrameGraphPtr::Make(m_allocator, uid);
	RHI::RHIFrameGraphPtr pRhiFrameGraph = RHI::RHIFrameGraphPtr::Make();

	auto& graph = pRhiFrameGraph->GetGraph();

	for (const auto& renderTarget : frameGraphAsset->m_renderTargets)
	{
		const bool bUsedWithComputeShaders = renderTarget.m_second->m_bIsCompatibleWithComputeShaders;
		const bool bShouldGenerateMips = renderTarget.m_second->m_bGenerateMips;
		const bool bIsDepthFormat = RHI::IsDepthFormat(renderTarget.m_second->m_format);

		const RHI::ETextureUsageFlags defaultUsage = (bIsDepthFormat ? RHI::ETextureUsageBit::DepthStencilAttachment_Bit : RHI::ETextureUsageBit::ColorAttachment_Bit) |
			RHI::ETextureUsageBit::TextureTransferSrc_Bit |
			RHI::ETextureUsageBit::TextureTransferDst_Bit |
			RHI::ETextureUsageBit::Sampled_Bit |
			(bUsedWithComputeShaders ? RHI::ETextureUsageBit::Storage_Bit : 0);

		const uint32_t maxExtent = std::max(renderTarget.m_second->m_width, renderTarget.m_second->m_height);
		const uint32_t numMips = std::min(renderTarget.m_second->m_maxMipLevel, bShouldGenerateMips ? (uint32_t)std::floor(std::log2f((float)maxExtent)) + 1 : 1u);
		const RHI::ETextureFiltration filtration = renderTarget.m_second->m_filtration;
		const RHI::ETextureClamping clamping = renderTarget.m_second->m_clamping;
		const  RHI::ESamplerReductionMode reduction = renderTarget.m_second->m_reduction;

		if (renderTarget.m_second->m_bIsSurface)
		{
			RHI::RHISurfacePtr rhiSurface = RHI::Renderer::GetDriver()->CreateSurface(glm::vec2(renderTarget.m_second->m_width, renderTarget.m_second->m_height),
				numMips, renderTarget.m_second->m_format, filtration, clamping, defaultUsage);

			pRhiFrameGraph->SetSurface(renderTarget.m_first, rhiSurface);
			pRhiFrameGraph->SetRenderTarget(renderTarget.m_first, rhiSurface->GetResolved());

			RHI::Renderer::GetDriver()->SetDebugName(rhiSurface->GetTarget(), renderTarget.m_first + " Target");
			RHI::Renderer::GetDriver()->SetDebugName(rhiSurface->GetResolved(), renderTarget.m_first + " Resolved");
		}
		else
		{
			RHI::RHIRenderTargetPtr rhiRenderTarget = RHI::Renderer::GetDriver()->CreateRenderTarget(glm::vec2(renderTarget.m_second->m_width, renderTarget.m_second->m_height),
				numMips, renderTarget.m_second->m_format, filtration, clamping, defaultUsage, reduction);

			pRhiFrameGraph->SetRenderTarget(renderTarget.m_first, rhiRenderTarget);

			RHI::Renderer::GetDriver()->SetDebugName(rhiRenderTarget, renderTarget.m_first);
		}
	}

	for (const auto& value : frameGraphAsset->m_values)
	{
		if (value.m_second->IsVec4())
		{
			pRhiFrameGraph->SetValue(value.m_first, value.m_second->GetVec4());
		}
		else if (value.m_second->IsFloat())
		{
			pRhiFrameGraph->SetValue(value.m_first, value.m_second->GetFloat());
		}
	}

	for (const auto& sampler : frameGraphAsset->m_samplers)
	{
		TexturePtr texture;
		if (sampler.m_second->m_fileId)
		{
			App::GetSubmodule<TextureImporter>()->LoadTexture_Immediate(sampler.m_second->m_fileId, texture);
		}
		else
		{
			if (auto assetInfo = App::GetSubmodule<AssetRegistry>()->GetAssetInfoPtr(sampler.m_second->m_path))
			{
				App::GetSubmodule<TextureImporter>()->LoadTexture_Immediate(assetInfo->GetFileId(), texture);
			}
		}

		check(texture);
		pRhiFrameGraph->SetSampler(sampler.m_first, texture->GetRHI());
	}

	for (auto& node : frameGraphAsset->m_nodes)
	{
		auto pNewNode = App::GetSubmodule<FrameGraphBuilder>()->CreateNode(node.m_name);

		if (!pNewNode)
		{
			SAILOR_LOG("FrameGraph Node %s is not implemented!", node.m_name.c_str());
			continue;
		}

		pNewNode->SetTag(node.m_tag.empty() ? node.m_name : node.m_tag);

		for (const auto& param : node.m_values)
		{
			if (param.m_second->IsVec4())
			{
				pNewNode->SetVec4(param.m_first, param.m_second->GetVec4());
			}
			else if (param.m_second->IsFloat())
			{
				pNewNode->SetFloat(param.m_first, param.m_second->GetFloat());
			}
			else if (param.m_second->IsString())
			{
				pNewNode->SetString(param.m_first, param.m_second->GetString());
			}
		}

		for (const auto& param : node.m_renderTargets)
		{
			if (auto pSurface = pRhiFrameGraph->GetSurface(*param.m_second))
			{
				pNewNode->SetRHIResource(param.m_first, pSurface);
			}
			else if (auto pRenderTarget = pRhiFrameGraph->GetRenderTarget(*param.m_second))
			{
				pNewNode->SetRHIResource(param.m_first, pRenderTarget);
			}
			else if (auto pTextureTarget = pRhiFrameGraph->GetSampler(*param.m_second))
			{
				pNewNode->SetRHIResource(param.m_first, pTextureTarget);
			}
			else
			{
				// Resolve per-frame resources, such as DepthBuffer and BackBuffer,
				// when the frame is recorded.
				pNewNode->SetRHIResource_Unresolved(param.m_first, *param.m_second);
			}
		}
		// TODO: Build params
		graph.Add(pNewNode);
	}

	pFrameGraph->m_frameGraph = pRhiFrameGraph;

	return pFrameGraph;
}

bool FrameGraphImporter::Instantiate_Immediate(FileId uid, FrameGraphPtr& outFrameGraph)
{
	if (auto pFrameGraphAsset = LoadFrameGraphAsset(uid))
	{
		FrameGraphPtr pFrameGraph = BuildFrameGraph(uid, pFrameGraphAsset);
		outFrameGraph = pFrameGraph;

		return outFrameGraph.IsValid();
	}

	return false;
}
