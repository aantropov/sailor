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
	using ComponentPtr = TObjectPtr<class Component>;

	// All components are tracked
	class Component : public Object
	{
	public:

		virtual void BeginPlay() {}
		virtual void EndPlay() {}

		virtual void Update() {}

		GameObjectPtr GetGameObject() const { return m_gameObject; }

	protected:

		Component() = default;

		GameObjectPtr m_gameObject;

		friend class TObjectPtr<Component>;
	};
}
