#pragma once
#include <glm/glm/glm.hpp>
#include "Core/RefPtr.hpp"
#include "Core/Singleton.hpp"
#include "GfxDevice/Vulkan/VulkanApi.h"

namespace Sailor
{
	class Renderer : public TSingleton<Renderer>
	{
	public:
		void Initialize(class Window const* pViewport, bool bIsDebug);

		~Renderer() override;
	
	protected:
		void RenderLoop_RenderThread();
	};

};