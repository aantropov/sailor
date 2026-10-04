#include "LogMacros.h"
#include "Sailor.h"
#include "Submodules/Editor.h"
#include "Tasks/Tasks.h"

#include <cstdarg>
#include <iostream>
#include <string>
#if defined(_WIN32)
#include <windows.h>
#endif

using namespace Sailor;

namespace
{
	void WriteConsole(ELogSeverity severity, const char* message, bool bIsRendererThread)
	{
#if defined(_WIN32)
		if (severity == ELogSeverity::Error)
		{
			SetConsoleTextAttribute(GetStdHandle(STD_OUTPUT_HANDLE), 4);
		}
#endif
		auto& output = severity == ELogSeverity::Error ? std::cerr : std::cout;
		if (bIsRendererThread)
		{
			output << "Renderer thread: ";
		}
		output << message << std::endl;
#if defined(_WIN32)
		if (severity == ELogSeverity::Error)
		{
			SetConsoleTextAttribute(GetStdHandle(STD_OUTPUT_HANDLE), 7);
		}
#endif
	}
}

void Sailor::LogMessage(ELogSeverity severity, const char* format, ...)
{
	char buffer[4096];
	va_list arguments;
	va_start(arguments, format);
	std::vsnprintf(buffer, sizeof(buffer), format, arguments);
	va_end(arguments);

	if (auto* editor = App::GetSubmodule<Editor>())
	{
		editor->PushMessage(buffer);
	}

	auto* scheduler = App::GetSubmodule<Tasks::Scheduler>();
	if (scheduler && !scheduler->IsMainThread())
	{
		const bool bIsRendererThread = scheduler->IsRendererThread();
		Tasks::CreateTask(severity == ELogSeverity::Error ? "LogError" : "Log",
			[message = std::string(buffer), severity, bIsRendererThread]()
			{
				WriteConsole(severity, message.c_str(), bIsRendererThread);
			}, EThreadType::Main)->Run();
	}
	else
	{
		WriteConsole(severity, buffer, false);
	}
}
