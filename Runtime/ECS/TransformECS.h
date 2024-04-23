#pragma once
#include "Sailor.h"
#include "Engine/Object.h"
#include "Tasks/Scheduler.h"
#include "ECS/ECS.h"
#include "Components/Component.h"
#include "Memory/Memory.h"
#include "Math/Transform.h"

namespace Sailor
{
	using WorldPtr = class World*;
	using GameObjectPtr = TObjectPtr<class GameObject>;

	class TransformComponent : public ECS::TComponent
	{
	public:

		SAILOR_API __forceinline const glm::mat4x4& GetCachedRelativeMatrix() const { return m_cachedRelativeMatrix; }
		SAILOR_API __forceinline const glm::mat4x4& GetCachedWorldMatrix() const { return m_cachedWorldMatrix; }

		SAILOR_API __forceinline const Math::Transform& GetTransform() const { return m_transform; }
		SAILOR_API __forceinline size_t GetParent() const { return m_parent; }
		SAILOR_API __forceinline void SetNewParent(const TransformComponent* parent);
		SAILOR_API __forceinline const TVector<size_t, Memory::TInlineAllocator<4 * sizeof(size_t)>>& GetChildren() const { return m_children; }

		__forceinline ObjectPtr& GetOwner() { return m_owner; }

		SAILOR_API __forceinline glm::vec4 GetWorldPosition() const { return glm::vec4(m_cachedWorldMatrix[3]); }
		SAILOR_API __forceinline glm::vec3 GetForwardVector() const { return glm::mat4(m_cachedWorldMatrix) * Math::vec4_Forward; }

		SAILOR_API __forceinline void SetPosition(const glm::vec3& position);
		//SAILOR_API __forceinline void SetPosition(const glm::vec4& position);
		SAILOR_API __forceinline void SetRotation(const glm::quat& quat);
		SAILOR_API __forceinline void SetScale(const glm::vec4& scale);

		SAILOR_API __forceinline const glm::vec4& GetPosition() const { return m_transform.m_position; }
		SAILOR_API __forceinline const glm::quat& GetRotation() const { return m_transform.m_rotation; }
		SAILOR_API __forceinline const glm::vec4& GetScale() const { return m_transform.m_scale; }

		SAILOR_API virtual void MarkDirty() override;

	protected:

		glm::mat4x4 m_cachedRelativeMatrix = glm::identity<glm::mat4>();
		glm::mat4x4 m_cachedWorldMatrix = glm::identity<glm::mat4>();

		Math::Transform m_transform;
		size_t m_parent = ECS::InvalidIndex;
		size_t m_newParent = ECS::InvalidIndex;
		TVector<size_t, Memory::TInlineAllocator<4 * sizeof(size_t)>> m_children;

		friend class TransformECS;
	};

	class SAILOR_API TransformECS : public ECS::TSystem<TransformECS, TransformComponent>
	{
	public:

		virtual Tasks::ITaskPtr PostTick() override;
		virtual Tasks::ITaskPtr Tick(float deltaTime) override;

		void MarkDirty(TransformComponent* ptr);
		void CalculateMatrices(TransformComponent& root);

		virtual uint32_t GetOrder() const override { return 0; }

	protected:

		TVector<size_t> m_dirtyComponents;
	};
}
