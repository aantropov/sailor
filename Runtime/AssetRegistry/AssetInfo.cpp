#include "AssetInfo.h"
#include "AssetRegistry.h"

using namespace Sailor;

void ns::to_json(json& j, const Sailor::AssetInfo& p)
{
	j = json{ {"guid", p.GetGUID()} };
}

void ns::from_json(const json& j, Sailor::AssetInfo& p)
{
	j.at("guid").get_to(p.guid);
}

void AssetInfoHandler::Initialize()
{
	instance = new AssetInfoHandler();
}

void AssetInfoHandler::Serialize(const AssetInfo* inInfo, json& outData) const
{
	ns::to_json(outData, *inInfo);
}

void AssetInfoHandler::Deserialize(const json& inData, AssetInfo* outInfo) const
{
	ns::from_json(inData, *outInfo);
}

AssetInfo* AssetInfoHandler::CreateNew() const
{
	//
	return new AssetInfo();
}

AssetInfo* AssetInfoHandler::ImportFile(const std::string& filePath) const
{
}
