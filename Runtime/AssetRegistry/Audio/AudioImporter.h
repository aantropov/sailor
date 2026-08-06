#pragma once

#include "AssetRegistry/AssetFactory.h"
#include "AssetRegistry/Audio/AudioAssetInfo.h"
#include "Containers/ConcurrentMap.h"
#include "Core/SpinLock.h"
#include "Core/Submodule.h"
#include "Engine/Object.h"
#include "Memory/ObjectAllocator.hpp"
#include "Tasks/Tasks.h"

#include <atomic>
#include <string>

namespace Sailor
{
	struct AudioClipSnapshot
	{
		std::string m_sourcePath;
		uint64_t m_revision = 0;
		bool m_bStream = false;
	};

	class AudioClip final : public Object
	{
	public:
		SAILOR_API explicit AudioClip(FileId uid);
		SAILOR_API AudioClip(FileId uid, std::string sourcePath, bool bStream);

		SAILOR_API bool IsReady() const override;
		SAILOR_API uint64_t GetRevision() const;
		SAILOR_API AudioClipSnapshot GetSnapshot() const;

	private:
		void UpdateSource(std::string sourcePath, bool bStream);

		mutable SpinLock m_lock;
		std::string m_sourcePath;
		std::atomic<uint64_t> m_revision{ 0 };
		std::atomic<bool> m_bReady{ false };
		bool m_bStream = false;

		friend class AudioImporter;
	};

	class AudioImporter final :
		public TSubmodule<AudioImporter>,
		public IAssetInfoHandlerListener,
		public IAssetFactory
	{
	public:
		SAILOR_API explicit AudioImporter(AudioAssetInfoHandler* infoHandler);
		SAILOR_API ~AudioImporter() override;

		SAILOR_API void OnImportAsset(AssetInfoPtr assetInfo) override;
		SAILOR_API void OnUpdateAssetInfo(AssetInfoPtr assetInfo, bool bWasExpired) override;
		SAILOR_API bool LoadAsset(FileId uid, TObjectPtr<Object>& out, bool bImmediate = true) override;
		SAILOR_API Tasks::TaskPtr<AudioClipPtr> LoadAudioClip(FileId uid, AudioClipPtr& outClip);
		SAILOR_API bool LoadAudioClip_Immediate(FileId uid, AudioClipPtr& outClip);
		SAILOR_API void CollectGarbage() override;

	private:
		bool ImportAudioClip(FileId uid, AudioClipPtr& outClip);

		TConcurrentMap<FileId, Tasks::TaskPtr<AudioClipPtr>> m_promises{};
		TConcurrentMap<FileId, AudioClipPtr> m_loadedClips{};
		Memory::ObjectAllocatorPtr m_allocator;
	};
}
