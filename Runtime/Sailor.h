#pragma once

#include "ExportDef.h"
#include "LogMacros.h"
#include "Utils.h"
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

		Window m_viewportWindow;

		uint32_t m_FPS = 0;
		float m_elapsedTime = 0.0f;

		static EngineInstance* m_pInstance;
		
	private:

		EngineInstance() = default;
		~EngineInstance() = default;
	};
}