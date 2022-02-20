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

		glm::mat4x4 m_cachedMatrix; 
		Math::Transform m_transform;
		TVector<size_t, Memory::TInlineAllocator<4 * sizeof(size_t)>> m_children;
	};

	class SAILOR_API TransformECS : public ECS::TSystem<Transform>
	{
	public:

		static size_t GetComponentStaticType() { return ECS::TSystem<Transform>::GetComponentStaticType(); }

	protected:

	};
}
