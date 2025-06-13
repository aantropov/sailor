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

SAILOR_API void SetAnimation(const AnimationPtr& animation);
SAILOR_API void Play();
SAILOR_API void Stop();
SAILOR_API void SetPlaySpeed(float speed);
SAILOR_API void SetPlayMode(EAnimationPlayMode mode);

SAILOR_API __forceinline AnimatorComponentData& GetData();
SAILOR_API __forceinline uint32_t GetSkeletonOffset() const { return GetData().m_gpuOffset; }

protected:
size_t m_handle = (size_t)(-1);
};
}

using namespace Sailor::Attributes;

REFL_AUTO(
type(Sailor::AnimatorComponent, bases<Sailor::Component>)
    , func(SetAnimation, property("animation"), SkipCDO())
    , func(Play)
    , func(Stop)
    , func(SetPlaySpeed)
    , func(SetPlayMode)
)

