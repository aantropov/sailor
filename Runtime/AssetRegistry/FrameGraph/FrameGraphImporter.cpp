#include "AssetRegistry/FrameGraph/FrameGraphImporter.h"

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
			m_nodes.Add(el.get<std::string>());
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

void FrameGraphImporter::OnUpdateAssetInfo(AssetInfoPtr assetInfo, bool bWasExpired)
{
}


void FrameGraphImporter::OnImportAsset(AssetInfoPtr assetInfo)
{
}

