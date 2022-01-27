#pragma once
#include "Sailor.h"
#include "Memory/SharedPtr.hpp"
#include "JobSystem/JobSystem.h"
#include "Engine/Object.h"

namespace Sailor
{
	using GameObjectPtr = TWeakPtr<class GameObject>;
	using WorldPtr = TWeakPtr<class World>;
	using ComponentPtr = TSharedPtr<class Component>;
	
	// All components are tracked
	class Component : public Object
	{
	public:

		virtual void BeginPlay() {}
		virtual void EndPlay() {}

		virtual void Update() {}
	};
}
