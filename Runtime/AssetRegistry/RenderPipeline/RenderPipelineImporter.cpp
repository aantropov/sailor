#include "AssetRegistry/RenderPipeline/RenderPipelineImporter.h"

#include "AssetRegistry/UID.h"
#include "AssetRegistry/AssetRegistry.h"
#include "RenderPipelineAssetInfo.h"
#include "Core/Utils.h"
#include <filesystem>
#include <fstream>
#include <algorithm>
#include <iostream>

#include "nlohmann_json/include/nlohmann/json.hpp"
#include "JobSystem/JobSystem.h"

using namespace Sailor;

void RenderPipelineImporter::Initialize()
{
	SAILOR_PROFILE_FUNCTION();

	m_pInstance = new RenderPipelineImporter();
	RenderPipelineAssetInfoHandler::GetInstance()->Subscribe(m_pInstance);
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
