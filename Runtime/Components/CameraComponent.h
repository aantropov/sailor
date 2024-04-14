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
		SAILOR_REFLECTABLE(CameraComponent)

	public:

		SAILOR_API virtual void BeginPlay() override;
		SAILOR_API virtual void Tick(float deltaTime) override;
		SAILOR_API virtual void EndPlay() override;
		SAILOR_API __forceinline CameraData& GetData();
		SAILOR_API __forceinline const CameraData& GetData() const;

		SAILOR_API float GetFov() const { return GetData().GetFov(); }
		SAILOR_API void SetFov(float value) { GetData().SetFov(value); }

		SAILOR_API float GetAspect() const { return GetData().GetAspect(); }
		SAILOR_API void SetAspect(float value) { GetData().SetAspect(value); }

		SAILOR_API float GetZNear() const { return GetData().GetZNear(); }
		SAILOR_API void SetZNear(float value) { GetData().SetZNear(value); }

		SAILOR_API float GetZFar() const { return GetData().GetZFar(); }
		SAILOR_API void SetZFar(float value) { GetData().SetZFar(value); }

		SAILOR_API static float CalculateAspect();

	protected:

		size_t m_handle = (size_t)(-1);

	};
}

REFL_AUTO(
	type(Sailor::CameraComponent, bases<Sailor::Component>),

	func(GetFov, property("fov")),
	func(SetFov, property("fov")),

	func(GetZNear, property("zNear")),
	func(SetZNear, property("zNear")),

	func(GetZFar, property("zFar")),
	func(SetZFar, property("zFar"))
)