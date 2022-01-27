#pragma once
#include "Sailor.h"
#include "Memory/SharedPtr.hpp"
#include "JobSystem/JobSystem.h"
#include "Engine/Object.h"
#include "Components/Component.h"

namespace Sailor
{
	using GameObjectPtr = TWeakPtr<class GameObject>;
	using WorldPtr = TWeakPtr<class World>;

	class StaticMeshComponent : public Component
	{
	public:
	protected:

	};
}
