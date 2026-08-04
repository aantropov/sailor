#include "AnimationControllerImporter.h"
#include "AssetRegistry/AssetRegistry.h"
#include "YamlExceptionBoundary.h"

using namespace Sailor;

namespace
{
	template<typename TAsset>
	bool ReadYamlAsset(FileId uid, TAsset& outAsset, TVector<std::string>& outErrors)
	{
		outErrors.Clear();
		AssetInfoPtr assetInfo = App::GetSubmodule<AssetRegistry>()->GetAssetInfoPtr(uid);
		if (!assetInfo)
		{
			outErrors.Add("Asset metadata was not found.");
			return false;
		}

		std::string source;
		if (!AssetRegistry::ReadAllTextFile(assetInfo->GetAssetFilepath(), source))
		{
			outErrors.Add("Asset source could not be read.");
			return false;
		}

		YAML::Node document;
		std::string diagnostic;
		if (!External::TryLoadYaml(source, document, diagnostic) ||
			!External::GuardYamlExceptions(
				[&outAsset, &document]()
				{
					outAsset.Deserialize(document);
				},
				diagnostic))
		{
			outErrors.Add("YAML: " + diagnostic);
			return false;
		}
		return true;
	}
}

AnimationControllerImporter::AnimationControllerImporter(
	AnimationControllerAssetInfoHandler* controllerInfoHandler,
	AnimationSetAssetInfoHandler* setInfoHandler)
{
	m_allocator = Memory::ObjectAllocatorPtr::Make(
		Memory::EAllocationPolicy::SharedMemory_MultiThreaded);
	controllerInfoHandler->Subscribe(this);
	setInfoHandler->Subscribe(this);
}

AnimationControllerImporter::~AnimationControllerImporter()
{
	for (auto& controller : m_loadedControllers)
	{
		controller.m_second.DestroyObject(m_allocator);
	}
	for (auto& set : m_loadedSets)
	{
		set.m_second.DestroyObject(m_allocator);
	}
}

void AnimationControllerImporter::OnImportAsset(AssetInfoPtr)
{
}

void AnimationControllerImporter::OnUpdateAssetInfo(
	AssetInfoPtr assetInfo,
	bool bWasExpired)
{
	if (!bWasExpired || !assetInfo)
	{
		return;
	}

	const FileId uid = assetInfo->GetFileId();
	if (auto controllerIt = m_loadedControllers.Find(uid);
		controllerIt != m_loadedControllers.end())
	{
		AnimationControllerAsset definition;
		TVector<std::string> errors;
		if (ReadControllerAsset(uid, definition, errors) &&
			(*controllerIt).m_second->Initialize(definition, &errors))
		{
#ifdef SAILOR_EDITOR
			(*controllerIt).m_second->TraceHotReload(nullptr);
#endif
		}
		else
		{
			LogErrors(uid, "animation controller", errors);
		}
	}

	if (auto setIt = m_loadedSets.Find(uid); setIt != m_loadedSets.end())
	{
		AnimationSetAsset definition;
		TVector<std::string> errors;
		if (ReadAnimationSetAsset(uid, definition, errors) &&
			(*setIt).m_second->Initialize(definition, &errors))
		{
#ifdef SAILOR_EDITOR
			(*setIt).m_second->TraceHotReload(nullptr);
#endif
		}
		else
		{
			LogErrors(uid, "animation set", errors);
		}
	}
}

bool AnimationControllerImporter::LoadAsset(
	FileId uid,
	TObjectPtr<Object>& out,
	bool)
{
	AssetInfoPtr assetInfo = App::GetSubmodule<AssetRegistry>()->GetAssetInfoPtr(uid);
	if (dynamic_cast<AnimationControllerAssetInfoPtr>(assetInfo))
	{
		AnimationControllerPtr controller;
		const bool bLoaded = LoadController_Immediate(uid, controller);
		out = controller;
		return bLoaded;
	}
	if (dynamic_cast<AnimationSetAssetInfoPtr>(assetInfo))
	{
		AnimationSetPtr set;
		const bool bLoaded = LoadAnimationSet_Immediate(uid, set);
		out = set;
		return bLoaded;
	}
	out.Clear();
	return false;
}

bool AnimationControllerImporter::LoadController_Immediate(
	FileId uid,
	AnimationControllerPtr& outController)
{
	if (auto it = m_loadedControllers.Find(uid); it != m_loadedControllers.end())
	{
		outController = (*it).m_second;
		return true;
	}

	AnimationControllerAsset definition;
	TVector<std::string> errors;
	if (!ReadControllerAsset(uid, definition, errors))
	{
		LogErrors(uid, "animation controller", errors);
		return false;
	}

	auto controller = AnimationControllerPtr::Make(m_allocator, uid);
	if (!controller->Initialize(definition, &errors))
	{
		LogErrors(uid, "animation controller", errors);
		controller.DestroyObject(m_allocator);
		return false;
	}

	auto& cached = m_loadedControllers.At_Lock(uid, AnimationControllerPtr{});
	if (!cached)
	{
		cached = controller;
	}
	else
	{
		controller.DestroyObject(m_allocator);
	}
	outController = cached;
	m_loadedControllers.Unlock(uid);
	return true;
}

bool AnimationControllerImporter::LoadAnimationSet_Immediate(
	FileId uid,
	AnimationSetPtr& outSet)
{
	if (auto it = m_loadedSets.Find(uid); it != m_loadedSets.end())
	{
		outSet = (*it).m_second;
		return true;
	}

	AnimationSetAsset definition;
	TVector<std::string> errors;
	if (!ReadAnimationSetAsset(uid, definition, errors))
	{
		LogErrors(uid, "animation set", errors);
		return false;
	}

	auto set = AnimationSetPtr::Make(m_allocator, uid);
	if (!set->Initialize(definition, &errors))
	{
		LogErrors(uid, "animation set", errors);
		set.DestroyObject(m_allocator);
		return false;
	}

	auto& cached = m_loadedSets.At_Lock(uid, AnimationSetPtr{});
	if (!cached)
	{
		cached = set;
	}
	else
	{
		set.DestroyObject(m_allocator);
	}
	outSet = cached;
	m_loadedSets.Unlock(uid);
	return true;
}

void AnimationControllerImporter::CollectGarbage()
{
}

bool AnimationControllerImporter::ReadControllerAsset(
	FileId uid,
	AnimationControllerAsset& outAsset,
	TVector<std::string>& outErrors) const
{
	return ReadYamlAsset(uid, outAsset, outErrors);
}

bool AnimationControllerImporter::ReadAnimationSetAsset(
	FileId uid,
	AnimationSetAsset& outAsset,
	TVector<std::string>& outErrors) const
{
	return ReadYamlAsset(uid, outAsset, outErrors);
}

void AnimationControllerImporter::LogErrors(
	FileId uid,
	const char* assetKind,
	const TVector<std::string>& errors) const
{
	if (errors.IsEmpty())
	{
		SAILOR_LOG_ERROR("Cannot load %s '%s'.", assetKind, uid.ToString().c_str());
		return;
	}
	for (const auto& error : errors)
	{
		SAILOR_LOG_ERROR(
			"Cannot load %s '%s': %s",
			assetKind,
			uid.ToString().c_str(),
			error.c_str());
	}
}
