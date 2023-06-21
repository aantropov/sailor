#pragma once
#include <string>
#include <iostream>
#include <windows.h>

#define SAILOR_LOG(Format, ...) \
{ \
	char buffer[4096]; \
	sprintf_s(buffer, Format, __VA_ARGS__); \
	auto scheduler = App::GetSubmodule<Tasks::Scheduler>(); \
	if (scheduler && !scheduler->IsMainThread()) \
	{ \
		const bool bIsRendererThread = scheduler->IsRendererThread(); \
		const uint64_t currentThread = (uint64_t)GetCurrentThread(); \
		Tasks::CreateTask("Log", [=]() \
		{ \
			if(!bIsRendererThread) \
			{ \
				std::cout << /* "Worker(" << currentThread << ") thread: " <<*/ (buffer) << std::endl; \
			} \
			else \
			{ \
				std::cout << "Renderer thread: " << (buffer) << std::endl; \
			} \
		}, Tasks::EThreadType::Main)->Run(); \
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
	auto scheduler = App::GetSubmodule<Tasks::Scheduler>(); \
	if (scheduler && !scheduler->IsMainThread()) \
	{ \
		const bool bIsRendererThread = scheduler->IsRendererThread(); \
		const uint64_t currentThread = (uint64_t)GetCurrentThread(); \
		Tasks::CreateTask("LogError", [=]() \
		{ \
			auto hConsole = GetStdHandle(STD_OUTPUT_HANDLE); \
			SetConsoleTextAttribute(hConsole, 4); \
			if(!bIsRendererThread) \
			{ \
				std::cerr << /* "Worker(" << currentThread << ") thread: " <<*/ (buffer) << std::endl; \
			} \
			else \
			{ \
				std::cerr << "Renderer thread: " << (buffer) << std::endl; \
			} \
			SetConsoleTextAttribute(hConsole, 7); \
		}, Tasks::EThreadType::Main)->Run(); \
	} \
	else \
	{ \
		auto hConsole = GetStdHandle(STD_OUTPUT_HANDLE); \
		SetConsoleTextAttribute(hConsole, 4); \
		std::cerr /*<< "Main thread: " */ << (buffer) << std::endl; \
		SetConsoleTextAttribute(hConsole, 7); \
	} \
}