#pragma once
#include "Sailor.h"
#include "Components/Component.h"
#include "ECS/AnimationECS.h"
#include "AssetRegistry/Animation/AnimationImporter.h"
#include "Engine/Types.h"

namespace Sailor
{
	class AnimatorComponent : public Component
	{
		SAILOR_REFLECTABLE(AnimatorComponent)

	public:
		SAILOR_API virtual void Initialize() override;
		SAILOR_API virtual void EndPlay() override;

		SAILOR_API const AnimationPtr& GetAnimation() const { return GetData().GetAnimation(); }
		SAILOR_API void SetAnimation(const AnimationPtr& animation);
		SAILOR_API const AnimationControllerPtr& GetController() const { return GetData().GetController(); }
		SAILOR_API void SetController(const AnimationControllerPtr& controller);
		SAILOR_API const AnimationSetPtr& GetAnimationSet() const { return GetData().GetAnimationSet(); }
		SAILOR_API void SetAnimationSet(const AnimationSetPtr& animationSet);

		SAILOR_API bool SetFloat(const std::string& name, float value);
		SAILOR_API bool SetInt(const std::string& name, int32_t value);
		SAILOR_API bool SetBool(const std::string& name, bool value);
		SAILOR_API bool SetTrigger(const std::string& name);
		SAILOR_API bool ResetTrigger(const std::string& name);

		SAILOR_API void Play();
		SAILOR_API void Stop();
		SAILOR_API void SetPlaySpeed(float speed);
		SAILOR_API void SetPlayMode(EAnimationPlayMode mode);

		SAILOR_API __forceinline AnimatorComponentData& GetData();
		SAILOR_API __forceinline const AnimatorComponentData& GetData() const;
		SAILOR_API __forceinline uint32_t GetSkeletonOffset() const;

	protected:
		size_t m_handle = (size_t)(-1);
	};
}

using namespace Sailor::Attributes;

REFL_AUTO(
	type(Sailor::AnimatorComponent, bases<Sailor::Component>),
	func(SetAnimation, property("animation"), SkipCDO()),
	func(GetAnimation, property("animation"), SkipCDO()),

	func(SetController, property("controller"), SkipCDO()),
	func(GetController, property("controller"), SkipCDO()),

	func(SetAnimationSet, property("animationSet"), SkipCDO()),
	func(GetAnimationSet, property("animationSet"), SkipCDO())
)
