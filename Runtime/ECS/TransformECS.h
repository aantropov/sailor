#pragma once
#include "Sailor.h"
#include "Memory/SharedPtr.hpp"
#include "Memory/ObjectPtr.hpp"
#include "JobSystem/JobSystem.h"
#include "Engine/Object.h"
#include "ECS/ECS.h"
#include "Components/Component.h"
#include "Math/Transform.h"
#include "Memory/Memory.h"

namespace Sailor
{
	using WorldPtr = class World*;
	using GameObjectPtr = TObjectPtr<class GameObject>;

	class Transform
	{
	public:

		SAILOR_API __forceinline const glm::mat4x4& GetCachedRelativeMatrix() const { return m_cachedRelativeMatrix; }
		SAILOR_API __forceinline const glm::mat4x4& GetCachedWorldMatrix() const { return m_cachedWorldMatrix; }

		SAILOR_API __forceinline const Math::Transform& GetTransform() const { return m_transform; }
		SAILOR_API __forceinline size_t GetParent() const { return m_parent; }
		SAILOR_API __forceinline const TVector<size_t, Memory::TInlineAllocator<4 * sizeof(size_t)>>& GetChildren() const { return m_children; }

		__forceinline ObjectPtr& GetOwner() { return m_owner; }

		SAILOR_API __forceinline void SetPosition(const glm::vec4& position);
		SAILOR_API __forceinline void SetRotation(const glm::quat& quat);
		SAILOR_API __forceinline void SetScale(const glm::vec4& scale);
		SAILOR_API __forceinline void SetOwner(const ObjectPtr& owner);

	protected:

		SAILOR_API __forceinline void MarkDirty();

		glm::mat4x4 m_cachedRelativeMatrix;
		glm::mat4x4 m_cachedWorldMatrix;

		Math::Transform m_transform;
		size_t m_index = ECS::InvalidIndex;
		bool m_bIsDirty : 1 = false;
		bool m_bIsActive : 1 = false;
		size_t m_parent;
		TVector<size_t, Memory::TInlineAllocator<4 * sizeof(size_t)>> m_children;
		ObjectPtr m_owner;

		friend class TransformECS;
	};

	class SAILOR_API TransformECS : public ECS::TSystem<TransformECS, Transform>
	{
	public:

		virtual JobSystem::ITaskPtr Tick(float deltaTime) override;

		void MarkDirty(Transform* ptr);
		void CalculateMatrices(Transform& root);

	protected:

		TVector<size_t> m_dirtyComponents;
	};
}