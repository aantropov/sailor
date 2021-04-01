#include "ShaderCache.h"

using namespace Sailor;

void ns::to_json(json& j, const Sailor::ShaderCacheEntry& p)
{
}

void ns::from_json(const json& j, Sailor::ShaderCacheEntry& p)
{
}

void ns::to_json(json& j, const Sailor::ShaderCacheData& p)
{
}

void ns::from_json(const json& j, Sailor::ShaderCacheData& p)
{
}

void ShaderCache::Initialize()
{
	instance = new ShaderCache();
}

void ShaderCache::Load()
{
}

void ShaderCache::ClearAll()
{
}

void ShaderCache::ClearExpired()
{
}