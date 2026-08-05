#pragma once
#include "Core/Submodule.h"

namespace Sailor::Physics
{
	class JoltRuntime final : public TSubmodule<JoltRuntime>
	{
	public:
		SAILOR_API JoltRuntime();
		SAILOR_API ~JoltRuntime() override;
	};
}
