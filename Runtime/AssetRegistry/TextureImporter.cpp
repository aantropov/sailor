#include "TextureImporter.h"

#include "UID.h"
#include "AssetRegistry.h"
#include "TextureAssetInfo.h"
#include "Utils.h"
#include <filesystem>
#include <fstream>
#include <algorithm>
#include <iostream>

#include "nlohmann_json/include/nlohmann/json.hpp"
#include "JobSystem/JobSystem.h"

using namespace Sailor;

void TextureImporter::Initialize()
{
	SAILOR_PROFILE_FUNCTION();

	m_pInstance = new TextureImporter();
	TextureAssetInfoHandler::GetInstance()->Subscribe(m_pInstance);
}

TextureImporter::~TextureImporter()
{
}

void TextureImporter::OnAssetInfoUpdated(AssetInfo* assetInfo)
{
	int a;
}