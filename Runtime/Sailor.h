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
		static void SAILOR_API Start();
		static void SAILOR_API Stop();
		static void SAILOR_API Release();

		static AWindow& GetViewportWindow();

	protected:

		AWindow ViewportWindow;

		unsigned int FPS = 0;
		float ElapsedTime = 0.0f;

		static GEngineInstance* Instance;
		
	private:

		GEngineInstance() = default;
		~GEngineInstance() = default;
	};
}