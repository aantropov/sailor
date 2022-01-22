#pragma once
#include "Sailor.h"
#include "Memory/SharedPtr.hpp"

namespace Sailor
{
	using ScenePtr = TWeakPtr<class Scene>;

	class Scene
	{
	public:

		Scene() = default;
		virtual ~Scene() = default;

		Scene(const Scene&) = delete;
		Scene& operator=(const Scene&) = delete;

		Scene(Scene&&) = default;
		Scene& operator=(Scene&&) = default;

	protected:
	};
}
