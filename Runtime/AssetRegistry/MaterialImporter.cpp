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

using namespace Sailor;

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

bool MaterialImporter::LoadMaterial(UID uid)
{
	SAILOR_PROFILE_FUNCTION();

	return false;
}
