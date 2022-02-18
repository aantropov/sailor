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

	class GameObject : public Object
	{
	public:

		virtual void BeginPlay() {}
		virtual void EndPlay();
		virtual void Update(float deltaTime) {}

		void SetName(std::string name) { m_name = std::move(name); }
		const std::string& GetName() const { return m_name; }

		WorldPtr GetWorld() const { return m_pWorld; }

		operator bool() const { return !bPendingDestroy; }

		template<typename TComponent, typename... TArgs >
		TObjectPtr<TComponent> AddComponent(TArgs&& ... args)
		{
			assert(m_pWorld);
			auto newObject = TObjectPtr<TComponent>::Make(m_pWorld->GetAllocator(), std::forward(args));
			assert(newObject);

			newObject->m_pWorld = this;
			m_components.Add(newObject);

			return newObject;
		}

		bool RemoveComponent(ComponentPtr component);
		void RemoveAllComponents();

		Math::Transform& GetTransform();

	protected:

		size_t m_transformHandle = (size_t)(-1);

		// Only world can create GameObject
		GameObject(WorldPtr world, const std::string& name);

		std::string m_name;

		bool bBeginPlayCalled = false;
		bool bPendingDestroy = false;
		WorldPtr m_pWorld;

		TVector<ComponentPtr> m_components;

		friend GameObjectPtr;
		friend class World;
	};
}
