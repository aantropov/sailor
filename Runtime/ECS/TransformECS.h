#pragma once
#include "Sailor.h"
#include "Memory/SharedPtr.hpp"
#include "Memory/ObjectPtr.hpp"
#include "JobSystem/JobSystem.h"
#include "Engine/Object.h"
#include "ECS/ECS.h"
#include "Components/Component.h"
#include "Math/Transform.h"

namespace Sailor
{
	using WorldPtr = class World*;
	using GameObjectPtr = TObjectPtr<class GameObject>;
	using TranfsormComponentPtr = TObjectPtr<class TranfsormComponent>;

	class SAILOR_API TransformECS : public ECS::TSystem<Math::Transform>
	{
	public:

		static size_t GetComponentStaticType() { return ECS::TSystem<Math::Transform>::GetComponentStaticType(); }

	protected:

	};
}
