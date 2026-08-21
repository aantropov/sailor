#pragma once
#include "Sailor.h"
#include "Engine/Object.h"
#include "ECS/ECS.h"
#include "Tasks/Scheduler.h"
#include "Components/Component.h"
#include "RHI/SceneView.h"
#include "RHI/Types.h"
#include "Math/Transform.h"
#include "AssetRegistry/Animation/AnimationController.h"

namespace Sailor
{
	class Animation;

	class AnimatorComponentData final : public ECS::TComponent
	{
	public:
		static constexpr uint32_t InvalidGpuOffset = (std::numeric_limits<uint32_t>::max)();

		SAILOR_API __forceinline TObjectPtr<Animation>& GetAnimation() { return m_animation; }
		SAILOR_API __forceinline const TObjectPtr<Animation>& GetAnimation() const { return m_animation; }
		SAILOR_API __forceinline const AnimationControllerPtr& GetController() const { return m_controller; }
		SAILOR_API __forceinline const AnimationSetPtr& GetAnimationSet() const { return m_animationSet; }
		SAILOR_API __forceinline AnimationControllerInstance& GetControllerInstance() { return m_controllerInstance; }
		SAILOR_API __forceinline const AnimationControllerInstance& GetControllerInstance() const { return m_controllerInstance; }

		SAILOR_API __forceinline uint32_t GetBonesCount() const { return m_bonesCount; }
		SAILOR_API __forceinline void SetBonesCount(uint32_t numBones) { m_bonesCount = numBones; }

		uint32_t m_frameIndex = 0;
		float m_lerp = 0.0f;
		uint32_t m_gpuOffset = InvalidGpuOffset;
		TVector<Math::Transform> m_currentSkeleton;
		TVector<Math::Transform> m_blendSkeleton;
		TVector<glm::mat4> m_currentMatrices;
		TVector<glm::mat4> m_globalMatrices;
		TVector<uint8_t> m_composeState;

		bool m_bIsPlaying = true;
		float m_playSpeed = 1.0f;
		EAnimationPlayMode m_playMode = EAnimationPlayMode::Repeat;
		bool m_bForward = true;
		float m_currentFrame = 0.0f;

	protected:

		TObjectPtr<Animation> m_animation;
		AnimationControllerPtr m_controller;
		AnimationSetPtr m_animationSet;
		AnimationControllerInstance m_controllerInstance;
		TVector<TObjectPtr<Animation>> m_controllerAnimations;
		TVector<uint64_t> m_controllerAnimationRevisions;
		uint64_t m_animationRevision = 0;
		uint64_t m_controllerRevision = 0;
		uint64_t m_animationSetRevision = 0;
		uint64_t m_skeletonSignature = 0;
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

		void SetAnimation(size_t componentIndex, const TObjectPtr<Animation>& animation);
		void SetController(size_t componentIndex, const AnimationControllerPtr& controller);
		void SetAnimationSet(size_t componentIndex, const AnimationSetPtr& animationSet);
		void InvalidateGpuLayout();

		static bool TryAllocateBoneRange(uint32_t numBones, uint32_t& nextBoneOffset, uint32_t& outGpuOffset);

		void FillAnimationData(RHI::RHISceneViewPtr& sceneView);

		virtual uint32_t GetOrder() const override { return 200; }

		RHI::RHIShaderBindingSetPtr GetBonesBinding() const { return m_bonesBinding; }

	protected:

		virtual void OnComponentUnregistered(size_t index, AnimatorComponentData& component) override;
		void RefreshController(size_t componentIndex, bool bResetInstance);

		RHI::RHIShaderBindingSetPtr m_bonesBinding{};
		RHI::RHIShaderBindingPtr m_bonesBuffer{};
		TVector<glm::mat4> m_cpuBoneMatrices{};
		TSharedPtr<TVector<glm::mat4>> m_publishedBoneMatrices{};
		TVector<TSharedPtr<TVector<glm::mat4>>> m_boneSnapshotPool{};
		uint64_t m_animationRevision = 0ull;
		uint32_t m_nextBoneOffset = 0;
	};

	template class ECS::TSystem<AnimationECS, AnimatorComponentData>;
}
