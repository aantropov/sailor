#pragma once

#include <memory>
#include "ExportDef.h"
#include "Types.h"
#include "Foundation/ConsoleWindow.h"
#include "Foundation/Window.h"

namespace Sailor
{
	class GEngineInstance
	{
		
	public:
		
		static void SAILOR_API Initialize();
		static void SAILOR_API Run();
		static void SAILOR_API Stop();
		static void SAILOR_API Release();

		static AWindow& GetViewportWindow();

	protected:

		static std::unique_ptr<AWindow> ViewportWindow;
		
		unsigned int FPS;
		float ElapsedTime;

		static GEngineInstance* Instance;
	};	
}