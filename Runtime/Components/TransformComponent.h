#pragma once
#include "Sailor.h"
#include "Memory/SharedPtr.hpp"
#include "Memory/ObjectPtr.hpp"
#include "JobSystem/JobSystem.h"
#include "Engine/Object.h"
#include "Components/Component.h"
#include "Math/Transform.h"

namespace Sailor
{
	using WorldPtr = TWeakPtr<class World>;
	using GameObjectPtr = TObjectPtr<class GameObject>;
	using TranfsormComponentPtr = TObjectPtr<class TranfsormComponent>;

	class TransformComponent : public Component
	{
	public:
		Math::Transform m_transform;

	protected:

	};
}
