#include "AssetRegistry/AssetRegistry.h"
#include "Core/FileRevision.h"
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
using namespace Sailor::Workspace;

bool Sailor::g_bUseLazyAssetInfoLoading = false;

#if defined(SAILOR_FILE_IO_TEST_HOOKS)
namespace
{
	thread_local AssetRegistry::TextReadObserver g_textReadObserver;
}

AssetRegistry::TextReadObserver AssetRegistry::ExchangeTextReadObserverForTests(TextReadObserver observer)
{
	auto previous = std::move(g_textReadObserver);
	g_textReadObserver = std::move(observer);
	return previous;
}

void AssetRegistry::NotifyTextReadForTests(const std::filesystem::path& path)
{
	if (g_textReadObserver) g_textReadObserver(path);
}
#endif

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
	auto stream = std::ifstream{PathFromUtf8(filename)};
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

bool AssetRegistry::ResolveContentFile(std::string_view virtualPath, AssetReadLocation& outLocation) const
{
	if (!IsSafeVirtualPath(virtualPath))
	{
		return false;
	}

	const auto winner = m_contentFileWinners.Find(VirtualPathKey(std::string(virtualPath)));
	if (winner == m_contentFileWinners.end())
	{
		return false;
	}
	outLocation = winner.Value();
	return true;
}

bool AssetRegistry::ReadContentText(std::string_view virtualPath, std::string& outText) const
{
	AssetReadLocation location;
	return ResolveContentFile(virtualPath, location) && ReadAllTextFile(PathToUtf8(location.m_physicalPath), outText);
}

bool AssetRegistry::GetContentFileModificationTime(std::string_view virtualPath, std::time_t& outTimestamp) const
{
	AssetReadLocation location;
	if (!ResolveContentFile(virtualPath, location))
	{
		return false;
	}
	outTimestamp = Utils::GetFileModificationTime(PathToUtf8(location.m_physicalPath));
	return true;
}

bool AssetRegistry::ResolveWorkspaceContentPathForWrite(std::string_view virtualPath,
	std::filesystem::path& outPath) const
{
	if (!IsSafeVirtualPath(virtualPath))
	{
		return false;
	}

	std::error_code error;
	const std::filesystem::path root = m_workspaceContext.GetContent();
	outPath = std::filesystem::weakly_canonical(root / PathFromUtf8(virtualPath), error);
	return !error && IsInside(root, outPath);
}

IAssetInfoHandler* AssetRegistry::GetAssetInfoHandler(std::string_view extension) const
{
	IAssetInfoHandler* handler = App::GetSubmodule<DefaultAssetInfoHandler>();
	auto handlerIt = m_assetInfoHandlers.Find(Lowercase(std::string(extension)));
	if (handlerIt != m_assetInfoHandlers.end())
	{
		handler = *(*handlerIt).m_second;
	}
	return handler;
}

IAssetInfoHandler* AssetRegistry::GetAssetInfoHandler(std::string_view extension,
	std::string_view assetInfoType,
	bool bPrimary) const
{
	if (bPrimary || assetInfoType.empty())
	{
		return GetAssetInfoHandler(extension);
	}
	if (assetInfoType == "Sailor::AnimationAssetInfo")
	{
		return GetAssetInfoHandler("anim");
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

IAssetInfoHandler* AssetRegistry::GetAssetInfoHandler(const AssetInfo& info) const
{
	auto path = PathFromUtf8(info.GetMetaFilepath());
	path.replace_extension();
	return GetAssetInfoHandler(Extension(PathToUtf8(path)), info.GetAssetInfoType(),
		PathKey(path) == PathKey(PathFromUtf8(info.GetAssetFilepath())));
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

std::string AssetRegistry::GetMetaFilePath(std::string_view assetFilepath)
{
	return std::string(assetFilepath) + "." + MetaFileExtension;
}

AssetRegistry::~AssetRegistry()
{
	m_assetCache.Shutdown();
	DeleteAssetInfos(m_loadedAssetInfo);
}
