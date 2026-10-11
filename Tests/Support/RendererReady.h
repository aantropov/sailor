#pragma once

#include "Sailor.h"
#include "RHI/Renderer.h"
#include "Tasks/Scheduler.h"
#include <chrono>

namespace Sailor::Tests
{
	inline bool WaitForRendererGraph()
	{
		auto* renderer = App::GetSubmodule<RHI::Renderer>();
		const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
		while (!renderer->EnsureFrameGraph())
		{
			if (std::chrono::steady_clock::now() >= deadline)
			{
				return false;
			}
			App::GetSubmodule<Tasks::Scheduler>()->WaitIdle({ EThreadType::Worker, EThreadType::RHI });
		}
		return true;
	}
}
