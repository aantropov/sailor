#pragma once
#include "Memory/RefPtr.hpp"
#include "Engine/Object.h"
#include "RHI/Types.h"

namespace Sailor::RHI
{
	class RHIRenderPipeline : public RHIResource
	{
	public:

		RHIRenderPipeline() = default;

	};

	using RHIRenderPipelinePtr = TRefPtr<RHIRenderPipeline>;
};
