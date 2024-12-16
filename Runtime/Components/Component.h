#pragma once
#include "Sailor.h"
#include "Memory/SharedPtr.hpp"
#include "Memory/ObjectPtr.hpp"
#include "Tasks/Scheduler.h"
#include "Engine/Types.h"
#include "Engine/Object.h"
#include "Core/Reflection.h"

namespace Sailor
{	
	// All components are tracked
	class Component : public Object, public IReflectable
	{
		SAILOR_REFLECTABLE(Component)

	public:

		SAILOR_API virtual void Initialize() {}
		SAILOR_API virtual void BeginPlay() {}
		SAILOR_API virtual void EndPlay() {}
		SAILOR_API virtual void OnGizmo() {}
		SAILOR_API virtual void Tick(float deltaTime) {}

		SAILOR_API virtual void EditorTick(float deltaTime) {}

		SAILOR_API GameObjectPtr GetOwner() const { return m_owner; }
		SAILOR_API WorldPtr GetWorld() const;

		// Components become valid only when BeginPlay is called
		SAILOR_API virtual bool IsValid() const override { return m_bBeginPlayCalled; }

	protected:

		Component() = default;
		virtual ~Component() = default;

		GameObjectPtr m_owner;

		bool m_bBeginPlayCalled = false;

		friend class TObjectPtr<Component>;
		friend class GameObject;
		friend class World;
	};
}

REFL_AUTO(
	type(Sailor::Component, bases<Sailor::IReflectable>),
	func(GetFileId, property("fileId")),
	func(GetInstanceId, property("instanceId"))
)