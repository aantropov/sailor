#pragma once
#include "Sailor.h"
#include "Memory/SharedPtr.hpp"
#include "Memory/ObjectPtr.hpp"
#include "JobSystem/JobSystem.h"
#include "Engine/Object.h"
#include "ECS/ECS.h"
#include "Components/Component.h"
#include "Math/Transform.h"

namespace Sailor
{
	using WorldPtr = class World*;
	using GameObjectPtr = TObjectPtr<class GameObject>;
	using TranfsormComponentPtr = TObjectPtr<class TranfsormComponent>;

	class Transform
	{
	public:

		const glm::mat4x4& GetCachedRelativeMatrix() const { return m_cachedRelativeMatrix; }
		const glm::mat4x4& GetCachedWorldMatrix() const { return m_cachedWorldMatrix; }
		
		const Math::Transform& GetTransform() const { return m_transform; }
		size_t GetParent() const { return m_parent; }
		const TVector<size_t, Memory::TInlineAllocator<4 * sizeof(size_t)>>& GetChildren() const { return m_children; }

		__forceinline void SetPosition(const glm::vec4& position);
		__forceinline void SetRotation(const glm::quat& quat);
		__forceinline void SetScale(const glm::vec4& scale);

	protected:

		glm::mat4x4 m_cachedRelativeMatrix;
		glm::mat4x4 m_cachedWorldMatrix;

		Math::Transform m_transform;
		bool m_bIsDirty : 1 = false;
		bool m_bIsActive : 1 = false;
		size_t m_parent;
		TVector<size_t, Memory::TInlineAllocator<4 * sizeof(size_t)>> m_children;

		friend class TransformECS;
	};

	class SAILOR_API TransformECS : public ECS::TSystem<Transform>
	{
	public:

		static size_t GetComponentStaticType() { return ECS::TSystem<Transform>::GetComponentStaticType(); }

		virtual void Tick(float deltaTime) override;

		void CalculateMatrices(Transform& root);

	protected:

	};
}
