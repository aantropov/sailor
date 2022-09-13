#pragma once
#include "Sailor.h"
#include "Memory/SharedPtr.hpp"
#include "Memory/ObjectPtr.hpp"
#include "Tasks/Scheduler.h"
#include "Engine/Types.h"
#include "Engine/Object.h"
#include "Components/Component.h"
#include "ECS/LightingECS.h"

namespace Sailor
{	
	class LightComponent : public Component
	{
	public:

		SAILOR_API virtual void BeginPlay() override;
		SAILOR_API virtual void EndPlay() override;
		SAILOR_API virtual void OnGizmo() override;
		SAILOR_API __forceinline LightData& GetData();
		SAILOR_API __forceinline const LightData& GetData() const;

	protected:

		size_t m_handle = (size_t)(-1);

	};
}
