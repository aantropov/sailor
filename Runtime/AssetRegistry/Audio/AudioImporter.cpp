#include "AssetRegistry/Audio/AudioImporter.h"
#include "AssetRegistry/AssetRegistry.h"

#include <filesystem>

using namespace Sailor;

AudioClip::AudioClip(FileId uid) : Object(std::move(uid))
{}

AudioClip::AudioClip(FileId uid, std::string sourcePath, bool bStream) :
	Object(std::move(uid))
{
	UpdateSource(std::move(sourcePath), bStream);
}

bool AudioClip::IsReady() const
{
	return m_bReady.load(std::memory_order_acquire);
}

uint64_t AudioClip::GetRevision() const
{
	return m_revision.load(std::memory_order_acquire);
}

AudioClipSnapshot AudioClip::GetSnapshot() const
{
	m_lock.Lock();
	AudioClipSnapshot snapshot;
	snapshot.m_sourcePath = m_sourcePath;
	snapshot.m_bStream = m_bStream;
	snapshot.m_revision = m_revision.load(std::memory_order_relaxed);
	m_lock.Unlock();
	return snapshot;
}

void AudioClip::UpdateSource(std::string sourcePath, bool bStream)
{
	m_lock.Lock();
	m_sourcePath = std::move(sourcePath);
	m_bStream = bStream;
	m_revision.fetch_add(1, std::memory_order_release);
	m_bReady.store(!m_sourcePath.empty(), std::memory_order_release);
	m_lock.Unlock();
}

AudioImporter::AudioImporter(AudioAssetInfoHandler* infoHandler)
{
	m_allocator = Memory::ObjectAllocatorPtr::Make(
		Memory::EAllocationPolicy::SharedMemory_MultiThreaded);
	infoHandler->Subscribe(this);
}

AudioImporter::~AudioImporter()
{
	for (auto& clip : m_loadedClips)
	{
		clip.m_second.DestroyObject(m_allocator);
	}
}

void AudioImporter::OnImportAsset(AssetInfoPtr)
{}

void AudioImporter::OnUpdateAssetInfo(AssetInfoPtr assetInfo, bool bWasExpired)
{
	if (!assetInfo || !bWasExpired)
	{
		return;
	}

	const FileId uid = assetInfo->GetFileId();
	if (auto it = m_loadedClips.Find(uid); it != m_loadedClips.end())
	{
		AudioClipPtr clip = (*it).m_second;
		if (clip && ImportAudioClip(uid, clip))
		{
#ifdef SAILOR_EDITOR
			clip->TraceHotReload(nullptr);
#endif
		}
	}
}

bool AudioImporter::LoadAsset(FileId uid, TObjectPtr<Object>& out, bool bImmediate)
{
	AudioClipPtr clip;
	if (bImmediate)
	{
		const bool bLoaded = LoadAudioClip_Immediate(uid, clip);
		out = clip;
		return bLoaded;
	}

	Tasks::TaskPtr<AudioClipPtr> task = LoadAudioClip(uid, clip);
	out = clip;
	return task.IsValid();
}

Tasks::TaskPtr<AudioClipPtr> AudioImporter::LoadAudioClip(FileId uid, AudioClipPtr& outClip)
{
	auto& promise = m_promises.At_Lock(uid, nullptr);
	auto& loadedClip = m_loadedClips.At_Lock(uid, AudioClipPtr{});
	if (loadedClip)
	{
		outClip = loadedClip;
		auto result = promise ? promise : Tasks::TaskPtr<AudioClipPtr>::Make(outClip);
		m_loadedClips.Unlock(uid);
		m_promises.Unlock(uid);
		return result;
	}

	if (!promise)
	{
		AudioClipPtr clip = AudioClipPtr::Make(m_allocator, uid);
		promise = Tasks::CreateTaskWithResult<AudioClipPtr>(
			"Load Audio Clip",
			[this, uid, clip]() mutable
			{
				AudioClipPtr imported = clip;
				ImportAudioClip(uid, imported);
				return imported;
			},
			EThreadType::Worker);
		outClip = loadedClip = clip;
		promise->Run();
	}
	else
	{
		outClip = loadedClip;
	}

	m_loadedClips.Unlock(uid);
	m_promises.Unlock(uid);
	return promise;
}

bool AudioImporter::LoadAudioClip_Immediate(FileId uid, AudioClipPtr& outClip)
{
	Tasks::TaskPtr<AudioClipPtr> task = LoadAudioClip(uid, outClip);
	if (!task)
	{
		return false;
	}
	task->Wait();
	return outClip && outClip->IsReady();
}

bool AudioImporter::ImportAudioClip(FileId uid, AudioClipPtr& outClip)
{
	AudioAssetInfoPtr info = App::GetSubmodule<AssetRegistry>()
		->GetAssetInfoPtr<AudioAssetInfoPtr>(uid);
	if (!info)
	{
		return false;
	}

	const std::string sourcePath = info->GetAssetFilepath();
	std::error_code error;
	if (!std::filesystem::is_regular_file(sourcePath, error) || error)
	{
		SAILOR_LOG_ERROR("Cannot load audio clip '%s'.", sourcePath.c_str());
		return false;
	}

	if (!outClip)
	{
		outClip = AudioClipPtr::Make(m_allocator, uid);
	}
	outClip->UpdateSource(sourcePath, info->ShouldStream());
	return true;
}

void AudioImporter::CollectGarbage()
{
	TVector<FileId> completed;
	m_promises.LockAll();
	const TVector<FileId> ids = m_promises.GetKeys();
	m_promises.UnlockAll();
	for (const FileId& id : ids)
	{
		Tasks::TaskPtr<AudioClipPtr> promise = m_promises.At_Lock(id);
		if (!promise || promise->IsFinished())
		{
			completed.Add(id);
		}
		m_promises.Unlock(id);
	}
	for (const FileId& id : completed)
	{
		m_promises.Remove(id);
	}
}
