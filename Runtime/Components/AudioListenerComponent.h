#pragma once

#include "Components/Component.h"
#include "ECS/AudioECS.h"

namespace Sailor
{
	class AudioListenerComponent final : public Component
	{
		SAILOR_REFLECTABLE(AudioListenerComponent)

	public:
		SAILOR_API void Initialize() override;
		SAILOR_API void BeginPlay() override;
		SAILOR_API void EndPlay() override;

		SAILOR_API bool GetEnabled() const;
		SAILOR_API void SetEnabled(bool value);
		SAILOR_API int32_t GetPriority() const;
		SAILOR_API void SetPriority(int32_t value);

	private:
		AudioListenerData& GetData();
		const AudioListenerData& GetData() const;

		size_t m_handle = ECS::InvalidIndex;
	};
}

REFL_AUTO(
	type(Sailor::AudioListenerComponent, bases<Sailor::Component>),

	func(GetEnabled, property("enabled"), Sailor::Attributes::SkipCDO()),
	func(SetEnabled, property("enabled"), Sailor::Attributes::SkipCDO()),

	func(GetPriority, property("priority"), Sailor::Attributes::SkipCDO()),
	func(SetPriority, property("priority"), Sailor::Attributes::SkipCDO())
)
