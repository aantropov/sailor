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
#include "Tasks/Scheduler.h"

using namespace Sailor;

FrameGraphImporter::FrameGraphImporter(FrameGraphAssetInfoHandler* infoHandler)
{
	SAILOR_PROFILE_FUNCTION();
	m_allocator = ObjectAllocatorPtr::Make();
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
		outFrameGraph = m_loadedFrameGraphs[uid] = pFrameGraph;

		return true;
	}

	return false;
}

bool FrameGraphImporter::Instantiate_Immediate(UID uid, FrameGraphPtr& outFrameGraph)
{
	if (auto pFrameGraphAsset = LoadFrameGraphAsset(uid))
	{
		FrameGraphPtr pFrameGraph = BuildFrameGraph(uid, pFrameGraphAsset);
		outFrameGraph = pFrameGraph;

		return outFrameGraph.IsValid();
	}

	return false;
}