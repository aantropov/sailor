#pragma once
#include "Sailor.h"
#include "Memory/SharedPtr.hpp"
#include "Memory/ObjectPtr.hpp"
#include "JobSystem/JobSystem.h"
#include "Engine/Object.h"
#include "Components/Component.h"
#include "ECS/CameraECS.h"

namespace Sailor
{
	using WorldPtr = class World*;
	using GameObjectPtr = TObjectPtr<class GameObject>;
	using CameraComponentPtr = TObjectPtr<class CameraComponent>;

	class CameraComponent : public Component
	{
	public:

		SAILOR_API virtual void BeginPlay() override;
		SAILOR_API virtual void Tick(float deltaTime) override;
		SAILOR_API virtual void EndPlay() override;
		SAILOR_API __forceinline CameraData& GetData();

		SAILOR_API float GetFov() const { return m_fovDegrees; }
		SAILOR_API float GetAspect() const { return m_aspect; }

		SAILOR_API static float CalculateAspect();

	protected:

		float m_aspect = 0.0f;
		float m_fovDegrees = 0.0f;
		size_t m_handle = (size_t)(-1);
	};
}
