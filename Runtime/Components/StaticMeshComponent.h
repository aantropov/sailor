#pragma once
#include "Sailor.h"
#include "Memory/SharedPtr.hpp"
#include "Memory/ObjectPtr.hpp"
#include "JobSystem/JobSystem.h"
#include "Engine/Object.h"
#include "Components/Component.h"

namespace Sailor
{
	using WorldPtr = class World*;
	using GameObjectPtr = TObjectPtr<class GameObject>;
	using StaticMeshComponentPtr = TObjectPtr<class StaticMeshComponent>;

	class StaticMeshComponent : public Component
	{
	public:

	protected:

	};
}
