#pragma once
#include <string>
#include <iostream>

#define SAILOR_LOG(Format, ...) \
{ \
	char buffer[4096]; \
	sprintf_s(buffer, Format, __VA_ARGS__); \
	auto scheduler = App::GetSubmodule<JobSystem::Scheduler>(); \
	if (scheduler && !scheduler->IsMainThread()) \
	{ \
		const bool bIsRendererThread = scheduler->IsRendererThread(); \
		const uint64_t currentThread = (uint64_t)GetCurrentThread(); \
		scheduler->CreateTask("Log", [=]() \
		{ \
			if(!bIsRendererThread) \
			{ \
				std::cout << /* "Worker(" << currentThread << ") thread: " <<*/ (buffer) << std::endl; \
			} \
			else \
			{ \
				std::cout << "Renderer thread: " << (buffer) << std::endl; \
			} \
		}, JobSystem::EThreadType::Main)->Run(); \
	} \
	else \
	{ \
		std::cout /*<< "Main thread: " */ << (buffer) << std::endl; \
	} \
}
