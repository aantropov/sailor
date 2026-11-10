#if defined(__linux__)
#include "Core/Defines.h"
#include "Sailor.h"
#include "Platform/NativeWindow.h"
#include "RHI/Renderer.h"
#include "RHI/Readback.h"
#include <cstring>

extern "C"
{
	// GTK owns native event delivery. The engine main queue owns frame publication;
	// retaining the immutable readback keeps its GPU ring slot alive during the copy.
	SAILOR_API int32_t SailorProtocolCopyLinuxViewportFrame(uint8_t* destination, uint64_t capacity,
		uint32_t* width, uint32_t* height, uint32_t* stride, uint64_t* frameIndex) noexcept
	{
		if (!width || !height || !stride || !frameIndex)
		{
			return 0;
		}
		try
		{
			const auto frame = Sailor::App::ExecuteOnEngineMainThread<Sailor::RHI::ReadbackFramePtr>({}, []()
			{
				const auto* renderer = Sailor::App::GetSubmodule<Sailor::RHI::Renderer>();
				return renderer ? renderer->GetEditorReadback() : Sailor::RHI::ReadbackFramePtr{};
			});
			if (!frame || !frame->GetBgraPixels())
			{
				return 0;
			}
			*width = static_cast<uint32_t>(frame->m_extent.x);
			*height = static_cast<uint32_t>(frame->m_extent.y);
			*stride = frame->GetBgraBytesPerRow();
			if (*frameIndex == frame->m_frameIndex)
			{
				return 3;
			}
			const uint64_t required = static_cast<uint64_t>(*stride) * *height;
			if (!destination || capacity < required)
			{
				return 2;
			}
			std::memcpy(destination, frame->GetBgraPixels(), static_cast<size_t>(required));
			*frameIndex = frame->m_frameIndex;
			return 1;
		}
		catch (...)
		{
			return 0;
		}
	}

	SAILOR_API void SailorProtocolPollLinuxWindowEvents() noexcept
	{
		if (Sailor::App::GetInstance())
		{
			if (auto& window = Sailor::App::GetMainWindow())
			{
				if (window->IsShown())
				{
					window->Show(false);
				}
				window->ProcessSystemMessages();
			}
		}
	}
}
#endif
