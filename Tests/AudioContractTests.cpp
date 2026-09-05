#include "AssetRegistry/Audio/AudioAssetInfo.h"
#include "AssetRegistry/Audio/AudioImporter.h"
#include "Components/AudioListenerComponent.h"
#include "Components/AudioSourceComponent.h"
#include "Core/Reflection.h"
#include "ECS/AudioECS.h"
#include "ECS/TransformECS.h"
#include "Engine/GameObject.h"
#include "Engine/World.h"
#include "Tasks/Scheduler.h"

#include <functional>
#include <iostream>
#include <stdexcept>
#include <string>

using namespace Sailor;

namespace
{
	class AudioComponentTestWorld final : public World
	{
	public:
		AudioComponentTestWorld() :
			World("AudioComponentTests", 0, CreateEcs())
		{}

	private:
		static TVector<ECS::TBaseSystemPtr> CreateEcs()
		{
			TVector<ECS::TBaseSystemPtr> systems;
			systems.Add(TUniquePtr<TransformECS>::Make());
			systems.Add(TUniquePtr<AudioECS>::Make());
			return systems;
		}
	};

	void Require(bool condition, const std::string& message)
	{
		if (!condition)
		{
			throw std::runtime_error(message);
		}
	}


	void TestAudioClipSnapshot()
	{
		AudioClip clip(
			FileId::CreateNewFileId(),
			"Fixtures/Sound.wav",
			true);
		Require(clip.IsReady(), "an imported audio clip should expose a ready snapshot");
		Require(clip.GetRevision() == 1, "the initial audio source should create revision one");

		const AudioClipSnapshot snapshot = clip.GetSnapshot();
		Require(
			snapshot.m_sourcePath == "Fixtures/Sound.wav" &&
				snapshot.m_bStream &&
				snapshot.m_revision == 1,
			"audio clip snapshots should be immutable copies of importer state");
	}

	void TestReflectedAudioAuthoringContract()
	{
		const TypeInfo& sourceType = TypeInfo::Get<AudioSourceComponent>();
		Require(
			sourceType.Name() == "Sailor::AudioSourceComponent" &&
				sourceType.Base() == "Sailor::Component",
			"audio source should remain a reflected engine component");
		Require(
			sourceType.Properties()["clip"] == "TObjectPtr<Sailor::AudioClip>",
			"audio clip references should export their canonical Editor type: " +
				sourceType.Properties()["clip"]);
		Require(
			sourceType.Properties()["volume"] == "float" &&
				sourceType.Properties()["spatial"] == "bool" &&
				sourceType.PropertyRanges()["volume"].m_min == 0.0 &&
				sourceType.PropertyRanges()["volume"].m_max == 4.0,
			"audio source controls should export typed values and ranges");

		const TypeInfo& listenerType = TypeInfo::Get<AudioListenerComponent>();
		Require(
			listenerType.Name() == "Sailor::AudioListenerComponent" &&
				listenerType.Properties()["enabled"] == "bool" &&
				listenerType.Properties()["priority"] == "int32",
			"audio listener should expose enabled and priority authoring fields");
	}

	void TestAudioAssetInfoContract()
	{
		AudioAssetInfo info;
		const YAML::Node serialized = info.Serialize();
		Require(
			serialized["assetInfoType"].as<std::string>() ==
				"Sailor::AudioAssetInfo",
			"new audio metadata should persist the canonical asset info type");
		Require(
			serialized["stream"].IsScalar() &&
				!serialized["stream"].as<bool>(),
			"audio metadata should default to decoded, non-streaming playback");

		const YAML::Node engineTypes = Reflection::ExportEngineTypes();
		bool bFoundAudioType = false;
		for (const YAML::Node& assetType : engineTypes["assetTypes"])
		{
			if (assetType["typename"].as<std::string>() != "Sailor::AudioAssetInfo")
			{
				continue;
			}

			bFoundAudioType = true;
			const YAML::Node extensions = assetType["extensions"];
			Require(
				extensions.size() == 3 &&
					extensions[0].as<std::string>() == "wav" &&
					extensions[1].as<std::string>() == "flac" &&
					extensions[2].as<std::string>() == "mp3",
				"Editor asset discovery should recognize WAV, FLAC, and MP3");
		}
		Require(bFoundAudioType, "engine type export should include AudioAssetInfo");
	}

	void TestComponentAuthoringAndTeardown()
	{
		AudioComponentTestWorld world;
		GameObjectPtr owner = world.Instantiate("Audio owner");
		auto firstSource = owner->AddComponent<AudioSourceComponent>();
		auto secondSource = owner->AddComponent<AudioSourceComponent>();
		auto listener = owner->AddComponent<AudioListenerComponent>();

		firstSource->SetAutoPlay(false);
		firstSource->SetLoop(true);
		firstSource->SetSpatial(false);
		firstSource->SetVolume(2.0f);
		firstSource->SetPitch(0.5f);
		firstSource->SetMinDistance(3.0f);
		firstSource->SetMaxDistance(25.0f);
		Require(
			!firstSource->GetAutoPlay() &&
				firstSource->GetLoop() &&
				!firstSource->GetSpatial() &&
				firstSource->GetVolume() == 2.0f &&
				firstSource->GetPitch() == 0.5f &&
				firstSource->GetMinDistance() == 3.0f &&
				firstSource->GetMaxDistance() == 25.0f,
			"audio source authoring values should stay in their own ECS slot");
		Require(
			secondSource->GetVolume() == 1.0f && !secondSource->GetLoop(),
			"multiple audio sources on one GameObject should not share state");

		listener->SetEnabled(false);
		listener->SetPriority(42);
		Require(
			!listener->GetEnabled() && listener->GetPriority() == 42,
			"audio listener authoring should update its ECS record");

		Require(owner->RemoveComponent(firstSource),
			"an individual audio source should unregister safely");
		owner->RemoveAllComponents();
		Require(owner->GetComponents().IsEmpty(),
			"audio source/listener teardown should release every ECS handle");
		world.Clear();
	}
}

int main()
{
	const std::pair<const char*, std::function<void()>> tests[] = {
		{ "AudioClipSnapshot", TestAudioClipSnapshot },
		{ "ReflectedAudioAuthoringContract", TestReflectedAudioAuthoringContract },
		{ "AudioAssetInfoContract", TestAudioAssetInfoContract },
		{ "ComponentAuthoringAndTeardown", TestComponentAuthoringAndTeardown },
	};

	for (const auto& test : tests)
	{
		try
		{
			test.second();
			std::cout << "[PASS] " << test.first << std::endl;
		}
		catch (const std::exception& error)
		{
			std::cerr << "[FAIL] " << test.first << ": " << error.what() << std::endl;
			return 1;
		}
	}

	return 0;
}
