#pragma once
#include "Sailor.h"
#include "Memory/SharedPtr.hpp"
#include "Memory/ObjectPtr.hpp"
#include "JobSystem/JobSystem.h"
#include "Engine/Object.h"

namespace Sailor
{
	using GameObjectPtr = TObjectPtr<class GameObject>;
	using WorldPtr = TWeakPtr<class World>;

	class GameObject : public Object
	{
	public:

		WorldPtr GetWorld() const { return m_world; }

	protected:

		bool bPendingDestroy = false;
		WorldPtr m_world;

		friend class World;
	};
}
