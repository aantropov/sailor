#pragma once
#include "Sailor.h"
#include "Memory/SharedPtr.hpp"
#include "Memory/ObjectPtr.hpp"
#include "Tasks/Scheduler.h"
#include "Engine/Object.h"
#include "Engine/Types.h"
#include "ECS/ECS.h"
#include "Components/Component.h"
#include "Engine/World.h"

namespace Sailor
{
	class GameObject final : public Object
	{
	public:

		SAILOR_API virtual void BeginPlay();
		SAILOR_API virtual void EndPlay();
		SAILOR_API virtual void Tick(float deltaTime);

		SAILOR_API void SetName(std::string name) { m_name = std::move(name); }
		SAILOR_API const std::string& GetName() const { return m_name; }

		SAILOR_API WorldPtr GetWorld() const { return m_pWorld; }

		SAILOR_API explicit operator bool() const { return !m_bPendingDestroy; }

		template<typename TComponent, typename... TArgs >
		SAILOR_API TObjectPtr<TComponent> AddComponent(TArgs&& ... args)
		{
			check(m_pWorld);
			auto newObject = TObjectPtr<TComponent>::Make(m_pWorld->GetAllocator(), std::forward<TArgs>(args) ...);

			newObject->m_owner = m_self;
			m_components.Add(newObject);

			if (m_bBeginPlayCalled)
			{
				newObject->BeginPlay();
				newObject->m_bBeginPlayCalled = true;
			}

			return newObject;
		}

		template<typename TComponent>
		SAILOR_API TObjectPtr<TComponent> GetComponent()
		{
			for (auto& el : m_components)
			{
				if (auto res = el.DynamicCast<TComponent>())
				{
					return res;
				}
			}

			return TObjectPtr<TComponent>();
		}

		SAILOR_API bool RemoveComponent(ComponentPtr component);
		SAILOR_API void RemoveAllComponents();

		SAILOR_API __forceinline class TransformComponent& GetTransformComponent();

		SAILOR_API __forceinline EMobilityType GetMobilityType() const { return m_type; }
		SAILOR_API __forceinline void SetMobilityType(EMobilityType type) { m_type = type; }

		SAILOR_API size_t GetFrameLastChange() const { return m_frameLastChange; }

	protected:

		size_t m_transformHandle = (size_t)(-1);

		EMobilityType m_type = EMobilityType::Stationary;

		// Only world can create GameObject
		GameObject(WorldPtr world, const std::string& name);

		std::string m_name;

		bool m_bBeginPlayCalled = false;
		bool m_bPendingDestroy = false;
		WorldPtr m_pWorld;
		GameObjectPtr m_self;

		TVector<ComponentPtr> m_components;

		size_t m_frameLastChange = 0;

		friend GameObjectPtr;
		friend class World;
		friend class ECS::TBaseSystem;
	};
}
