#pragma once
#include "Sailor.h"
#include "Engine/Object.h"
#include "ECS/ECS.h"
#include "Tasks/Scheduler.h"
#include "Components/Component.h"
#include "RHI/SceneView.h"
#include "RHI/Types.h"
#include <glm/mat4x4.hpp>

namespace Sailor
{
	class Animation;

	class AnimatorComponentData final : public ECS::TComponent
	{
	public:
		SAILOR_API __forceinline TObjectPtr<Animation>& GetAnimation() { return m_animation; }
		SAILOR_API __forceinline const TObjectPtr<Animation>& GetAnimation() const { return m_animation; }

		SAILOR_API __forceinline uint32_t GetBonesCount() const { return m_bonesCount; }
		SAILOR_API __forceinline void SetBonesCount(uint32_t numBones) { m_bonesCount = numBones; }

		uint32_t m_frameIndex = 0;
		float m_lerp = 0.0f;
		uint32_t m_gpuOffset = std::numeric_limits<uint32_t>::max();
		TVector<glm::mat4> m_currentSkeleton;

		bool m_bIsPlaying = false;
		float m_playSpeed = 1.0f;
		EAnimationPlayMode m_playMode = EAnimationPlayMode::Repeat;
		bool m_bForward = true;
		float m_currentFrame = 0.0f;

	protected:
		
		TObjectPtr<Animation> m_animation;
		uint32_t m_bonesCount = 0;

		friend class AnimationECS;
		friend class AnimationComponent;
	};

	class SAILOR_API AnimationECS : public ECS::TSystem<AnimationECS, AnimatorComponentData>
	{
	public:

		static constexpr uint32_t BonesMaxNum = 65535;

		virtual void BeginPlay() override;
		virtual void EndPlay() override;
		virtual Tasks::ITaskPtr Tick(float deltaTime) override;

		void FillAnimationData(RHI::RHISceneViewPtr& sceneView);

		RHI::RHIShaderBindingSetPtr GetBonesBinding() const { return m_bonesBinding; }

	protected:

		RHI::RHIShaderBindingSetPtr m_bonesBinding{};
		RHI::RHIShaderBindingPtr m_bonesBuffer{};
		uint32_t m_nextBoneOffset = 0;
	};

	template class ECS::TSystem<AnimationECS, AnimatorComponentData>;
}
