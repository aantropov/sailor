#include "EditorRuntimeBridge.h"

#include "Sailor.h"
#include "AssetRegistry/AssetRegistry.h"
#include "AssetRegistry/FrameGraph/FrameGraphImporter.h"
#include "Core/Reflection.h"
#include "ECS/ECS.h"
#include "Engine/InstanceId.h"
#include "FrameGraph/RHIFrameGraph.h"
#include "GraphicsDriver/Vulkan/VulkanGraphicsDriver.h"
#include "Platform/Win32/Input.h"
#include "RHI/Buffer.h"
#include "RHI/Fence.h"
#include "RHI/Renderer.h"
#include "RHI/RenderTarget.h"
#include "RHI/Surface.h"
#include "Submodules/Editor.h"
#include "Submodules/EditorRemote/RemoteViewportMacTransport.h"
#include "Submodules/ImGuiApi.h"
#include "Tasks/Scheduler.h"

#include <algorithm>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <mutex>
#include <optional>
#include <sstream>
#include <thread>
#include <unordered_map>
#include <vector>

using namespace Sailor;
using namespace Sailor::RHI;

namespace
{
	using namespace Sailor::EditorRemote;

	constexpr ViewportId kPrimaryEditorViewportId = 1;

	std::optional<PixelFormat> ToRemotePixelFormat(RHI::EFormat format)
	{
		switch (format)
		{
		case RHI::EFormat::R8G8B8A8_UNORM:
		case RHI::EFormat::R8G8B8A8_SRGB:
			return PixelFormat::R8G8B8A8_UNorm;
		case RHI::EFormat::B8G8R8A8_UNORM:
		case RHI::EFormat::B8G8R8A8_SRGB:
			return PixelFormat::B8G8R8A8_UNorm;
		case RHI::EFormat::R16G16B16A16_SFLOAT:
			return PixelFormat::R16G16B16A16_Float;
		default:
			return std::nullopt;
		}
	}

	float HalfToFloat(uint16_t value)
	{
		const uint32_t sign = (static_cast<uint32_t>(value & 0x8000u)) << 16u;
		const uint32_t exp = (value >> 10u) & 0x1fu;
		const uint32_t mant = value & 0x03ffu;
		uint32_t out = 0;
		if (exp == 0)
		{
			if (mant == 0)
			{
				out = sign;
			}
			else
			{
				uint32_t normalizedMant = mant;
				uint32_t normalizedExp = 113u;
				while ((normalizedMant & 0x0400u) == 0)
				{
					normalizedMant <<= 1u;
					normalizedExp--;
				}
				normalizedMant &= 0x03ffu;
				out = sign | (normalizedExp << 23u) | (normalizedMant << 13u);
			}
		}
		else if (exp == 31u)
		{
			out = sign | 0x7f800000u | (mant << 13u);
		}
		else
		{
			out = sign | ((exp + 112u) << 23u) | (mant << 13u);
		}

		float result = 0.0f;
		std::memcpy(&result, &out, sizeof(float));
		return result;
	}

	uint8_t FloatToUnorm8(float value)
	{
		value = std::clamp(value, 0.0f, 1.0f);
		return static_cast<uint8_t>(value * 255.0f + 0.5f);
	}

	std::shared_ptr<std::vector<uint8_t>> TryReadbackRendererTargetToBGRA8Bytes(const RHI::RHIRenderTargetPtr& renderTarget, PixelFormat pixelFormat, uint32_t& outBytesPerRow)
	{
		auto& driver = RHI::Renderer::GetDriver();
		auto* commands = RHI::Renderer::GetDriverCommands();
		if (!driver || !commands || !renderTarget)
		{
			return nullptr;
		}

		const uint32_t srcBytesPerPixel = pixelFormat == PixelFormat::R16G16B16A16_Float ? 8u : 4u;
		const glm::ivec2 extent = renderTarget->GetExtent();
		const size_t readbackSize = static_cast<size_t>(extent.x) * static_cast<size_t>(extent.y) * srcBytesPerPixel;
		auto readbackBuffer = driver->CreateBuffer(readbackSize, RHI::EBufferUsageBit::BufferTransferDst_Bit, RHI::EMemoryPropertyBit::HostCoherent | RHI::EMemoryPropertyBit::HostVisible);
		if (!readbackBuffer)
		{
			return nullptr;
		}

		auto cmd = driver->CreateCommandList(false, RHI::ECommandListQueue::Graphics);
		commands->BeginCommandList(cmd, true);
		commands->ImageMemoryBarrier(cmd, renderTarget, renderTarget->GetFormat(), renderTarget->GetDefaultLayout(), RHI::EImageLayout::TransferSrcOptimal);
		commands->CopyImageToBuffer(cmd, renderTarget, readbackBuffer);
		commands->ImageMemoryBarrier(cmd, renderTarget, renderTarget->GetFormat(), RHI::EImageLayout::TransferSrcOptimal, renderTarget->GetDefaultLayout());
		commands->EndCommandList(cmd);

		auto fence = RHI::RHIFencePtr::Make();
		driver->SubmitCommandList(cmd, fence);
		fence->Wait();
		fence->ClearDependencies();
		fence->ClearObservables();

		const auto* src = reinterpret_cast<const uint8_t*>(readbackBuffer->GetPointer());
		if (!src)
		{
			return nullptr;
		}

		outBytesPerRow = static_cast<uint32_t>(extent.x) * 4u;
		auto outBytes = std::make_shared<std::vector<uint8_t>>(static_cast<size_t>(outBytesPerRow) * static_cast<size_t>(extent.y));
		for (int y = 0; y < extent.y; ++y)
		{
			uint8_t* dstRow = outBytes->data() + static_cast<size_t>(y) * outBytesPerRow;
			const uint8_t* srcRow = src + static_cast<size_t>(y) * static_cast<size_t>(extent.x) * srcBytesPerPixel;
			for (int x = 0; x < extent.x; ++x)
			{
				uint8_t* dstPixel = dstRow + static_cast<size_t>(x) * 4u;
				const uint8_t* srcPixel = srcRow + static_cast<size_t>(x) * srcBytesPerPixel;
				switch (pixelFormat)
				{
				case PixelFormat::B8G8R8A8_UNorm:
					dstPixel[0] = srcPixel[0];
					dstPixel[1] = srcPixel[1];
					dstPixel[2] = srcPixel[2];
					dstPixel[3] = srcPixel[3];
					break;
				case PixelFormat::R8G8B8A8_UNorm:
					dstPixel[0] = srcPixel[2];
					dstPixel[1] = srcPixel[1];
					dstPixel[2] = srcPixel[0];
					dstPixel[3] = srcPixel[3];
					break;
				case PixelFormat::R16G16B16A16_Float:
				{
					const uint16_t* halfs = reinterpret_cast<const uint16_t*>(srcPixel);
					dstPixel[0] = FloatToUnorm8(HalfToFloat(halfs[2]));
					dstPixel[1] = FloatToUnorm8(HalfToFloat(halfs[1]));
					dstPixel[2] = FloatToUnorm8(HalfToFloat(halfs[0]));
					dstPixel[3] = FloatToUnorm8(HalfToFloat(halfs[3]));
					break;
				}
				default:
					return nullptr;
				}
			}
		}

		return outBytes;
	}

	bool TryFillRendererFrameSourceFromTarget(const char* debugName, const RHI::RHIRenderTargetPtr& renderTarget, MacRendererFrameSource& outSource)
	{
		if (!renderTarget)
		{
			return false;
		}

#if defined(SAILOR_BUILD_WITH_VULKAN)
		if (!renderTarget->m_vulkan.m_image || !renderTarget->m_vulkan.m_imageView)
		{
			return false;
		}
#endif

		const auto extent = renderTarget->GetExtent();
		if (extent.x <= 0 || extent.y <= 0)
		{
			return false;
		}

		const auto pixelFormat = ToRemotePixelFormat(renderTarget->GetFormat());
		if (!pixelFormat.has_value())
		{
			return false;
		}

		outSource.m_sourceObject = reinterpret_cast<uintptr_t>(renderTarget.GetRawPtr());
		outSource.m_sourceToken = renderTarget.GetHash();
		outSource.m_width = static_cast<uint32_t>(extent.x);
		outSource.m_height = static_cast<uint32_t>(extent.y);
		outSource.m_pixelFormat = *pixelFormat;
		outSource.m_debugName = debugName;

		auto cpuBytes = TryReadbackRendererTargetToBGRA8Bytes(renderTarget, *pixelFormat, outSource.m_bytesPerRow);
		if (cpuBytes && !cpuBytes->empty())
		{
			outSource.m_kind = MacRendererFrameSourceKind::RendererOwnedRenderTargetMetadata;
			outSource.m_cpuBytes = std::move(cpuBytes);
			return true;
		}

		return false;
	}

	bool TryFillRendererFrameSourceFromSurface(const char* debugName, const RHI::RHISurfacePtr& surface, MacRendererFrameSource& outSource)
	{
		if (!surface)
		{
			return false;
		}

		auto renderTarget = surface->GetResolved();
		if (!renderTarget)
		{
			renderTarget = surface->GetTarget();
		}

		return TryFillRendererFrameSourceFromTarget(debugName, renderTarget, outSource);
	}

	bool TryAcquireFrameGraphFrameSource(MacRendererFrameSource& outSource, std::string* outSummary = nullptr)
	{
		outSource = MacRendererFrameSource{};
		auto* renderer = App::GetSubmodule<RHI::Renderer>();
		FrameGraphPtr frameGraph{};
		if (renderer)
		{
			frameGraph = renderer->GetFrameGraph();
		}
		auto rhiFrameGraph = frameGraph ? frameGraph->GetRHI() : nullptr;

		struct Candidate
		{
			const char* m_name;
			const char* m_surfaceName;
		};

		constexpr Candidate candidates[] =
		{
			{ "Renderer.SceneView.Main", "Main" },
			{ "Renderer.SceneView.BackBuffer", "BackBuffer" },
			{ "Renderer.SceneView.Secondary", "Secondary" }
		};

		const char* selectedSurface = nullptr;
		for (const auto& candidate : candidates)
		{
			auto surface = rhiFrameGraph ? rhiFrameGraph->GetSurface(candidate.m_surfaceName) : nullptr;
			if (TryFillRendererFrameSourceFromSurface(candidate.m_name, surface, outSource))
			{
				selectedSurface = candidate.m_surfaceName;
				break;
			}
		}

		const bool acquired = selectedSurface != nullptr;

		if (outSummary)
		{
			std::ostringstream ss;
			ss << "renderer=" << (renderer ? 1 : 0)
				<< " frameGraph=" << (frameGraph ? 1 : 0)
				<< " available=" << (acquired ? 1 : 0);
			if (acquired)
			{
				ss << " surface=" << selectedSurface
					<< " srcSize=" << outSource.m_width << "x" << outSource.m_height
					<< " srcPitch=" << outSource.m_bytesPerRow;
			}
			*outSummary = ss.str();
		}

		return acquired;
	}

	class SailorRendererFrameSourceProvider final : public IMacRendererFrameSourceProvider
	{
	public:
		Failure AcquireFrameSource(const MacViewportSurfaceState& state, FrameIndex nextFrameIndex, MacRendererFrameSource& outSource) override
		{
			(void)state;

			if (TryAcquireFrameGraphFrameSource(outSource, &m_lastProbeSummary))
			{
				m_hasAcquiredRealSource = true;
				return Failure::Ok();
			}

			const uint32_t maxAttempts = !m_hasAcquiredRealSource || nextFrameIndex <= 2 ? 2u : 1u;
			for (uint32_t attempt = 0; attempt < maxAttempts; attempt++)
			{
				std::this_thread::sleep_for(std::chrono::milliseconds(8));
				if (TryAcquireFrameGraphFrameSource(outSource, &m_lastProbeSummary))
				{
					m_hasAcquiredRealSource = true;
					return Failure::Ok();
				}
			}

			outSource = {};
			return Failure::Ok();
		}

		const std::string& GetLastProbeSummary() const { return m_lastProbeSummary; }

	private:
		std::string m_lastProbeSummary{};
		bool m_hasAcquiredRealSource = false;
	};

	struct RemoteViewportBinding
	{
		ViewportDescriptor m_descriptor{};
		SailorRendererFrameSourceProvider m_rendererFrameSourceProvider{};
		MacLoopbackIOSurfaceProvider m_surfaceProvider{ &m_rendererFrameSourceProvider };
		MacLoopbackViewportPresenter m_presenter{};
		MacViewportLoopbackBinding m_binding{ m_descriptor, m_surfaceProvider, m_presenter };
		RECT m_lastRect{};
		bool m_created = false;
		bool m_visible = true;
		bool m_focused = false;
		uint64_t m_nowMs = 0;
		std::mutex m_mutex{};

		explicit RemoteViewportBinding(ViewportDescriptor descriptor) :
			m_descriptor(std::move(descriptor)),
			m_binding(m_descriptor, m_surfaceProvider, m_presenter)
		{
		}

		void Pump()
		{
			m_binding.PumpFrame();
			m_binding.GetRuntimeSession().TickTimeouts(++m_nowMs);
		}

		void SetVisible(bool value)
		{
			if (m_visible != value)
			{
				m_visible = value;
				m_binding.SetVisible(value);
			}
		}

		void SetFocused(bool value)
		{
			if (m_focused != value)
			{
				m_focused = value;
				m_binding.SetFocused(value);
			}
		}
	};

	std::unordered_map<ViewportId, std::shared_ptr<RemoteViewportBinding>> g_remoteViewportBindings;
	std::mutex g_remoteViewportBindingsMutex;
	std::mutex g_editorViewportMutex;
	RECT g_pendingEditorViewport{};
	glm::ivec2 g_appliedEditorRenderArea{ 0, 0 };
	glm::ivec2 g_editorRemoteViewportRenderArea{ 0, 0 };
	bool g_hasPendingEditorViewport = false;

	glm::ivec2 GetEditorRemoteViewportRenderArea(uint32_t fallbackWidth, uint32_t fallbackHeight);
	glm::ivec2 GetAppliedEditorRenderArea();

	ViewportDescriptor MakeRemoteViewportDescriptor(ViewportId viewportId, uint32_t width, uint32_t height)
	{
		ViewportDescriptor descriptor{};
		descriptor.m_viewportId = viewportId;
		descriptor.m_width = std::max(width, 1u);
		descriptor.m_height = std::max(height, 1u);
		descriptor.m_pixelFormat = PixelFormat::B8G8R8A8_UNorm;
		descriptor.m_colorSpace = ColorSpace::Srgb;
		descriptor.m_presentMode = PresentMode::Mailbox;
		descriptor.m_debugName = "Editor.SceneView";
		return descriptor;
	}

	void DispatchEditorInputToRuntime(const InputPacket& input)
	{
		using Sailor::Win32::GlobalInput;
		using Sailor::Win32::KeyState;

		const KeyState state = input.m_pressed ? KeyState::Pressed : KeyState::Up;
		switch (input.m_kind)
		{
		case InputKind::PointerMove:
			GlobalInput::SetCursorPosition(static_cast<int32_t>(input.m_pointerX), static_cast<int32_t>(input.m_pointerY));
			break;
		case InputKind::PointerButton:
			GlobalInput::SetCursorPosition(static_cast<int32_t>(input.m_pointerX), static_cast<int32_t>(input.m_pointerY));
			if (input.m_keyCode < 3)
			{
				GlobalInput::SetMouseButtonState(input.m_keyCode, state);
			}
			break;
		case InputKind::PointerWheel:
			GlobalInput::SetCursorPosition(static_cast<int32_t>(input.m_pointerX), static_cast<int32_t>(input.m_pointerY));
			break;
		case InputKind::Key:
			if (input.m_keyCode < 256)
			{
				GlobalInput::SetKeyState(input.m_keyCode, state);
			}
			break;
		default:
			break;
		}

#if defined(__APPLE__)
		if (auto* imGui = App::GetSubmodule<ImGuiApi>())
		{
			ImGuiApi::MacEvent event{};
			switch (input.m_kind)
			{
			case InputKind::PointerMove:
				event.EventType = ImGuiApi::MacEvent::Type::MousePos;
				event.X = input.m_pointerX;
				event.Y = input.m_pointerY;
				break;
			case InputKind::PointerButton:
				event.EventType = ImGuiApi::MacEvent::Type::MouseButton;
				event.Button = static_cast<int32_t>(input.m_keyCode);
				event.bPressed = input.m_pressed;
				break;
			case InputKind::PointerWheel:
				event.EventType = ImGuiApi::MacEvent::Type::MouseWheel;
				event.X = input.m_wheelDeltaX;
				event.Y = input.m_wheelDeltaY;
				break;
			case InputKind::Key:
				event.EventType = ImGuiApi::MacEvent::Type::Key;
				event.Key = input.m_keyCode;
				event.bPressed = input.m_pressed;
				break;
			case InputKind::Focus:
				event.EventType = ImGuiApi::MacEvent::Type::Focus;
				event.bPressed = input.m_focused;
				break;
			default:
				return;
			}

			imGui->HandleMac(event);
		}
#endif
	}

	glm::ivec2 GetEditorRemoteViewportRenderArea(uint32_t fallbackWidth, uint32_t fallbackHeight)
	{
		std::lock_guard lock(g_editorViewportMutex);
		if (g_editorRemoteViewportRenderArea.x > 0 && g_editorRemoteViewportRenderArea.y > 0)
		{
			return g_editorRemoteViewportRenderArea;
		}

		return glm::ivec2(std::max<uint32_t>(fallbackWidth, 1u), std::max<uint32_t>(fallbackHeight, 1u));
	}

	glm::ivec2 GetAppliedEditorRenderArea()
	{
		std::lock_guard lock(g_editorViewportMutex);
		return g_appliedEditorRenderArea;
	}
}

bool Sailor::EditorRuntime::ApplyPendingEditorViewportOnEngineThread()
{
	RECT rect{};
	{
		std::lock_guard lock(g_editorViewportMutex);
		if (!g_hasPendingEditorViewport)
		{
			return false;
		}

		rect = g_pendingEditorViewport;
		g_hasPendingEditorViewport = false;
	}

	const int32_t width = std::max<int32_t>(1, static_cast<int32_t>(rect.right - rect.left));
	const int32_t height = std::max<int32_t>(1, static_cast<int32_t>(rect.bottom - rect.top));
	const glm::ivec2 requestedWindowArea{ width, height };
	{
		std::lock_guard lock(g_editorViewportMutex);
		if (requestedWindowArea == g_appliedEditorRenderArea)
		{
			return false;
		}
	}

	auto& mainWindow = App::GetMainWindow();
	if (!mainWindow)
	{
		return false;
	}

	if (auto scheduler = App::GetSubmodule<Tasks::Scheduler>())
	{
		scheduler->WaitIdle({ EThreadType::Render, EThreadType::RHI });
	}

	if (auto renderer = App::GetSubmodule<RHI::Renderer>())
	{
		if (renderer->IsInitialized())
		{
			renderer->GetDriver()->WaitIdle();
		}
		mainWindow->SetRenderArea(requestedWindowArea);
		{
			std::lock_guard lock(g_editorViewportMutex);
			g_editorRemoteViewportRenderArea = requestedWindowArea;
		}
		renderer->RefreshFrameGraph();
		renderer->EnsureFrameGraph();
	}
	else
	{
		mainWindow->SetRenderArea(requestedWindowArea);
		std::lock_guard lock(g_editorViewportMutex);
		g_editorRemoteViewportRenderArea = requestedWindowArea;
	}

	{
		std::lock_guard lock(g_editorViewportMutex);
		g_appliedEditorRenderArea = requestedWindowArea;
	}
	const glm::ivec2 remoteRenderArea = GetEditorRemoteViewportRenderArea(requestedWindowArea.x, requestedWindowArea.y);
	SAILOR_LOG("Applied editor viewport area: window=%dx%d render=%dx%d", requestedWindowArea.x, requestedWindowArea.y, remoteRenderArea.x, remoteRenderArea.y);
	return true;
}

bool Sailor::EditorRuntime::HasAppliedEditorRenderArea()
{
	std::lock_guard lock(g_editorViewportMutex);
	return g_appliedEditorRenderArea.x > 0 && g_appliedEditorRenderArea.y > 0;
}

void Sailor::EditorRuntime::PumpEditorRemoteViewportsOnEngineThread()
{
	std::vector<std::shared_ptr<RemoteViewportBinding>> bindings{};
	{
		std::lock_guard lock(g_remoteViewportBindingsMutex);
		bindings.reserve(g_remoteViewportBindings.size());
		for (auto& [viewportId, binding] : g_remoteViewportBindings)
		{
			(void)viewportId;
			if (binding)
			{
				bindings.push_back(binding);
			}
		}
	}

	for (auto& binding : bindings)
	{
		std::lock_guard bindingLock(binding->m_mutex);
		if (binding && binding->m_created && binding->m_visible)
		{
			binding->Pump();
		}
	}
}

void App::SetEditorViewport(uint32_t windowPosX, uint32_t windowPosY, uint32_t width, uint32_t height)
{
	width = std::max(width, 1u);
	height = std::max(height, 1u);

	if (auto editor = GetSubmodule<Editor>())
	{
		RECT rect{};
		rect.left = windowPosX;
		rect.right = windowPosX + width;
		rect.bottom = windowPosY + height;
		rect.top = windowPosY;

		editor->SetViewport(rect);
	}
}

void App::SetEditorRenderTargetSize(uint32_t width, uint32_t height)
{
	width = std::max(width, 1u);
	height = std::max(height, 1u);

	RECT rect{};
	rect.left = 0;
	rect.right = width;
	rect.bottom = height;
	rect.top = 0;

	std::lock_guard lock(g_editorViewportMutex);
	g_pendingEditorViewport = rect;
	g_hasPendingEditorViewport = true;
}

bool App::UpsertEditorRemoteViewport(uint64_t viewportId, uint32_t windowPosX, uint32_t windowPosY, uint32_t width, uint32_t height, bool bVisible, bool bFocused)
{
#if !defined(__APPLE__)
	SetEditorViewport(windowPosX, windowPosY, width, height);
#else
	SetEditorRenderTargetSize(width, height);
#endif

	if (!GetInstance())
	{
		return false;
	}

#if defined(_WIN32)
	if (!HasEditor())
	{
		return false;
	}
#endif

	viewportId = viewportId == 0 ? kPrimaryEditorViewportId : viewportId;
	const uint32_t remoteWidth = std::max(width, 1u);
	const uint32_t remoteHeight = std::max(height, 1u);

#if defined(__APPLE__)
	const glm::ivec2 appliedRenderArea = GetAppliedEditorRenderArea();
	if (appliedRenderArea.x != static_cast<int32_t>(remoteWidth) ||
		appliedRenderArea.y != static_cast<int32_t>(remoteHeight))
	{
		return true;
	}
#endif

	std::lock_guard bindingsLock(g_remoteViewportBindingsMutex);
	auto& binding = g_remoteViewportBindings[viewportId];
	if (!binding)
	{
		binding = std::make_shared<RemoteViewportBinding>(MakeRemoteViewportDescriptor(viewportId, remoteWidth, remoteHeight));
	}

	RECT rect{};
	rect.left = windowPosX;
	rect.right = windowPosX + width;
	rect.top = windowPosY;
	rect.bottom = windowPosY + height;

	std::unique_lock bindingLock(binding->m_mutex, std::try_to_lock);
	if (!bindingLock.owns_lock())
	{
		return true;
	}
	if (!binding->m_created)
	{
		binding->m_binding.Create();
		binding->m_created = true;

		const auto createdDescriptor = binding->m_binding.GetRuntimeSession().GetDescriptor();
		if (createdDescriptor.m_width != remoteWidth || createdDescriptor.m_height != remoteHeight)
		{
			binding->m_binding.Resize(remoteWidth, remoteHeight);
		}
	}
	else
	{
		const auto descriptor = binding->m_binding.GetRuntimeSession().GetDescriptor();
		if (descriptor.m_width != remoteWidth || descriptor.m_height != remoteHeight)
		{
			binding->m_binding.Resize(remoteWidth, remoteHeight);
		}
	}

	binding->m_lastRect = rect;
	binding->SetVisible(bVisible);
	binding->SetFocused(bFocused);
	binding->Pump();
	return true;
}

bool App::DestroyEditorRemoteViewport(uint64_t viewportId)
{
	viewportId = viewportId == 0 ? kPrimaryEditorViewportId : viewportId;
	std::lock_guard bindingsLock(g_remoteViewportBindingsMutex);
	auto it = g_remoteViewportBindings.find(viewportId);
	if (it == g_remoteViewportBindings.end())
	{
		return false;
	}

	auto binding = it->second;
	std::unique_lock bindingLock(binding->m_mutex, std::try_to_lock);
	if (!bindingLock.owns_lock())
	{
		return false;
	}
	binding->m_binding.Destroy();
	g_remoteViewportBindings.erase(it);
	return true;
}

uint32_t App::GetEditorRemoteViewportState(uint64_t viewportId)
{
	viewportId = viewportId == 0 ? kPrimaryEditorViewportId : viewportId;
	std::lock_guard bindingsLock(g_remoteViewportBindingsMutex);
	auto it = g_remoteViewportBindings.find(viewportId);
	if (it == g_remoteViewportBindings.end())
	{
		return static_cast<uint32_t>(Sailor::EditorRemote::SessionState::Created);
	}

	std::unique_lock bindingLock(it->second->m_mutex, std::try_to_lock);
	if (!bindingLock.owns_lock())
	{
		return static_cast<uint32_t>(Sailor::EditorRemote::SessionState::Active);
	}
	return static_cast<uint32_t>(it->second->m_binding.GetRuntimeSession().GetState());
}

uint32_t App::GetEditorRemoteViewportDiagnostics(uint64_t viewportId, char** diagnostics)
{
	if (!diagnostics)
	{
		return 0;
	}

	viewportId = viewportId == 0 ? kPrimaryEditorViewportId : viewportId;
	std::lock_guard bindingsLock(g_remoteViewportBindingsMutex);
	auto it = g_remoteViewportBindings.find(viewportId);
	if (it == g_remoteViewportBindings.end())
	{
		diagnostics[0] = nullptr;
		return 0;
	}

	std::unique_lock bindingLock(it->second->m_mutex, std::try_to_lock);
	if (!bindingLock.owns_lock())
	{
		const std::string text = "state=3 busy=1";
		diagnostics[0] = new char[text.size() + 1];
		memcpy(diagnostics[0], text.c_str(), text.size());
		diagnostics[0][text.size()] = '\0';
		return static_cast<uint32_t>(text.size());
	}

	auto info = it->second->m_binding.GetRuntimeSession().GetDiagnostics();
#if defined(__APPLE__)
	info.m_nativePresenterSummary = it->second->m_presenter.BuildViewportSummary(viewportId);
	if (const auto* allocation = it->second->m_surfaceProvider.FindAllocation({ viewportId, info.m_connectionEpoch, info.m_generation }))
	{
		if (!allocation->m_lastRendererSource.m_debugName.empty())
		{
			if (!info.m_nativePresenterSummary.empty())
			{
				info.m_nativePresenterSummary += " ";
			}
			const bool isSyntheticSource = allocation->m_lastRendererSource.m_kind == Sailor::EditorRemote::MacRendererFrameSourceKind::SyntheticIntermediate;
			std::ostringstream macSource;
			macSource << "sourceName='" << allocation->m_lastRendererSource.m_debugName
				<< "' syntheticSource=" << (isSyntheticSource ? 1 : 0)
				<< " srcSize=" << allocation->m_lastRendererSource.m_width << "x" << allocation->m_lastRendererSource.m_height
				<< " srcPitch=" << allocation->m_lastRendererSource.m_bytesPerRow
				<< " copyToken=" << allocation->m_lastProducerCopyToken;
			info.m_nativePresenterSummary += macSource.str();
		}
	}

	const std::string probeSummary = it->second->m_rendererFrameSourceProvider.GetLastProbeSummary();
	if (!probeSummary.empty())
	{
		if (!info.m_nativePresenterSummary.empty())
		{
			info.m_nativePresenterSummary += " ";
		}
		info.m_nativePresenterSummary += "probe{" + probeSummary + "}";
	}
#endif

	std::ostringstream ss;
	ss << "state=" << static_cast<uint32_t>(info.m_state)
		<< " epoch=" << info.m_connectionEpoch
		<< " gen=" << info.m_generation
		<< " transport=" << static_cast<uint32_t>(info.m_transportType)
		<< " lastGoodFrame=" << info.m_lastGoodFrameIndex
		<< " recoveries=" << info.m_recoveryAttemptCount
		<< " resizes=" << info.m_resizeCount;

	if (!info.m_lastEvent.empty())
	{
		ss << " event=" << info.m_lastEvent;
	}
	if (!info.m_nativePresenterSummary.empty())
	{
		ss << " " << info.m_nativePresenterSummary;
	}

	if (info.m_lastFailure.has_value() && !info.m_lastFailure->IsOk())
	{
		ss << " failure=[result=" << static_cast<uint32_t>(info.m_lastFailure->m_code)
			<< " nativeCode=" << info.m_lastFailure->m_nativeCode
			<< " scope=" << static_cast<uint32_t>(info.m_lastFailure->m_scope)
			<< " message='" << info.m_lastFailure->m_message << "']";
	}

	const std::string text = ss.str();
	diagnostics[0] = new char[text.size() + 1];
	memcpy(diagnostics[0], text.c_str(), text.size());
	diagnostics[0][text.size()] = '\0';
	return static_cast<uint32_t>(text.size());
}

bool App::RetryEditorRemoteViewport(uint64_t viewportId)
{
	viewportId = viewportId == 0 ? kPrimaryEditorViewportId : viewportId;
	std::lock_guard bindingsLock(g_remoteViewportBindingsMutex);
	auto it = g_remoteViewportBindings.find(viewportId);
	if (it == g_remoteViewportBindings.end())
	{
		return false;
	}

	std::unique_lock bindingLock(it->second->m_mutex, std::try_to_lock);
	if (!bindingLock.owns_lock())
	{
		return false;
	}
	if (it->second->m_binding.GetRuntimeSession().GetState() == Sailor::EditorRemote::SessionState::Recovering ||
		it->second->m_binding.GetRuntimeSession().GetState() == Sailor::EditorRemote::SessionState::Lost)
	{
		it->second->m_binding.Create();
		it->second->Pump();
	}
	return true;
}

bool App::SetEditorRemoteViewportMacHostHandle(uint64_t viewportId, uint32_t hostHandleKind, uint64_t hostHandleValue)
{
#if defined(__APPLE__)
	viewportId = viewportId == 0 ? kPrimaryEditorViewportId : viewportId;
	std::lock_guard bindingsLock(g_remoteViewportBindingsMutex);
	auto& binding = g_remoteViewportBindings[viewportId];
	if (!binding)
	{
		binding = std::make_shared<RemoteViewportBinding>(MakeRemoteViewportDescriptor(viewportId, 1u, 1u));
	}

	std::unique_lock bindingLock(binding->m_mutex, std::try_to_lock);
	if (!bindingLock.owns_lock())
	{
		return true;
	}
	Sailor::EditorRemote::MacNativeHostHandle hostHandle{};
	hostHandle.m_kind = static_cast<Sailor::EditorRemote::MacNativeHostHandleKind>(hostHandleKind);
	hostHandle.m_value = static_cast<uintptr_t>(hostHandleValue);
	binding->m_binding.GetHost().BindNativeHostHandle(viewportId, hostHandle);
	return true;
#else
	(void)viewportId;
	(void)hostHandleKind;
	(void)hostHandleValue;
	return false;
#endif
}

bool App::SendEditorRemoteViewportInput(uint64_t viewportId, uint32_t kind, float pointerX, float pointerY, float wheelDeltaX, float wheelDeltaY, uint32_t keyCode, uint32_t button, uint32_t modifiers, bool bPressed, bool bFocused, bool bCaptured)
{
	viewportId = viewportId == 0 ? kPrimaryEditorViewportId : viewportId;

	InputPacket input{};
	input.m_kind = static_cast<InputKind>(kind);
	input.m_modifiers = static_cast<InputModifier>(modifiers);
	input.m_pointerX = pointerX;
	input.m_pointerY = pointerY;
	input.m_wheelDeltaX = wheelDeltaX;
	input.m_wheelDeltaY = wheelDeltaY;
	input.m_keyCode = input.m_kind == InputKind::PointerButton ? button : keyCode;
	input.m_pressed = bPressed;
	input.m_focused = bFocused;
	input.m_captured = bCaptured;

	DispatchEditorInputToRuntime(input);

	std::lock_guard bindingsLock(g_remoteViewportBindingsMutex);
	auto it = g_remoteViewportBindings.find(viewportId);
	if (it == g_remoteViewportBindings.end())
	{
		return false;
	}

	std::unique_lock bindingLock(it->second->m_mutex, std::try_to_lock);
	if (!bindingLock.owns_lock())
	{
		return true;
	}

	auto& runtimeSession = it->second->m_binding.GetRuntimeSession();
	input.m_viewportId = runtimeSession.GetViewportId();
	input.m_connectionEpoch = runtimeSession.GetConnectionEpoch();
	input.m_generation = runtimeSession.GetGeneration();
	input.m_timestampNs = ++it->second->m_nowMs;
	return runtimeSession.HandleInput(input).IsOk();
}
