#pragma once
#include <string>
#include <iostream>
#include <cstdio>
#if defined(_WIN32)
#include <windows.h>
#include "Submodules/Editor.h"
#define SAILOR_PUSH_EDITOR_MESSAGE(buffer) \
	if(auto editor = App::GetSubmodule<Editor>()) { editor->PushMessage(std::string(buffer)); }
#else
#define SAILOR_PUSH_EDITOR_MESSAGE(buffer)
#endif

#if defined(_WIN32)
#define SAILOR_SNPRINTF sprintf_s
#define SAILOR_ERROR_COLOR_BEGIN() { auto hConsole = GetStdHandle(STD_OUTPUT_HANDLE); SetConsoleTextAttribute(hConsole, 4); }
#define SAILOR_ERROR_COLOR_END() { auto hConsole = GetStdHandle(STD_OUTPUT_HANDLE); SetConsoleTextAttribute(hConsole, 7); }
#else
#define SAILOR_SNPRINTF std::snprintf
#define SAILOR_ERROR_COLOR_BEGIN()
#define SAILOR_ERROR_COLOR_END()
#endif

#define SAILOR_LOG(Format, ...) \
{ \
	char buffer[4096]; \
	SAILOR_SNPRINTF(buffer, sizeof(buffer), Format __VA_OPT__(,) __VA_ARGS__); \
	SAILOR_PUSH_EDITOR_MESSAGE(buffer); \
	auto scheduler = App::GetSubmodule<Tasks::Scheduler>(); \
	if (scheduler && !scheduler->IsMainThread()) \
	{ \
		const bool bIsRendererThread = scheduler->IsRendererThread(); \
		Tasks::CreateTask("Log", [=]() \
		{ \
			if(!bIsRendererThread) \
			{ \
				/*const uint64_t currentThread = (uint64_t)GetCurrentThread();*/ \
				std::cout << /* "Worker(" << currentThread << ") thread: " <<*/ (buffer) << std::endl; \
			} \
			else \
			{ \
				std::cout << "Renderer thread: " << (buffer) << std::endl; \
			} \
		}, EThreadType::Main)->Run(); \
	} \
	else \
	{ \
		std::cout /*<< "Main thread: " */ << (buffer) << std::endl; \
	} \
}

#define SAILOR_LOG_ERROR(Format, ...) \
{ \
	char buffer[4096]; \
	SAILOR_SNPRINTF(buffer, sizeof(buffer), Format __VA_OPT__(,) __VA_ARGS__); \
	SAILOR_PUSH_EDITOR_MESSAGE(buffer); \
	auto scheduler = App::GetSubmodule<Tasks::Scheduler>(); \
	if (scheduler && !scheduler->IsMainThread()) \
	{ \
		const bool bIsRendererThread = scheduler->IsRendererThread(); \
		Tasks::CreateTask("LogError", [=]() \
		{ \
			SAILOR_ERROR_COLOR_BEGIN(); \
			if(!bIsRendererThread) \
			{ \
				/*const uint64_t currentThread = (uint64_t)GetCurrentThread();*/ \
				std::cerr << /* "Worker(" << currentThread << ") thread: " <<*/ (buffer) << std::endl; \
			} \
			else \
			{ \
				std::cerr << "Renderer thread: " << (buffer) << std::endl; \
			} \
			SAILOR_ERROR_COLOR_END(); \
		}, EThreadType::Main)->Run(); \
	} \
	else \
	{ \
		SAILOR_ERROR_COLOR_BEGIN(); \
		std::cerr /*<< "Main thread: " */ << (buffer) << std::endl; \
		SAILOR_ERROR_COLOR_END(); \
	} \
}