#pragma once
#include "Memory/RefPtr.hpp"
#include "Engine/Object.h"

namespace Sailor
{
	class RenderPipeline : public Object
	{
	public:

		SAILOR_API virtual bool IsReady() const override { return true; }

	protected:

	};

	using RenderPipelinePtr = TObjectPtr<RenderPipeline>;
};
