#pragma once
#include "Sailor.h"
#include "Memory/SharedPtr.hpp"
#include "JobSystem/JobSystem.h"
#include "Engine/Object.h"

namespace Sailor
{
	using GameObjectPtr = TWeakPtr<class GameObject>;
	class World;

	class GameObject : public Object
	{
	public:
		
	protected:

		bool bPendingDestroy = false;
		TWeakPtr<World> m_world;

		friend class World;
	};
}
