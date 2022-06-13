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
