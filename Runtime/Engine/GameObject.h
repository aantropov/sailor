#pragma once
#include "Sailor.h"
#include "Memory/SharedPtr.hpp"
#include "Memory/ObjectPtr.hpp"
#include "JobSystem/JobSystem.h"
#include "Engine/Object.h"
#include "Components/Component.h"

namespace Sailor
{
	//using ComponentPtr = TObjectPtr<class Component>;
	using GameObjectPtr = TObjectPtr<class GameObject>;
	using WorldPtr = TWeakPtr<class World>;

	class GameObject : public Object
	{
	public:

		virtual void BeginPlay() {}
		virtual void EndPlay() {}
		virtual void Update(float deltaTime) {}

		void SetName(std::string name) { m_name = std::move(name); }
		const std::string& GetName() const { return m_name; }

		WorldPtr GetWorld() const { return m_world; }

		operator bool() const { return !bPendingDestroy; }

		template<typename TComponent, typename... TArgs >
		TObjectPtr<TComponent> AddComponent(TArgs&& ... args)
		{
			assert(m_world);
			auto newObject = TObjectPtr<TComponent>::Make(m_world.Lock()->GetAllocator(), std::forward(args));
			assert(newObject);

			newObject->m_world = this;
			m_components.Add(newObject);

			return newObject;
		}

		bool RemoveComponent(ComponentPtr component);
		void RemoveAllComponents();

	protected:

		// Only world can create GameObject
		GameObject() : m_name("Untitled") {}

		std::string m_name;

		bool bBeginPlayCalled = false;
		bool bPendingDestroy = false;
		WorldPtr m_world;

		TVector<ComponentPtr> m_components;

		friend class World;

		//friend class TObjectPtr<T>;
	};
}
