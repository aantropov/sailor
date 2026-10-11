#pragma once

#include "Components/Component.h"
#include "ECS/TransformECS.h"
#include "Engine/GameObject.h"
#include "Engine/World.h"
#include <functional>

namespace Sailor
{
	class LifecycleTestComponent final : public Component
	{
		SAILOR_REFLECTABLE(LifecycleTestComponent)

	public:

		LifecycleTestComponent() = default;

		void Initialize() override
		{
			++s_initialized;
			for (const auto& component : GetOwner()->GetComponents())
			{
				m_bPublishedAtInitialize |= component.GetRawPtr() == this;
			}
			auto* ecs = GetWorld()->GetECS<TransformECS>();
			m_ecsHandle = ecs->RegisterComponent();
			ecs->GetComponentData(m_ecsHandle).SetOwner(GetOwner());
		}

		void BeginPlay() override
		{
			++s_begun;
			++m_begins;
			m_bValidAtBegin = IsValid();
			m_valueAtBegin = m_value;
			m_dependencyAtBegin = m_dependency;
			m_externalDependencyAtBegin = m_externalDependency;
			m_parentAtBegin = GetOwner()->GetParent();
			m_positionAtBegin = GetOwner()->GetTransformComponent().GetPosition();
			// The callback may destroy this component, including its stored function.
			auto callback = m_onBegin;
			if (callback)
			{
				callback();
			}
		}

		void Tick(float) override
		{
			++m_ticks;
			auto callback = m_onTick;
			if (callback)
			{
				callback();
			}
		}

		void EditorTick(float) override { ++m_editorTicks; }

		void EndPlay() override
		{
			++s_ended;
			if (s_onEnd) s_onEnd();
			GetWorld()->GetECS<TransformECS>()->UnregisterComponent(m_ecsHandle);
			m_ecsHandle = ECS::InvalidIndex;
			auto callback = m_onEnd;
			if (callback)
			{
				callback();
			}
		}

		float GetValue() const { return m_value; }
		void SetValue(float value)
		{
			m_value = value;
			GetWorld()->GetECS<TransformECS>()->GetComponentData(m_ecsHandle).SetPosition(glm::vec3(value, 0.0f, 0.0f));
		}
		float GetSlotValue() const
		{
			return GetWorld()->GetECS<TransformECS>()->GetComponentData(m_ecsHandle).GetPosition().x;
		}
		const ComponentPtr& GetExternalDependency() const { return m_externalDependency; }
		void SetExternalDependency(const ComponentPtr& component) { m_externalDependency = component; }

		inline static uint32_t s_initialized = 0;
		inline static uint32_t s_begun = 0;
		inline static uint32_t s_ended = 0;
		inline static std::function<void()> s_onEnd;
		uint32_t m_begins = 0;
		uint32_t m_ticks = 0;
		uint32_t m_editorTicks = 0;
		bool m_bPublishedAtInitialize = false;
		bool m_bValidAtBegin = false;
		float m_valueAtBegin = 0.0f;
		glm::vec4 m_positionAtBegin{};
		ComponentPtr m_dependency;
		ComponentPtr m_dependencyAtBegin;
		ComponentPtr m_externalDependencyAtBegin;
		GameObjectPtr m_parentAtBegin;
		std::function<void()> m_onBegin;
		std::function<void()> m_onTick;
		std::function<void()> m_onEnd;

	private:
		float m_value = 0.0f;
		size_t m_ecsHandle = ECS::InvalidIndex;
		ComponentPtr m_externalDependency;
	};
}

REFL_AUTO(
	type(Sailor::LifecycleTestComponent, bases<Sailor::Component>),
	func(GetValue, property("value")),
	func(SetValue, property("value")),
	func(GetExternalDependency, property("externalDependency")),
	func(SetExternalDependency, property("externalDependency")),
	field(m_dependency)
)
