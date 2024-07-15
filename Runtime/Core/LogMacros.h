#pragma once
#include <string>
#include <iostream>
#include <windows.h>
#include "Submodules/Editor.h"

#define SAILOR_LOG(Format, ...) \
{ \
	char buffer[4096]; \
	sprintf_s(buffer, Format, __VA_ARGS__); \
	if(auto editor = App::GetSubmodule<Editor>()) \
	{ \
		editor->PushMessage(std::string(buffer)); \
	} \
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
	sprintf_s(buffer, Format, __VA_ARGS__); \
	if(auto editor = App::GetSubmodule<Editor>()) \
	{ \
		editor->PushMessage(std::string(buffer)); \
	} \
	auto scheduler = App::GetSubmodule<Tasks::Scheduler>(); \
	if (scheduler && !scheduler->IsMainThread()) \
	{ \
		const bool bIsRendererThread = scheduler->IsRendererThread(); \
		Tasks::CreateTask("LogError", [=]() \
		{ \
			auto hConsole = GetStdHandle(STD_OUTPUT_HANDLE); \
			SetConsoleTextAttribute(hConsole, 4); \
			if(!bIsRendererThread) \
			{ \
				/*const uint64_t currentThread = (uint64_t)GetCurrentThread();*/ \
				std::cerr << /* "Worker(" << currentThread << ") thread: " <<*/ (buffer) << std::endl; \
			} \
			else \
			{ \
				std::cerr << "Renderer thread: " << (buffer) << std::endl; \
			} \
			SetConsoleTextAttribute(hConsole, 7); \
		}, EThreadType::Main)->Run(); \
	} \
	else \
	{ \
		auto hConsole = GetStdHandle(STD_OUTPUT_HANDLE); \
		SetConsoleTextAttribute(hConsole, 4); \
		std::cerr /*<< "Main thread: " */ << (buffer) << std::endl; \
		SetConsoleTextAttribute(hConsole, 7); \
	} \
}