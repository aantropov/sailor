#pragma once

#include "AnimationController.h"
#include "AnimationControllerAssetInfo.h"
#include "AssetRegistry/AssetFactory.h"
#include "Containers/ConcurrentMap.h"
#include "Core/Submodule.h"
#include "Memory/ObjectAllocator.hpp"

namespace Sailor
{
	class AnimationControllerImporter final :
		public TSubmodule<AnimationControllerImporter>,
		public IAssetInfoHandlerListener,
		public IAssetFactory
	{
	public:
		SAILOR_API AnimationControllerImporter(
			AnimationControllerAssetInfoHandler* controllerInfoHandler,
			AnimationSetAssetInfoHandler* setInfoHandler);
		SAILOR_API ~AnimationControllerImporter() override;

		SAILOR_API void OnImportAsset(AssetInfoPtr assetInfo) override;
		SAILOR_API void OnUpdateAssetInfo(AssetInfoPtr assetInfo, bool bWasExpired) override;
		SAILOR_API bool LoadAsset(FileId uid, TObjectPtr<Object>& out, bool bImmediate = true) override;

		SAILOR_API bool LoadController_Immediate(
			FileId uid,
			AnimationControllerPtr& outController);
		SAILOR_API bool LoadAnimationSet_Immediate(
			FileId uid,
			AnimationSetPtr& outSet);
		SAILOR_API void CollectGarbage() override;

	private:
		bool ReadControllerAsset(
			FileId uid,
			AnimationControllerAsset& outAsset,
			TVector<std::string>& outErrors) const;
		bool ReadAnimationSetAsset(
			FileId uid,
			AnimationSetAsset& outAsset,
			TVector<std::string>& outErrors) const;
		void LogErrors(FileId uid, const char* assetKind, const TVector<std::string>& errors) const;

		TConcurrentMap<FileId, AnimationControllerPtr> m_loadedControllers;
		TConcurrentMap<FileId, AnimationSetPtr> m_loadedSets;
		Memory::ObjectAllocatorPtr m_allocator;
	};
}
