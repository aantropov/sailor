#pragma once

#include "AssetRegistry/Audio/AudioImporter.h"
#include "Components/Component.h"
#include "ECS/AudioECS.h"

namespace Sailor
{
	class AudioSourceComponent final : public Component
	{
		SAILOR_REFLECTABLE(AudioSourceComponent)

	public:
		SAILOR_API void Initialize() override;
		SAILOR_API void BeginPlay() override;
		SAILOR_API void EndPlay() override;

		SAILOR_API const AudioClipPtr& GetClip() const;
		SAILOR_API void SetClip(const AudioClipPtr& clip);
		SAILOR_API bool GetAutoPlay() const { return m_bAutoPlay; }
		SAILOR_API void SetAutoPlay(bool value) { m_bAutoPlay = value; }
		SAILOR_API bool GetLoop() const;
		SAILOR_API void SetLoop(bool value);
		SAILOR_API bool GetSpatial() const;
		SAILOR_API void SetSpatial(bool value);
		SAILOR_API float GetVolume() const;
		SAILOR_API void SetVolume(float value);
		SAILOR_API float GetPitch() const;
		SAILOR_API void SetPitch(float value);
		SAILOR_API float GetMinDistance() const;
		SAILOR_API void SetMinDistance(float value);
		SAILOR_API float GetMaxDistance() const;
		SAILOR_API void SetMaxDistance(float value);

		SAILOR_API void Play();
		SAILOR_API void Stop();
		SAILOR_API bool IsPlaying() const;

	private:
		AudioSourceData& GetData();
		const AudioSourceData& GetData() const;
		void MarkSettingsDirty();

		size_t m_handle = ECS::InvalidIndex;
		bool m_bAutoPlay = true;
	};
}

using namespace Sailor::Attributes;

REFL_AUTO(
	type(Sailor::AudioSourceComponent, bases<Sailor::Component>),

	func(GetClip, property("clip"), SkipCDO()),
	func(SetClip, property("clip"), SkipCDO()),

	func(GetAutoPlay, property("autoPlay")),
	func(SetAutoPlay, property("autoPlay")),

	func(GetLoop, property("loop"), SkipCDO()),
	func(SetLoop, property("loop"), SkipCDO()),

	func(GetSpatial, property("spatial"), SkipCDO()),
	func(SetSpatial, property("spatial"), SkipCDO()),

	func(GetVolume, property("volume"), SkipCDO(), Range(0.0, 4.0)),
	func(SetVolume, property("volume"), SkipCDO()),

	func(GetPitch, property("pitch"), SkipCDO(), Range(0.01, 4.0)),
	func(SetPitch, property("pitch"), SkipCDO()),

	func(GetMinDistance, property("minDistance"), SkipCDO(), Range(0.01, 10000.0)),
	func(SetMinDistance, property("minDistance"), SkipCDO()),

	func(GetMaxDistance, property("maxDistance"), SkipCDO(), Range(0.01, 100000.0)),
	func(SetMaxDistance, property("maxDistance"), SkipCDO())
)
