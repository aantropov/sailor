#pragma once
#include "Sailor.h"
#include "Memory/SharedPtr.hpp"
#include "Memory/ObjectPtr.hpp"
#include "JobSystem/JobSystem.h"
#include "Engine/Object.h"

namespace Sailor
{
	using WorldPtr = class World*;
	using GameObjectPtr = TObjectPtr<class GameObject>;
	using ComponentPtr = TObjectPtr<class Component>;

	// All components are tracked
	class Component : public Object
	{
	public:

		virtual void BeginPlay() = 0;
		virtual void EndPlay() = 0;
		virtual void Update() = 0;

		GameObjectPtr GetGameObject() const { return m_gameObject; }

	protected:

		Component() = default;
		virtual ~Component() = default;

		GameObjectPtr m_gameObject;

		friend class TObjectPtr<Component>;
	};
}
