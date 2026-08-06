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

#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <iterator>
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

	std::string ReadText(const std::filesystem::path& path)
	{
		std::ifstream input(path, std::ios::binary);
		Require(input.is_open(), "test source should be readable: " + path.generic_string());
		return std::string(
			std::istreambuf_iterator<char>(input),
			std::istreambuf_iterator<char>());
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

	void TestAudioPlaybackSceneFixture()
	{
		const std::filesystem::path root = SAILOR_TEST_SOURCE_DIR;
		const std::filesystem::path fixtureRoot =
			root / "Content/Tests/Audio";
		const YAML::Node world = YAML::LoadFile(
			(fixtureRoot / "AudioPlayback.world").string());
		Require(
			world["name"].as<std::string>() == "AudioPlayback" &&
				world["prefabs"].IsSequence(),
			"the audio playback fixture should be a loadable world");

		bool bFoundListener = false;
		bool bFoundSource = false;
		for (const YAML::Node& prefab : world["prefabs"])
		{
			const YAML::Node components = prefab["components"];
			if (!components.IsSequence())
			{
				continue;
			}

			for (const YAML::Node& component : components)
			{
				const std::string typeName =
					component["typename"].as<std::string>();
				const YAML::Node properties = component["overrideProperties"];
				if (typeName == "Sailor::AudioListenerComponent")
				{
					bFoundListener =
						properties["enabled"].as<bool>() &&
						properties["priority"].as<int32_t>() == 0;
				}
				else if (typeName == "Sailor::AudioSourceComponent")
				{
					const YAML::Node clip = properties["clip"];
					bFoundSource =
						clip["fileId"].as<std::string>() ==
							"{A1390002-0000-4000-8000-000000000002}" &&
						properties["autoPlay"].as<bool>() &&
						properties["loop"].as<bool>() &&
						properties["spatial"].as<bool>();
				}
			}
		}
		Require(
			bFoundListener && bFoundSource,
			"the audio playback fixture should pair a listener with a spatial looping source");

		const YAML::Node metadata = YAML::LoadFile(
			(fixtureRoot / "TestTone440Hz.wav.asset").string());
		Require(
			metadata["assetInfoType"].as<std::string>() ==
				"Sailor::AudioAssetInfo" &&
				metadata["fileId"].as<std::string>() ==
					"{A1390002-0000-4000-8000-000000000002}" &&
				!metadata["stream"].as<bool>(),
			"the test tone should use typed non-streaming audio metadata");

		const std::string wav = ReadText(
			fixtureRoot / "TestTone440Hz.wav");
		Require(
			wav.size() > 44 &&
				wav.compare(0, 4, "RIFF") == 0 &&
				wav.compare(8, 4, "WAVE") == 0,
			"the audio playback fixture should contain a valid PCM WAV payload");
	}

	void TestDedicatedAudioThreadContract()
	{
		const std::filesystem::path root = SAILOR_TEST_SOURCE_DIR;
		const std::string schedulerHeader = ReadText(root / "Runtime/Tasks/Scheduler.h");
		const std::string schedulerSource = ReadText(root / "Runtime/Tasks/Scheduler.cpp");
		const std::string audioHeader = ReadText(root / "Runtime/Audio/AudioSystem.h");
		const std::string audioSource = ReadText(root / "Runtime/Audio/AudioSystem.cpp");
		const std::string appSource = ReadText(root / "Runtime/Sailor.cpp");

		Require(
			schedulerHeader.find("Audio = 7") != std::string::npos &&
				schedulerSource.find("\"Audio Thread\"") != std::string::npos,
			"Scheduler should own one dedicated Audio worker");
		Require(
			audioSource.find("EThreadType::Audio") != std::string::npos &&
				audioSource.find("Process audio commands") != std::string::npos &&
				audioSource.find("m_pendingCommands") != std::string::npos,
			"audio backend calls should be serialized through Sailor Tasks");
		Require(
			audioHeader.find("miniaudio") == std::string::npos &&
				audioHeader.find("ma_") == std::string::npos,
			"miniaudio implementation types should not leak into public engine APIs");
		Require(
			appSource.find("--null-audio") != std::string::npos &&
				appSource.find("m_bForceNullAudioDevice") != std::string::npos,
			"headless and CI launches should be able to force the null audio device");
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
		{ "AudioPlaybackSceneFixture", TestAudioPlaybackSceneFixture },
		{ "DedicatedAudioThreadContract", TestDedicatedAudioThreadContract },
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
