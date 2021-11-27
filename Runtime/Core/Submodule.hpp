#pragma once
#include "Sailor.h"

namespace Sailor
{
	class Submodule
	{
	public:

		virtual SAILOR_API ~Submodule() noexcept = default;

		template<typename TSubmodule>
		static TSubmodule* Make()
		{
			return new TSubmodule();
		}

	protected:

		SAILOR_API Submodule() noexcept = default;

		SAILOR_API Submodule(Submodule&& move) noexcept = delete;
		SAILOR_API Submodule(Submodule& copy) noexcept = delete;
		SAILOR_API Submodule& operator =(Submodule& rhs) noexcept = delete;
		SAILOR_API Submodule& operator =(Submodule&& rhs) noexcept = delete;

		friend class EngineInstance;
	};

}