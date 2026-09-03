#include "AssetRegistry/AssetRegistry.h"
#include "AssetRegistry/AssetRegistryInternal.h"

#include "AssetRegistry/Animation/AnimationAssetInfo.h"
#include "AssetRegistry/Animation/AnimationControllerAssetInfo.h"
#include "AssetRegistry/AssetInfo.h"
#include "AssetRegistry/FrameGraph/FrameGraphAssetInfo.h"
#include "AssetRegistry/Landscape/LandscapeVegetationAsset.h"
#include "AssetRegistry/Material/MaterialAssetInfo.h"
#include "AssetRegistry/Model/ModelAssetInfo.h"
#include "AssetRegistry/Prefab/PrefabAssetInfo.h"
#include "AssetRegistry/Shader/ShaderAssetInfo.h"
#include "AssetRegistry/Texture/TextureAssetInfo.h"
#include "AssetRegistry/World/WorldPrefabAssetInfo.h"
#include "Core/Utils.h"
#include "Tasks/Scheduler.h"

#include <ctime>
#include <filesystem>
#include <fstream>
#include <string>

using namespace Sailor;
using namespace Sailor::AssetRegistryInternal;

bool Sailor::g_bUseLazyAssetInfoLoading = false;

std::string AssetRegistry::GetContentFolder()
{
	return GetWorkspaceContentFolder();
}

std::string AssetRegistry::GetWorkspaceContentFolder()
{
	return AsFolderPath(App::GetWorkspaceContext().GetContent());
}

std::string AssetRegistry::GetEngineContentFolder()
{
	return AsFolderPath(App::GetWorkspaceContext().GetEngineContent());
}

std::string AssetRegistry::GetCacheFolder()
{
	return AsFolderPath(App::GetWorkspaceContext().GetCache());
}

AssetRegistry::AssetRegistry() : AssetRegistry(App::GetWorkspaceContext(), App::GetSubmodule<Tasks::Scheduler>())
{
}

AssetRegistry::AssetRegistry(const Workspace::WorkspaceContext& workspaceContext)
	: AssetRegistry(workspaceContext, App::GetSubmodule<Tasks::Scheduler>())
{
}

AssetRegistry::AssetRegistry(const Workspace::WorkspaceContext& workspaceContext, Tasks::Scheduler* scheduler)
	: m_workspaceContext(workspaceContext), m_scheduler(scheduler)
{
	m_contentMounts = {AssetMountDescriptor{m_workspaceContext.GetEngineContent(), EAssetMountKind::Engine, 0, false},
		AssetMountDescriptor{m_workspaceContext.GetContent(), EAssetMountKind::Workspace, 100, true}};
	m_assetCache.Initialize(m_workspaceContext);
}

bool AssetRegistry::IsAssetExpired(const AssetInfoPtr info) const
{
	return m_assetCache.IsExpired(info);
}

bool AssetRegistry::ReadAllTextFile(const std::string& filename, std::string& text)
{
	SAILOR_PROFILE_FUNCTION();

	constexpr auto readSize = std::size_t{4096};
	auto stream = std::ifstream{filename.data()};
	if (!stream.is_open())
	{
		return false;
	}

	text.clear();
	auto buffer = std::string(readSize, '\0');
	while (stream.read(buffer.data(), readSize))
	{
		text.append(buffer, 0, stream.gcount());
	}
	text.append(buffer, 0, stream.gcount());
	if (stream.bad())
	{
		text.clear();
		return false;
	}
	return true;
}

bool AssetRegistry::ResolveContentFile(const std::string& virtualPath, AssetReadLocation& outLocation) const
{
	if (!IsSafeVirtualPath(virtualPath))
	{
		return false;
	}

	const auto winner = m_contentFileWinners.Find(VirtualPathKey(virtualPath));
	if (winner == m_contentFileWinners.end())
	{
		return false;
	}
	outLocation = winner.Value();
	return true;
}

bool AssetRegistry::ReadContentText(const std::string& virtualPath, std::string& outText) const
{
	AssetReadLocation location;
	return ResolveContentFile(virtualPath, location) && ReadAllTextFile(location.m_physicalPath.string(), outText);
}

bool AssetRegistry::GetContentFileModificationTime(const std::string& virtualPath, std::time_t& outTimestamp) const
{
	AssetReadLocation location;
	if (!ResolveContentFile(virtualPath, location))
	{
		return false;
	}
	outTimestamp = Utils::GetFileModificationTime(location.m_physicalPath.string());
	return true;
}

bool AssetRegistry::ResolveWorkspaceContentPathForWrite(const std::string& virtualPath,
	std::filesystem::path& outPath) const
{
	if (!IsSafeVirtualPath(virtualPath))
	{
		return false;
	}

	std::error_code error;
	const std::filesystem::path root = m_workspaceContext.GetContent();
	outPath = std::filesystem::weakly_canonical(root / std::filesystem::path(virtualPath), error);
	return !error && IsInside(root, outPath);
}

IAssetInfoHandler* AssetRegistry::GetAssetInfoHandler(const std::string& extension) const
{
	IAssetInfoHandler* handler = App::GetSubmodule<DefaultAssetInfoHandler>();
	auto handlerIt = m_assetInfoHandlers.Find(Lowercase(extension));
	if (handlerIt != m_assetInfoHandlers.end())
	{
		handler = *(*handlerIt).m_second;
	}
	return handler;
}

IAssetInfoHandler* AssetRegistry::GetAssetInfoHandler(const std::string& extension,
	const std::string& assetInfoType,
	bool bPrimary) const
{
	if (bPrimary || assetInfoType.empty())
	{
		return GetAssetInfoHandler(extension);
	}
	if (assetInfoType == "Sailor::AnimationAssetInfo")
	{
		return App::GetSubmodule<AnimationAssetInfoHandler>();
	}
	if (assetInfoType == "Sailor::AnimationControllerAssetInfo")
	{
		return App::GetSubmodule<AnimationControllerAssetInfoHandler>();
	}
	if (assetInfoType == "Sailor::AnimationSetAssetInfo")
	{
		return App::GetSubmodule<AnimationSetAssetInfoHandler>();
	}
	if (assetInfoType == "Sailor::FrameGraphAssetInfo")
	{
		return App::GetSubmodule<FrameGraphAssetInfoHandler>();
	}
	if (assetInfoType == "Sailor::LandscapeVegetationAssetInfo")
	{
		return App::GetSubmodule<LandscapeVegetationAssetInfoHandler>();
	}
	if (assetInfoType == "Sailor::MaterialAssetInfo")
	{
		return App::GetSubmodule<MaterialAssetInfoHandler>();
	}
	if (assetInfoType == "Sailor::ModelAssetInfo")
	{
		return App::GetSubmodule<ModelAssetInfoHandler>();
	}
	if (assetInfoType == "Sailor::PrefabAssetInfo")
	{
		return App::GetSubmodule<PrefabAssetInfoHandler>();
	}
	if (assetInfoType == "Sailor::ShaderAssetInfo")
	{
		return App::GetSubmodule<ShaderAssetInfoHandler>();
	}
	if (assetInfoType == "Sailor::TextureAssetInfo")
	{
		return App::GetSubmodule<TextureAssetInfoHandler>();
	}
	if (assetInfoType == "Sailor::WorldPrefabAssetInfo")
	{
		return App::GetSubmodule<WorldPrefabAssetInfoHandler>();
	}
	return GetAssetInfoHandler(extension);
}

bool AssetRegistry::RegisterAssetInfoHandler(const TVector<std::string>& supportedExtensions,
	IAssetInfoHandler* assetInfoHandler)
{
	SAILOR_PROFILE_FUNCTION();

	bool bAssigned = false;
	for (const std::string& extension : supportedExtensions)
	{
		const std::string key = Lowercase(extension);
		if (!m_assetInfoHandlers.ContainsKey(key))
		{
			m_assetInfoHandlers[key] = assetInfoHandler;
			bAssigned = true;
		}
	}
	return bAssigned;
}

void AssetRegistry::SubscribeContentChanges(IAssetRegistryContentListener* listener)
{
	if (listener != nullptr && !m_contentListeners.Contains(listener))
	{
		m_contentListeners.Add(listener);
	}
}

void AssetRegistry::UnsubscribeContentChanges(IAssetRegistryContentListener* listener)
{
	m_contentListeners.Remove(listener);
}

std::string AssetRegistry::GetMetaFilePath(const std::string& assetFilepath)
{
	return assetFilepath + "." + MetaFileExtension;
}

AssetRegistry::~AssetRegistry()
{
	m_assetCache.Shutdown();
	DeleteAssetInfos(m_loadedAssetInfo);
}
