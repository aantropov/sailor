#pragma once

#include <memory>
#include "ExportDef.h"
#include "LogMacros.h"
#include "Utils.h"
#include "Foundation/ConsoleWindow.h"
#include "Foundation/Window.h"

namespace Sailor
{
	class EngineInstance
	{

	public:

		static const std::string ApplicationName;
		static const std::string EngineName;
		
		static void SAILOR_API Initialize();
		static void SAILOR_API Start();
		static void SAILOR_API Stop();
		static void SAILOR_API Shutdown();

		static Window& GetViewportWindow();

	protected:

		Window viewportWindow;

		unsigned int FPS = 0;
		float elapsedTime = 0.0f;

		static EngineInstance* instance;
		
	private:

		EngineInstance() = default;
		~EngineInstance() = default;
	};
}