#include "AssetRegistry/RenderPipeline/RenderPipelineImporter.h"

#include "AssetRegistry/UID.h"
#include "AssetRegistry/AssetRegistry.h"
#include "RenderPipelineAssetInfo.h"
#include "Core/Utils.h"
#include <filesystem>
#include <fstream>
#include <algorithm>
#include <iostream>
#include "RHI/RenderPipeline/RenderPipelineNode.h"

#include "nlohmann_json/include/nlohmann/json.hpp"
#include "JobSystem/JobSystem.h"

using namespace Sailor;

TUniquePtr<TMap<std::string, std::function<RHI::RHIRenderPipelineNodePtr(void)>>> RenderPipelineImporter::s_pNodeFactoryMethods;

void RenderPipelineImporter::RegisterRenderPipelineNode(const std::string& nodeName, std::function<RHI::RHIRenderPipelineNodePtr(void)> factoryMethod)
{
	if (!s_pNodeFactoryMethods)
	{
		s_pNodeFactoryMethods = TUniquePtr<TMap<std::string, std::function<RHI::RHIRenderPipelineNodePtr(void)>>>::Make();
	}

	(*s_pNodeFactoryMethods)[nodeName] = factoryMethod;
}

RHI::RHIRenderPipelineNodePtr RenderPipelineImporter::CreateNode(const std::string& nodeName) const
{
	return (*s_pNodeFactoryMethods)[nodeName]();
}

RenderPipelineImporter::RenderPipelineImporter(RenderPipelineAssetInfoHandler* infoHandler)
{
	SAILOR_PROFILE_FUNCTION();

	infoHandler->Subscribe(this);		
}

RenderPipelineImporter::~RenderPipelineImporter()
{
}

void RenderPipelineImporter::OnUpdateAssetInfo(AssetInfoPtr assetInfo, bool bWasExpired)
{
}


void RenderPipelineImporter::OnImportAsset(AssetInfoPtr assetInfo)
{
}
