#pragma once
#include "Sailor.h"
#include "Engine/Object.h"
#include "Tasks/Scheduler.h"
#include "ECS/ECS.h"
#include "Components/Component.h"
#include "Memory/Memory.h"

namespace Sailor
{
	using WorldPtr = class World*;
	using GameObjectPtr = TObjectPtr<class GameObject>;

	class LightData : public ECS::TComponent
	{
	public:

		SAILOR_API __forceinline ObjectPtr& GetOwner() { return m_owner; }
		SAILOR_API __forceinline void SetOwner(const ObjectPtr& owner) { m_owner = owner; }

	protected:

		bool m_bIsDirty : 1 = false;
		bool m_bIsActive : 1 = true;
		ObjectPtr m_owner;
		friend class LightingECS;
	};

	class SAILOR_API LightingECS : public ECS::TSystem<LightingECS, LightData>
	{
	public:

		virtual Tasks::ITaskPtr Tick(float deltaTime) override;
		virtual uint32_t GetOrder() const override { return 150; }

	protected:

		TVector<size_t> m_dirtyComponents;
	};
}
