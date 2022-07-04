#pragma once
#include "Sailor.h"
#include "Memory/SharedPtr.hpp"
#include "Memory/ObjectPtr.hpp"
#include "Tasks/Scheduler.h"
#include "Engine/Types.h"
#include "Engine/Object.h"
#include "Components/Component.h"
#include "ECS/CameraECS.h"

namespace Sailor
{	
	class CameraComponent : public Component
	{
	public:

		SAILOR_API virtual void BeginPlay() override;
		SAILOR_API virtual void Tick(float deltaTime) override;
		SAILOR_API virtual void EndPlay() override;
		SAILOR_API __forceinline CameraData& GetData();
		SAILOR_API __forceinline const CameraData& GetData() const;

		SAILOR_API float GetFov() const { return GetData().GetFov(); }
		SAILOR_API float GetAspect() const { return GetData().GetAspect(); }

		SAILOR_API float GetZNear() const { return GetData().GetZNear(); }
		SAILOR_API float GetZFar() const { return GetData().GetZFar(); }

		SAILOR_API static float CalculateAspect();

	protected:

		size_t m_handle = (size_t)(-1);

	};
}
