#pragma once
#include "Sailor.h"
#include "Memory/SharedPtr.hpp"
#include "Memory/ObjectPtr.hpp"
#include "JobSystem/JobSystem.h"
#include "Engine/Object.h"
#include "Components/Component.h"

namespace Sailor
{
	using GameObjectPtr = TObjectPtr<class GameObject>;
	using WorldPtr = TWeakPtr<class World>;
	using TranfsormComponentPtr = TObjectPtr<class TranfsormComponent>;

	class TransformComponent : public Component
	{
	public:
	protected:

	};
}
