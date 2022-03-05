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

		virtual void BeginPlay() {}
		virtual void EndPlay() {}
		virtual void Tick(float deltaTime) {}

		GameObjectPtr GetOwner() const { return m_owner; }
		WorldPtr GetWorld() const;

	protected:

		Component() = default;
		virtual ~Component() = default;

		GameObjectPtr m_owner;

		bool m_bBeginPlayCalled = false;

		friend class TObjectPtr<Component>;
		friend class GameObject;
	};
}
