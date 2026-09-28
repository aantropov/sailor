#include "Sailor.h"
#include "AssetRegistry/AssetRegistry.h"
#include "AssetRegistry/Audio/AudioImporter.h"
#include "Audio/AudioSystem.h"
#include "Components/AudioSourceComponent.h"
#include "ECS/AudioECS.h"
#include "ECS/TransformECS.h"
#include "Engine/GameObject.h"
#include "Engine/World.h"

#include <array>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <latch>
#include <stdexcept>

using namespace Sailor;

namespace
{
	void Require(bool condition, const char* message)
	{
		if (!condition) throw std::runtime_error(message);
	}

	void WriteWave(const std::filesystem::path& path, uint32_t frames = 4800)
	{
		std::array<uint8_t, 44> header{};
		auto word = [&](size_t offset, uint32_t value, size_t size)
		{
			for (size_t i = 0; i < size; ++i) header[offset + i] = uint8_t(value >> (i * 8));
		};
		std::memcpy(header.data(), "RIFF", 4);
		std::memcpy(header.data() + 8, "WAVEfmt ", 8);
		std::memcpy(header.data() + 36, "data", 4);
		word(4, 36 + frames * 2, 4);
		word(16, 16, 4);
		word(20, 1, 2);
		word(22, 1, 2);
		word(24, 48000, 4);
		word(28, 96000, 4);
		word(32, 2, 2);
		word(34, 16, 2);
		word(40, frames * 2, 4);
		std::ofstream output(path, std::ios::binary);
		output.write(reinterpret_cast<const char*>(header.data()), header.size());
		for (uint32_t i = 0; i < frames; ++i)
		{
			const uint16_t sample = uint16_t(i % 64 < 32 ? 8192 : -8192);
			const char bytes[]{ char(sample & 0xff), char(sample >> 8) };
			output.write(bytes, 2);
		}
		Require(static_cast<bool>(output), "PCM WAV fixture must be written");
	}

	class HoldAudioQueue
	{
	public:
		HoldAudioQueue()
		{
			m_task = Tasks::CreateTask("Hold test audio queue", [this]()
				{
					m_entered.count_down();
					m_release.wait();
				}, EThreadType::Audio);
			m_task->Run();
			m_entered.wait();
		}
		~HoldAudioQueue() { Release(); m_task->Wait(); }
		void Release()
		{
			if (!m_released) { m_released = true; m_release.count_down(); }
		}
	private:
		Tasks::ITaskPtr m_task;
		std::latch m_entered{ 1 }, m_release{ 1 };
		bool m_released = false;
	};

	class VoiceFixture
	{
	public:
		explicit VoiceFixture(AudioSystem& system) : audio(system) {}
		~VoiceFixture()
		{
			for (const auto id : ids) audio.DestroyVoice(id);
			audio.Flush();
		}
		AudioVoiceId Create(const AudioClipPtr& clip)
		{
			AudioVoiceId id = InvalidAudioVoiceId;
			Require(audio.CreateVoice(clip, id) && id != InvalidAudioVoiceId, "voice creation request must be accepted");
			ids.Add(id);
			return id;
		}
		AudioSystem& audio;
		TVector<AudioVoiceId> ids;
	};

	AudioClipPtr LoadClip(const std::filesystem::path& path, bool stream = false)
	{
		auto* registry = App::GetSubmodule<AssetRegistry>();
		const auto id = registry->GetOrLoadFile(path.string());
		if (stream)
		{
			const auto metadata = registry->GetAssetInfoPtr(id)->GetMetaFilepath();
			auto document = YAML::LoadFile(metadata);
			document["stream"] = true;
			{
				std::ofstream output(metadata);
				output << document;
			}
			Require(App::UpdateAsset(id.ToString().c_str()), "streaming metadata must update through the registry");
		}
		AudioClipPtr clip;
		Require(App::GetSubmodule<AudioImporter>()->LoadAudioClip_Immediate(id, clip) &&
			clip->GetSnapshot().m_bStream == stream, "real audio importer must load the requested source mode");
		return clip;
	}

	AudioVoiceSettings CustomSettings()
	{
		AudioVoiceSettings settings;
		settings.m_volume = 0.25f;
		settings.m_pitch = 0.5f;
		settings.m_minDistance = 3.0f;
		settings.m_maxDistance = 25.0f;
		settings.m_bLoop = true;
		settings.m_bSpatial = false;
		return settings;
	}

	void TestFailedCreation(const std::filesystem::path& workspace, AudioSystem& audio, bool stream, bool corrupt)
	{
		const auto path = workspace / "Content" / (std::string("AudioFailed") + (stream ? "Stream" : "Decoded") +
			(corrupt ? "Corrupt.wav" : "Missing.wav"));
		WriteWave(path);
		auto clip = LoadClip(path, stream);
		const size_t before = audio.GetNumVoices();
		VoiceFixture voices(audio);
		HoldAudioQueue hold;
		const auto voice = voices.Create(clip);
		Require(audio.PlayVoice(voice, true), "play before backend creation must be accepted");
		if (corrupt)
		{
			std::ofstream output(path, std::ios::binary);
			output << "not a sound";
		}
		else
		{
			Require(std::filesystem::remove(path), "only the owned WAV fixture must be removed before creation");
		}
		hold.Release();
		audio.Flush();
		Require(audio.GetNumVoices() == before && !audio.PlayVoice(voice) &&
			audio.GetVoiceClipRevision(voice) == 0 && !audio.IsVoicePlaying(voice),
			"failed backend creation must remove the ghost voice and reject later play");
		AudioVoiceSettings settings;
		Require(!audio.GetVoiceSettings(voice, settings) && !audio.SetVoiceSettings(voice, CustomSettings()) &&
			!audio.SetVoiceTransform(voice, {}) && !audio.StopVoice(voice), "failed IDs must reject all voice commands");
		WriteWave(path);
		const auto retry = voices.Create(clip);
		Require(retry != voice && audio.SetVoiceSettings(retry, CustomSettings()) && audio.PlayVoice(retry, true),
			"a repaired source must accept an explicit new creation request");
		audio.Flush();
		Require(audio.GetNumVoices() == before + 1 && audio.IsVoicePlaying(retry) &&
			audio.GetVoiceSettings(retry, settings) && settings == CustomSettings(),
			"repair must initialize the real backend and apply queued settings");
	}

	void TestPendingCommands(const std::filesystem::path& workspace, AudioSystem& audio)
	{
		const auto path = workspace / "Content/AudioPending.wav";
		WriteWave(path);
		auto clip = LoadClip(path);
		const size_t before = audio.GetNumVoices();
		VoiceFixture voices(audio);
		HoldAudioQueue hold;
		const auto first = voices.Create(clip);
		const auto cancelled = voices.Create(clip);
		const auto stopped = voices.Create(clip);
		AudioVoiceSettings settings;
		AudioTransformState transform;
		transform.m_position = glm::vec3(7, 3, -4);
		transform.m_velocity = glm::vec3(2, 0, 1);
		Require(!audio.GetVoiceSettings(first, settings), "pending creation must not claim applied backend settings");
		Require(audio.SetVoiceSettings(first, CustomSettings()) && audio.SetVoiceTransform(first, transform) &&
			audio.PlayVoice(first, true) && audio.PlayVoice(cancelled, true) &&
			audio.PlayVoice(stopped, true) && audio.StopVoice(stopped), "commands must queue behind pending creation");
		audio.DestroyVoice(cancelled);
		audio.DestroyVoice(cancelled);
		Require(!audio.PlayVoice(cancelled), "destroyed pending IDs must reject later commands immediately");
		hold.Release();
		audio.Flush();
		AudioTransformState applied;
		Require(audio.GetNumVoices() == before + 2 && audio.GetVoiceSettings(first, settings) &&
			settings == CustomSettings() && audio.IsVoicePlaying(first) && audio.GetVoiceTransform(first, applied) &&
			applied.m_position == transform.m_position && applied.m_velocity == transform.m_velocity,
			"pending command order must preserve settings, transform and play intent");
		Require(!audio.GetVoiceSettings(cancelled, settings) && !audio.IsVoicePlaying(cancelled) &&
			audio.GetVoiceClipRevision(cancelled) == 0, "pending destroy must not resurrect public voice state");
		Require(audio.GetVoiceSettings(stopped, settings) && !audio.IsVoicePlaying(stopped),
			"stop queued before creation completes must win over the preceding play");
		Require(audio.StopVoice(first), "initialized voice must accept stop");
		audio.Flush();
		Require(!audio.IsVoicePlaying(first), "backend stop must be acknowledged");
	}

	class AudioWorld final : public World
	{
	public:
		AudioWorld() : World("Audio runtime tests", uint8_t(EWorldBehaviourBit::CallBeginPlay), Systems()) {}
		~AudioWorld() override
		{
			Clear();
			App::GetSubmodule<AudioSystem>()->Flush();
		}
		void TickAudio()
		{
			++m_currentFrame;
			BeginPlayEcs();
			TickGameObjects(0.5f);
			GetECS<TransformECS>()->Tick(0.5f);
			GetECS<TransformECS>()->PostTick();
			GetECS<AudioECS>()->Tick(0.5f);
		}
		AudioVoiceId Voice() { return GetECS<AudioECS>()->GetComponentData(0).GetVoiceId(); }
	private:
		static TVector<ECS::TBaseSystemPtr> Systems()
		{
			TVector<ECS::TBaseSystemPtr> systems;
			systems.Add(TUniquePtr<TransformECS>::Make());
			systems.Add(TUniquePtr<AudioECS>::Make());
			return systems;
		}
	};

	void ApplySettings(AudioSourceComponent& source)
	{
		const auto settings = CustomSettings();
		source.SetVolume(settings.m_volume);
		source.SetPitch(settings.m_pitch);
		source.SetMinDistance(settings.m_minDistance);
		source.SetMaxDistance(settings.m_maxDistance);
		source.SetLoop(settings.m_bLoop);
		source.SetSpatial(settings.m_bSpatial);
	}

	void TestEcsReload(const std::filesystem::path& workspace, AudioSystem& audio, bool stream)
	{
		const auto path = workspace / "Content" / (stream ? "AudioEcsStream.wav" : "AudioEcsDecoded.wav");
		WriteWave(path);
		auto clip = LoadClip(path, stream);
		const size_t before = audio.GetNumVoices();
		AudioWorld world;
		auto owner = world.Instantiate("Audio reload owner");
		auto source = owner->AddComponent<AudioSourceComponent>();
		source->SetAutoPlay(false);
		source->SetClip(clip);
		ApplySettings(*source);
		owner->GetTransformComponent().SetPosition(glm::vec3(7, 3, -4));
		source->Play();
		world.TickAudio();
		audio.Flush();
		AudioVoiceSettings settings;
		Require(source->IsValid() && audio.GetVoiceSettings(world.Voice(), settings) && settings == CustomSettings() &&
			source->IsPlaying(), "initial ECS creation must apply authored settings and play intent");
		for (uint32_t frames : { 9600u, 14400u })
		{
			const auto oldVoice = world.Voice();
			const auto revision = clip->GetRevision();
			WriteWave(path, frames);
			Require(App::UpdateAsset(clip->GetFileId().ToString().c_str()) && clip->GetRevision() > revision &&
				source->GetClip() == clip && !world.GetECS<AudioECS>()->GetComponentData(0).IsDirty(),
				"real importer reload must update the same clip without dirtying authored source settings");
			world.TickAudio();
			audio.Flush();
			Require(world.Voice() != oldVoice && audio.GetVoiceClipRevision(oldVoice) == 0 &&
				audio.GetNumVoices() == before + 1 && audio.GetVoiceSettings(world.Voice(), settings) &&
				settings == CustomSettings(), "ECS clip reload must reapply all settings to the replacement backend voice");
			AudioTransformState transform;
			Require(audio.GetVoiceTransform(world.Voice(), transform) && transform.m_position == glm::vec3(7, 3, -4) &&
				transform.m_velocity == glm::vec3{} && source->IsPlaying() == (frames == 9600),
				"recreation must resubmit unchanged transform and preserve requested or stopped playback");
			source->Stop();
			world.TickAudio();
			audio.Flush();
		}
		const auto stoppedVoice = world.Voice();
		for (uint32_t i = 0; i < 16; ++i) world.TickAudio();
		audio.Flush();
		Require(world.Voice() == stoppedVoice && !source->IsPlaying(), "clean ticks must retain stopped voice identity");
		source->SetClip({});
		world.TickAudio();
		audio.Flush();
		Require(world.Voice() == InvalidAudioVoiceId && audio.GetNumVoices() == before, "clearing a source must destroy its voice");
		source->SetClip(clip);
		world.TickAudio();
		audio.Flush();
		Require(audio.GetVoiceSettings(world.Voice(), settings) && settings == CustomSettings() && !source->IsPlaying(),
			"reattaching a stopped clip must initialize settings without starting playback");
		world.Clear();
		world.Clear();
		audio.Flush();
		Require(!owner && !source && audio.GetNumVoices() == before, "world clear must release real backend voices");
	}

	void TestEcsRetry(const std::filesystem::path& workspace, AudioSystem& audio)
	{
		const auto path = workspace / "Content/AudioEcsRetry.wav";
		WriteWave(path);
		auto clip = LoadClip(path);
		const size_t before = audio.GetNumVoices();
		AudioWorld world;
		auto owner = world.Instantiate("Audio retry owner");
		auto source = owner->AddComponent<AudioSourceComponent>();
		source->SetAutoPlay(false);
		source->SetClip(clip);
		ApplySettings(*source);
		source->Play();
		Require(std::filesystem::remove(path), "retry test must remove its owned WAV");
		world.TickAudio();
		audio.Flush();
		const auto failed = world.Voice();
		Require(failed != InvalidAudioVoiceId && audio.GetVoiceClipRevision(failed) == 0,
			"ECS failure fixture must reach native voice creation failure");
		for (uint32_t i = 0; i < 16; ++i)
		{
			world.TickAudio();
			Require(world.Voice() == InvalidAudioVoiceId, "a failed revision must not retry voice creation each frame");
		}
		audio.Flush();
		WriteWave(path);
		world.TickAudio();
		Require(world.Voice() == InvalidAudioVoiceId, "a repaired file alone must not bypass explicit retry policy");
		source->Play();
		world.TickAudio();
		audio.Flush();
		AudioVoiceSettings settings;
		Require(world.Voice() != InvalidAudioVoiceId && world.Voice() != failed && source->IsPlaying() &&
			audio.GetNumVoices() == before + 1 && audio.GetVoiceSettings(world.Voice(), settings) && settings == CustomSettings(),
			"explicit Play must retry the same clip revision and fully initialize the new voice");

		source->SetClip({});
		world.TickAudio();
		audio.Flush();
		Require(std::filesystem::remove(path), "retry revision test must remove its owned WAV");
		source->SetClip(clip);
		world.TickAudio();
		audio.Flush();
		source->Play();
		world.TickAudio();
		audio.Flush();
		for (uint32_t i = 0; i < 16; ++i)
		{
			world.TickAudio();
			Require(world.Voice() == InvalidAudioVoiceId, "a failed explicit Play must not turn into per-frame retries");
		}
		const auto revision = clip->GetRevision();
		WriteWave(path, 9600);
		Require(App::UpdateAsset(clip->GetFileId().ToString().c_str()) && clip->GetRevision() > revision,
			"audio retry must observe a real importer revision change");
		world.TickAudio();
		audio.Flush();
		Require(source->IsPlaying() && audio.GetVoiceSettings(world.Voice(), settings) && settings == CustomSettings(),
			"a changed clip revision must retry without another Play request");
	}

	void TestEcsAutoplayAndClear(const std::filesystem::path& workspace, AudioSystem& audio)
	{
		const auto path = workspace / "Content/AudioManySources.wav";
		WriteWave(path);
		auto clip = LoadClip(path);
		const size_t before = audio.GetNumVoices();
		AudioWorld world;
		TVector<TObjectPtr<AudioSourceComponent>> sources;
		for (uint32_t i = 0; i < 16; ++i)
		{
			auto owner = world.Instantiate("Audio clear owner");
			auto source = owner->AddComponent<AudioSourceComponent>();
			source->SetAutoPlay(i % 2 == 0);
			source->SetClip(clip);
			ApplySettings(*source);
			sources.Add(source);
		}
		world.TickAudio();
		audio.Flush();
		Require(audio.GetNumVoices() == before + sources.Num(), "each ECS source must create one voice");
		for (size_t i = 0; i < sources.Num(); ++i)
		{
			AudioVoiceSettings settings;
			Require(sources[i]->IsPlaying() == (i % 2 == 0) && audio.GetVoiceSettings(
				world.GetECS<AudioECS>()->GetComponentData(i).GetVoiceId(), settings) && settings == CustomSettings(),
				"autoplay selection and backend settings must stay independent for each source");
		}
		world.Clear();
		audio.Flush();
		Require(audio.GetNumVoices() == before, "bulk clear must release all registered voices");
		for (const auto& source : sources) Require(!source, "bulk clear must invalidate every source handle");
		auto replacement = world.Instantiate("Audio after clear")->AddComponent<AudioSourceComponent>();
		replacement->SetClip(clip);
		world.TickAudio();
		audio.Flush();
		AudioVoiceSettings settings;
		Require(replacement->IsPlaying() && audio.GetVoiceSettings(world.Voice(), settings) &&
			settings == AudioVoiceSettings{}, "a reused ECS slot must initialize new default settings and autoplay");
	}
}

namespace Sailor::Tests
{
	void RunAudioCommandTests(const std::filesystem::path& workspace)
	{
		auto* audio = App::GetSubmodule<AudioSystem>();
		Require(audio && audio->IsInitialized() && audio->IsUsingNullDevice(),
			"audio command tests require the real initialized null-device backend");
		std::string failures;
		auto run = [&](const char* name, auto test)
		{
			try { test(); }
			catch (const std::exception& error) { failures += std::string(name) + ": " + error.what() + '\n'; }
		};
		for (bool stream : { false, true })
		{
			run(stream ? "Missing stream" : "Missing decoded", [&]() { TestFailedCreation(workspace, *audio, stream, false); });
			run(stream ? "Corrupt stream" : "Corrupt decoded", [&]() { TestFailedCreation(workspace, *audio, stream, true); });
			run(stream ? "Streaming ECS reload" : "Decoded ECS reload", [&]() { TestEcsReload(workspace, *audio, stream); });
		}
		run("Pending commands", [&]() { TestPendingCommands(workspace, *audio); });
		run("ECS explicit retry", [&]() { TestEcsRetry(workspace, *audio); });
		run("ECS autoplay and clear", [&]() { TestEcsAutoplayAndClear(workspace, *audio); });
		if (!failures.empty()) throw std::runtime_error(failures);
		std::cout << "Audio backend creation and ECS reload tests passed\n";
	}
}
