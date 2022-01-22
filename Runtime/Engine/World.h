#pragma once
#include "Sailor.h"
#include "Object.h"
#include "Memory/SharedPtr.hpp"

namespace Sailor
{
	using WorldPtr = TWeakPtr<class World>;

	class World
	{
	public:

		World() = default;
		virtual ~World() = default;

		World(const World&) = delete;
		World& operator=(const World&) = delete;

		World(World&&) = default;
		World& operator=(World&&) = default;

	protected:
	};
}
