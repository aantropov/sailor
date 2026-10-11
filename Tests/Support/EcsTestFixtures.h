#pragma once

#include "ECS/AnimationECS.h"
#include "ECS/StaticMeshRendererECS.h"
#include "ECS/TransformECS.h"
#include "Engine/GameObject.h"
#include "Engine/World.h"

#include <algorithm>
#include <cmath>
#include <stdexcept>
#include <string>
#include <string_view>

namespace Sailor::Tests
{
	inline void Require(bool condition, std::string_view message)
	{
		if (!condition)
		{
			throw std::runtime_error(std::string(message));
		}
	}

	inline bool AreMatricesNear(const glm::mat4& lhs, const glm::mat4& rhs, float tolerance = 0.0001f)
	{
		for (glm::length_t column = 0; column < lhs.length(); ++column)
		{
			for (glm::length_t row = 0; row < lhs[column].length(); ++row)
			{
				const float scale = std::max({ 1.0f, std::abs(lhs[column][row]), std::abs(rhs[column][row]) });
				if (std::abs(lhs[column][row] - rhs[column][row]) > tolerance * scale)
				{
					return false;
				}
			}
		}

		return true;
	}

	class AnimationLayoutTestSystem final : public AnimationECS
	{
	public:

		bool TryAllocateForTest(uint32_t numBones, uint32_t& outGpuOffset)
		{
			return TryAllocateBoneRange(numBones, m_nextBoneOffset, outGpuOffset);
		}

		uint32_t GetNextBoneOffsetForTest() const { return m_nextBoneOffset; }
		size_t GetNumSlotsForTest() const { return m_components.Num(); }
	};

	class PublishedMeshTestSystem final : public StaticMeshRendererECS
	{
	public:
		const RHI::RHISpatialSceneVersionPtr& GetSpatialVersion() const { return m_publishedSceneVersion; }
	};

	class PrefabTestWorld final : public World
	{
	public:

		explicit PrefabTestWorld(EWorldBehaviourMask mask = 0) :
			World("PrefabRollbackTests", mask, CreateEcs()) {}
		using World::DestroyPendingGameObjects;
		using World::ApplyComponentReflection;
		void ResetRemovalVisits() { m_numRemovalVisits = 0; }
		size_t GetRemovalVisits() const { return m_numRemovalVisits; }
		void AdvanceFrame() { ++m_currentFrame; }
		void TickLifecycle(float deltaTime = 0.016f)
		{
			AdvanceFrame();
			BeginPlayEcs();
			TickGameObjects(deltaTime);
		}
		size_t GetPendingDependencyCount() const { return GetNumPendingDependencyResolutions(); }
		bool RemovePrefabMetadataForTest(
			const InstanceId& rootInstanceId)
		{
			if (!m_prefabLinks.m_instances.ContainsKey(rootInstanceId))
			{
				return false;
			}

			m_prefabLinks.m_instances.Remove(rootInstanceId);
			return true;
		}

	private:

		static TVector<ECS::TBaseSystemPtr> CreateEcs()
		{
			TVector<ECS::TBaseSystemPtr> systems;
			systems.Add(TUniquePtr<TransformECS>::Make());
			systems.Add(TUniquePtr<PublishedMeshTestSystem>::Make());
			return systems;
		}
	};
}
