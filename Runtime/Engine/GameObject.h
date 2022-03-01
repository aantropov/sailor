#pragma once
#include "Sailor.h"
#include "Memory/SharedPtr.hpp"
#include "Memory/ObjectPtr.hpp"
#include "JobSystem/JobSystem.h"
#include "Engine/Object.h"
#include "ECS/ECS.h"
#include "Components/Component.h"
#include "Engine/World.h"

namespace Sailor
{
	//using ComponentPtr = TObjectPtr<class Component>;
	using GameObjectPtr = TObjectPtr<class GameObject>;
	using WorldPtr = class World*;

	class GameObject final : public Object
	{
	public:

		SAILOR_API virtual void BeginPlay();
		SAILOR_API virtual void EndPlay();
		SAILOR_API virtual void Tick(float deltaTime);

		SAILOR_API void SetName(std::string name) { m_name = std::move(name); }
		SAILOR_API const std::string& GetName() const { return m_name; }

		SAILOR_API WorldPtr GetWorld() const { return m_pWorld; }

		SAILOR_API operator bool() const { return !bPendingDestroy; }

		template<typename TComponent, typename... TArgs >
		SAILOR_API TObjectPtr<TComponent> AddComponent(TArgs&& ... args)
		{
			assert(m_pWorld);
			auto newObject = TObjectPtr<TComponent>::Make(m_pWorld->GetAllocator(), std::forward<TArgs>(args) ...);
			assert(newObject);

			newObject->m_owner = m_self;
			m_components.Add(newObject);

			newObject->BeginPlay();

			return newObject;
		}

		SAILOR_API bool RemoveComponent(ComponentPtr component);
		SAILOR_API void RemoveAllComponents();

		SAILOR_API __forceinline class Transform& GetTransform();

	protected:

		size_t m_transformHandle = (size_t)(-1);

		// Only world can create GameObject
		GameObject(WorldPtr world, const std::string& name);

		std::string m_name;

		bool bBeginPlayCalled = false;
		bool bPendingDestroy = false;
		WorldPtr m_pWorld;
		GameObjectPtr m_self;

		TVector<ComponentPtr> m_components;

		friend GameObjectPtr;
		friend class World;
	};
}
