#include "EditorRuntimeBridge.h"
#include "Containers/Containers.h"

#include "Sailor.h"
#include "AssetRegistry/AssetRegistry.h"
#include "Core/Reflection.h"
#include "ECS/ECS.h"
#include "ECS/GlobalIlluminationECS.h"
#include "Engine/World.h"
#include "Engine/InstanceId.h"
#include "GraphicsDriver/Vulkan/VulkanGraphicsDriver.h"
#include "Memory/WeakPtr.hpp"
#include "Platform/Win32/Input.h"
#include "RHI/Buffer.h"
#include "RHI/Renderer.h"
#include "Submodules/Editor.h"
#include "Submodules/EditorRemote/RemoteViewportMacTransport.h"
#if defined(_WIN32)
#include "Submodules/EditorRemote/RemoteViewportWindowsNative.h"
#endif
#include "Submodules/ImGuiApi.h"
#include "Tasks/Scheduler.h"
#include "Tasks/Tasks.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
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
	std::atomic<uint32_t> g_numVisibleRemoteViewports = 0;
	Tasks::ITaskPtr g_viewportPumpTask; // Main owns scheduling; Editor owns the registry.

	template<typename TResult, typename TOperation>
	TResult ExecuteOnViewportOwner(TResult fallback, TOperation operation)
	{
		auto* scheduler = App::GetSubmodule<Tasks::Scheduler>();
		if (!scheduler) return fallback;
		if (scheduler->IsEditorThread()) return operation();
		auto task = Tasks::CreateTask<TResult>("Editor viewport command"_h, std::move(operation), EThreadType::Editor);
		task->Run();
		task->Wait();
		return task->GetResult();
	}

	class SailorRendererFrameSourceProvider final : public IMacRendererFrameSourceProvider
	{
	public:
		Failure AcquireFrameSource(const MacViewportSurfaceState& state, FrameIndex nextFrameIndex, MacRendererFrameSource& outSource) override
		{
			(void)state;
			(void)nextFrameIndex;

			outSource = m_frameSource;
			const bool available = outSource.m_readback != nullptr;
			m_lastProbeSummary = available ? "editorReadback=1 available=1" : "editorReadback=1 available=0";
			return Failure::Ok();
		}

		const std::string& GetLastProbeSummary() const { return m_lastProbeSummary; }
		void SetFrameSource(const MacRendererFrameSource& source) { m_frameSource = source; }

	private:
		std::string m_lastProbeSummary{};
		MacRendererFrameSource m_frameSource;
	};

	struct RemoteViewportUpdate
	{
		glm::uvec2 m_position{};
		glm::uvec2 m_extent{};
		bool m_bIsVisible = true;
		bool m_bIsFocused = false;

		bool operator==(const RemoteViewportUpdate&) const = default;
	};

	struct RemoteViewportBinding
	{
		ViewportDescriptor m_descriptor{};
#if defined(_WIN32)
		SailorWindowsSharedSurfaceProvider m_surfaceProvider{};
		SailorWindowsViewportPresenter m_presenter{};
		WindowsViewportLoopbackBinding m_binding{ m_descriptor, m_surfaceProvider, m_presenter };
#else
		SailorRendererFrameSourceProvider m_rendererFrameSourceProvider{};
		MacLoopbackIOSurfaceProvider m_surfaceProvider{ &m_rendererFrameSourceProvider };
		MacLoopbackViewportPresenter m_presenter{};
		MacViewportLoopbackBinding m_binding{ m_descriptor, m_surfaceProvider, m_presenter };
#endif
		RECT m_lastRect{};
		// Only the Editor queue reads or replaces pending updates.
		std::optional<RemoteViewportUpdate> m_pendingUpdate;
		std::atomic_bool m_bIsCreated = false;
		std::atomic_bool m_bIsVisible = true;
		bool m_bIsFocused = false;
		Failure m_lastPumpFailure = Failure::Ok();
#if defined(_WIN32)
		std::atomic_bool m_bIsPumpScheduled = false;
		// SwapChainPanel binding runs on UI; presentation runs on Render.
		std::mutex m_mutex{};
#endif

		explicit RemoteViewportBinding(ViewportDescriptor descriptor) :
			m_descriptor(std::move(descriptor)),
			m_binding(m_descriptor, m_surfaceProvider, m_presenter)
		{
		}

			void Pump()
			{
				auto result = m_binding.PumpFrame();
				if (!result.IsOk() && result.m_code != ResultCode::Retryable)
			{
				if (m_lastPumpFailure.m_code != result.m_code ||
					m_lastPumpFailure.m_nativeCode != result.m_nativeCode ||
					m_lastPumpFailure.m_message != result.m_message)
				{
					SAILOR_LOG_ERROR("Remote viewport pump failed: code=%d native=%d scope=%d message=%s",
						(int32_t)result.m_code,
						result.m_nativeCode,
						(int32_t)result.m_scope,
						result.m_message.c_str());
				}
				m_lastPumpFailure = result;
			}
			else
			{
				m_lastPumpFailure = Failure::Ok();
			}
		}

		bool Create()
		{
			const bool bWasVisible = m_bIsCreated && m_bIsVisible;
			m_bIsCreated = m_binding.Create().IsOk();
			if (m_bIsCreated)
			{
				m_binding.SetVisible(m_bIsVisible);
				m_binding.SetFocused(m_bIsFocused);
			}
			if (bWasVisible != (m_bIsCreated && m_bIsVisible))
			{
				if (bWasVisible) --g_numVisibleRemoteViewports;
				else ++g_numVisibleRemoteViewports;
			}
			return m_bIsCreated;
		}

		void Destroy()
		{
			if (m_bIsCreated.exchange(false) && m_bIsVisible) --g_numVisibleRemoteViewports;
			m_binding.Destroy();
		}

		void SetVisible(bool value)
		{
			if (m_bIsVisible != value)
			{
				if (m_bIsCreated)
				{
					if (value) ++g_numVisibleRemoteViewports;
					else --g_numVisibleRemoteViewports;
				}
				m_bIsVisible = value;
				m_binding.SetVisible(value);
			}
		}

		void SetFocused(bool value)
		{
			if (m_bIsFocused != value)
			{
				m_bIsFocused = value;
				if (m_bIsCreated) m_binding.SetFocused(value);
			}
		}
	};

	TMap<ViewportId, TSharedPtr<RemoteViewportBinding>> g_remoteViewportBindings;
#if defined(__APPLE__)
	TMap<ViewportId, Sailor::EditorRemote::MacNativeHostHandle> g_pendingRemoteViewportHostHandles;
#endif
	struct QueuedEditorInput
	{
		TWeakPtr<RemoteViewportBinding> m_binding;
		InputPacket m_packet{};
		bool m_bIsReset = false;
	};

	TVector<QueuedEditorInput> g_pendingEditorInput;
	std::optional<QueuedEditorInput> g_activeEditorInput;
	std::mutex g_pendingEditorInputMutex;
	std::mutex g_editorViewportMutex;
	RECT g_pendingEditorViewport{};
	glm::ivec2 g_appliedEditorRenderArea{ 0, 0 };
	glm::ivec2 g_editorRemoteViewportRenderArea{ 0, 0 };
	bool g_hasPendingEditorViewport = false;

	glm::ivec2 GetEditorRemoteViewportRenderArea(uint32_t fallbackWidth, uint32_t fallbackHeight);
	glm::ivec2 GetAppliedEditorRenderArea();

	TSharedPtr<RemoteViewportBinding> FindRemoteViewportBinding(ViewportId viewportId)
	{
		check(App::GetSubmodule<Tasks::Scheduler>()->IsEditorThread());
		const auto it = g_remoteViewportBindings.Find(viewportId);
		return it != g_remoteViewportBindings.end() ? it.Value() : nullptr;
	}

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

	void RequestEditorInputReset(const TSharedPtr<RemoteViewportBinding>& binding)
	{
		std::lock_guard inputLock(g_pendingEditorInputMutex);
		g_pendingEditorInput.Add(QueuedEditorInput{ binding, {}, true });
	}

	bool BindRemoteViewportHost(const TSharedPtr<RemoteViewportBinding>& binding)
	{
#if defined(__APPLE__)
		const auto viewportId = binding->m_descriptor.m_viewportId;
		const auto host = g_pendingRemoteViewportHostHandles.Find(viewportId);
		if (host != g_pendingRemoteViewportHostHandles.end())
		{
			binding->m_presenter.BindHostHandle(viewportId, host.Value());
			return binding->m_presenter.GetLastFailure().IsOk();
		}
#endif
		return true;
	}

	std::optional<RemoteViewportUpdate> TakePendingRemoteViewportUpdate(const TSharedPtr<RemoteViewportBinding>& binding)
	{
		return std::exchange(binding->m_pendingUpdate, {});
	}

	bool ApplyRemoteViewportUpdate(const TSharedPtr<RemoteViewportBinding>& binding, const RemoteViewportUpdate& update)
	{
		binding->SetVisible(update.m_bIsVisible);
		if (binding->m_bIsFocused && !update.m_bIsFocused) RequestEditorInputReset(binding);
		binding->SetFocused(update.m_bIsFocused);
		if (!BindRemoteViewportHost(binding)) return false;
#if defined(__APPLE__)
		if (GetAppliedEditorRenderArea() != glm::ivec2(update.m_extent))
		{
			binding->m_pendingUpdate = update;
			return false;
		}
#endif
		if (!binding->m_bIsCreated && !binding->Create()) return false;
		const auto& descriptor = binding->m_binding.GetRuntimeSession().GetDescriptor();
		if (descriptor.m_width != update.m_extent.x || descriptor.m_height != update.m_extent.y)
		{
			// New-generation input must enter the queue after this reset.
			RequestEditorInputReset(binding);
			if (!binding->m_binding.Resize(update.m_extent.x, update.m_extent.y).IsOk()) return false;
		}
		binding->m_lastRect.left = update.m_position.x;
		binding->m_lastRect.top = update.m_position.y;
		binding->m_lastRect.right = update.m_position.x + update.m_extent.x;
		binding->m_lastRect.bottom = update.m_position.y + update.m_extent.y;
		return true;
	}

	void ResetEditorInputStateOnEngineThread()
	{
		Win32::GlobalInput::ApplyEvent({ Platform::InputEvent::Type::Reset });
		if (auto editor = App::GetSubmodule<Editor>())
		{
			editor->CancelViewportInteraction();
		}

		g_activeEditorInput.reset();
	}

	bool IsEditorInputCurrent(const QueuedEditorInput& input)
	{
		const auto binding = input.m_binding.TryLock();
		return binding && binding->m_bIsCreated &&
			binding->m_binding.GetRuntimeSession().IsInputCurrent(input.m_packet);
	}

	void SyncEditorMouseButtons(const InputPacket& input)
	{
		using Win32::GlobalInput;
		const auto& raw = GlobalInput::GetInputState();
		const std::array state{ raw.IsButtonDown(VK_LBUTTON), raw.IsButtonDown(VK_RBUTTON), raw.IsButtonDown(VK_MBUTTON) };
		const auto desired = ResolveRemoteMouseButtonState(state, input);
		const auto cursor = raw.GetCursorPos();
		for (uint32_t i = 0; i < desired.size(); ++i)
		{
			if (state[i] == desired[i]) continue;
			GlobalInput::ApplyEvent({ Platform::InputEvent::Type::MouseButton,
				static_cast<float>(cursor.x), static_cast<float>(cursor.y), 0, static_cast<int32_t>(i), desired[i] });
		}
	}

	void SyncEditorKeyboardModifiers(const InputPacket& input)
	{
		constexpr std::array modifiers{ InputModifier::Shift, InputModifier::Control, InputModifier::Alt, InputModifier::Meta };
		constexpr std::array<uint32_t, 4> keyCodes{ VK_SHIFT, VK_CONTROL, VK_MENU, VK_LWIN };
		constexpr uint32_t physicalKeys[][2] = { { VK_LSHIFT, VK_RSHIFT }, { VK_LCONTROL, VK_RCONTROL },
			{ VK_LMENU, VK_RMENU }, { VK_LWIN, VK_RWIN } };
		const auto& state = Win32::GlobalInput::GetInputState();
		for (uint32_t i = 0; i < modifiers.size(); ++i)
		{
			const bool bIsPressed = (input.m_modifiers & modifiers[i]) == modifiers[i];
			if (!bIsPressed)
			{
				for (const auto key : physicalKeys[i])
					if (state.IsKeyDown(key))
						Win32::GlobalInput::ApplyEvent({ Platform::InputEvent::Type::Key, 0.0f, 0.0f, key, -1, false });
			}
			const bool bIsDown = state.IsKeyDown(keyCodes[i]) || (i == 3 && state.IsKeyDown(VK_RWIN));
			if (bIsDown == bIsPressed) continue;
			Win32::GlobalInput::ApplyEvent({ Platform::InputEvent::Type::Key, 0.0f, 0.0f, keyCodes[i], -1, bIsPressed });
		}
	}

	void DispatchEditorInputToRuntime(const InputPacket& input)
	{
		using Win32::GlobalInput;
		using Type = Platform::InputEvent::Type;

		if ((input.m_kind == InputKind::Focus && !input.m_focused) ||
			(input.m_kind == InputKind::Capture && !input.m_captured))
		{
			ResetEditorInputStateOnEngineThread();
			if (input.m_kind == InputKind::Focus) GlobalInput::ApplyEvent({ Type::Focus });
			return;
		}

		if (input.m_kind == InputKind::PointerMove || input.m_kind == InputKind::PointerButton ||
			input.m_kind == InputKind::PointerWheel)
			GlobalInput::ApplyEvent({ Type::MousePos, input.m_pointerX, input.m_pointerY });

		switch (input.m_kind)
		{
		case InputKind::PointerMove:
		case InputKind::PointerButton:
		case InputKind::PointerWheel:
		case InputKind::Focus:
		case InputKind::Capture:
			SyncEditorKeyboardModifiers(input);
			SyncEditorMouseButtons(input);
			break;
		default:
			break;
		}

		switch (input.m_kind)
		{
		case InputKind::Key:
			GlobalInput::ApplyEvent({ Type::Key, 0.0f, 0.0f, input.m_keyCode, -1, input.m_pressed });
			break;
		case InputKind::PointerWheel:
		{
#if defined(_WIN32)
			constexpr float wheelUnit = static_cast<float>(WHEEL_DELTA);
#else
			constexpr float wheelUnit = 1.0f;
#endif
			GlobalInput::ApplyEvent({ Type::MouseWheel, input.m_wheelDeltaX / wheelUnit, input.m_wheelDeltaY / wheelUnit });
			break;
		}
		case InputKind::Focus:
			GlobalInput::ApplyEvent({ Type::Focus, 0.0f, 0.0f, 0, -1, input.m_focused });
			break;
		default:
			break;
		}
	}

	[[maybe_unused]] glm::ivec2 GetEditorRemoteViewportRenderArea(uint32_t fallbackWidth, uint32_t fallbackHeight)
	{
		std::lock_guard lock(g_editorViewportMutex);
		if (g_editorRemoteViewportRenderArea.x > 0 && g_editorRemoteViewportRenderArea.y > 0)
		{
			return g_editorRemoteViewportRenderArea;
		}

		return glm::ivec2(std::max<uint32_t>(fallbackWidth, 1u), std::max<uint32_t>(fallbackHeight, 1u));
	}

	[[maybe_unused]] glm::ivec2 GetAppliedEditorRenderArea()
	{
		std::lock_guard lock(g_editorViewportMutex);
		return g_appliedEditorRenderArea;
	}
}

bool Sailor::EditorRuntime::TryAcquireEditorReadbackFrameSource(EditorRemote::MacRendererFrameSource& outSource)
{
	outSource = {};
	const auto* renderer = App::GetSubmodule<RHI::Renderer>();
	const auto frame = renderer ? renderer->GetEditorReadback() : ReadbackFramePtr{};
	if (!frame) return false;

	outSource.m_kind = EditorRemote::MacRendererFrameSourceKind::RendererOwnedRenderTargetMetadata;
	outSource.m_sourceObject = reinterpret_cast<uintptr_t>(frame->m_buffer.GetRawPtr());
	outSource.m_sourceToken = frame->m_frameIndex;
	outSource.m_width = static_cast<uint32_t>(frame->m_extent.x);
	outSource.m_height = static_cast<uint32_t>(frame->m_extent.y);
	outSource.m_pixelFormat = EditorRemote::PixelFormat::B8G8R8A8_UNorm;
	outSource.m_debugName = "EditorReadback";
	outSource.m_bytesPerRow = frame->GetBgraBytesPerRow();
	outSource.m_readback = frame;
	return true;
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
#if defined(_WIN32)
		mainWindow->Show(false);
#else
		mainWindow->ChangeWindowSize(requestedWindowArea.x, requestedWindowArea.y, false);
#endif
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
#if defined(_WIN32)
		mainWindow->Show(false);
#else
		mainWindow->ChangeWindowSize(requestedWindowArea.x, requestedWindowArea.y, false);
#endif
		mainWindow->SetRenderArea(requestedWindowArea);
		std::lock_guard lock(g_editorViewportMutex);
		g_editorRemoteViewportRenderArea = requestedWindowArea;
	}

	{
		std::lock_guard lock(g_editorViewportMutex);
		g_appliedEditorRenderArea = requestedWindowArea;
	}
	return true;
}

void Sailor::EditorRuntime::DrainEditorRemoteViewportInputOnEngineThread()
{
	TVector<QueuedEditorInput> pendingInput;
	{
		std::lock_guard lock(g_pendingEditorInputMutex);
		pendingInput = std::move(g_pendingEditorInput);
		g_pendingEditorInput = {};
	}

	if (g_activeEditorInput && !IsEditorInputCurrent(*g_activeEditorInput))
	{
		ResetEditorInputStateOnEngineThread();
	}

	for (const auto& event : pendingInput)
	{
		const bool bOwnsInput = g_activeEditorInput && g_activeEditorInput->m_binding == event.m_binding;
		if (event.m_bIsReset)
		{
			if (bOwnsInput) ResetEditorInputStateOnEngineThread();
			continue;
		}
		if (!IsEditorInputCurrent(event)) continue;

		const auto& input = event.m_packet;
		const bool bReleasesInput = (input.m_kind == InputKind::Focus && !input.m_focused) ||
			(input.m_kind == InputKind::Capture && !input.m_captured);
		if (bReleasesInput && !bOwnsInput) continue;
		if (!bOwnsInput)
		{
			const bool bTakesInput = (input.m_kind == InputKind::Focus && input.m_focused) ||
				(input.m_kind == InputKind::Capture && input.m_captured) ||
				(input.m_kind == InputKind::PointerButton && input.m_pressed);
			if (g_activeEditorInput && !bTakesInput) continue;
			ResetEditorInputStateOnEngineThread();
		}
		g_activeEditorInput = event;
		DispatchEditorInputToRuntime(input);
	}
}

void Sailor::EditorRuntime::UpdateRuntimeGIWorkAllowanceOnEngineThread()
{
	auto* scheduler = App::GetSubmodule<Tasks::Scheduler>();
	if (!scheduler || !scheduler->IsMainThread()) return;

	auto* editor = App::GetSubmodule<Editor>();
	auto* world = editor ? editor->GetWorld() : nullptr;
	auto* globalIllumination = world ? world->GetECS<GlobalIlluminationECS>() : nullptr;
	if (globalIllumination)
	{
		globalIllumination->SetRuntimeGIProbesWorkAllowed(g_numVisibleRemoteViewports.load() != 0);
	}
}

void Sailor::EditorRuntime::ResetForAppLifecycle()
{
	const auto clearBindings = []()
	{
		for (const auto& entry : g_remoteViewportBindings)
		{
			const auto& binding = *entry.m_second;
#if defined(_WIN32)
			std::lock_guard bindingLock(binding->m_mutex);
#endif
			binding->Destroy();
		}
		g_remoteViewportBindings.Clear();
#if defined(__APPLE__)
		g_pendingRemoteViewportHostHandles.Clear();
#endif
		return true;
	};
	if (App::GetSubmodule<Tasks::Scheduler>()) ExecuteOnViewportOwner<bool>(false, clearBindings);
	else clearBindings(); // Before Scheduler initialization there are no viewport producers.
	g_viewportPumpTask.Clear();

	Win32::GlobalInput::Reset();
	g_activeEditorInput.reset();
	{
		std::lock_guard inputLock(g_pendingEditorInputMutex);
		g_pendingEditorInput.Clear();
	}
	{
		std::lock_guard viewportLock(g_editorViewportMutex);
		g_pendingEditorViewport = {};
		g_appliedEditorRenderArea = { 0, 0 };
		g_editorRemoteViewportRenderArea = { 0, 0 };
		g_hasPendingEditorViewport = false;
	}
}

bool Sailor::EditorRuntime::HasAppliedEditorRenderArea()
{
	std::lock_guard lock(g_editorViewportMutex);
	return g_appliedEditorRenderArea.x > 0 && g_appliedEditorRenderArea.y > 0;
}

void Sailor::EditorRuntime::PumpEditorRemoteViewportsOnEngineThread()
{
	auto* scheduler = App::GetSubmodule<Tasks::Scheduler>();
	if (!scheduler || !scheduler->IsMainThread()) return;
	if (g_viewportPumpTask && !g_viewportPumpTask->IsFinished()) return;

#if !defined(_WIN32)
	MacRendererFrameSource frameSource;
	EditorRuntime::TryAcquireEditorReadbackFrameSource(frameSource);
	const auto* renderer = App::GetSubmodule<RHI::Renderer>();
	const bool bHasReadback = renderer && renderer->HasEditorReadback();
#endif
	g_viewportPumpTask = Tasks::CreateTask("Pump editor viewports"_h,
#if defined(_WIN32)
		[]()
#else
		[frameSource = std::move(frameSource), bHasReadback]()
#endif
		{
			for (const auto& entry : g_remoteViewportBindings)
			{
				const auto& binding = *entry.m_second;
#if defined(_WIN32)
				if (binding->m_bIsPumpScheduled.exchange(true)) continue;
				{
					std::lock_guard bindingLock(binding->m_mutex);
					if (auto update = TakePendingRemoteViewportUpdate(binding)) ApplyRemoteViewportUpdate(binding, *update);
				}
				Tasks::CreateTask("Pump Windows editor remote viewport"_h,
					[binding]()
					{
						std::lock_guard bindingLock(binding->m_mutex);
						if (binding->m_bIsCreated && binding->m_bIsVisible) binding->Pump();
						binding->m_bIsPumpScheduled = false;
					}, EThreadType::Render)->Run();
#else
				if (auto update = TakePendingRemoteViewportUpdate(binding)) ApplyRemoteViewportUpdate(binding, *update);
				else if (!BindRemoteViewportHost(binding)) continue;
				if (!binding->m_bIsCreated || !binding->m_bIsVisible) continue;

				const auto& descriptor = binding->m_binding.GetRuntimeSession().GetDescriptor();
				const glm::ivec2 extent(descriptor.m_width, descriptor.m_height);
				if (extent != GetAppliedEditorRenderArea()) continue;
				if (bHasReadback && (!frameSource.m_readback || frameSource.m_readback->m_extent != extent)) continue;
				binding->m_rendererFrameSourceProvider.SetFrameSource(frameSource);
				binding->Pump();
#endif
			}
		}, EThreadType::Editor);
	g_viewportPumpTask->Run();
}

void App::SetEditorViewport(uint32_t windowPosX, uint32_t windowPosY, uint32_t width, uint32_t height)
{
	width = std::max(width, 1u);
	height = std::max(height, 1u);

	RECT rect{};
	rect.left = windowPosX;
	rect.right = windowPosX + width;
	rect.bottom = windowPosY + height;
	rect.top = windowPosY;

	ExecuteOnEngineMainThread<bool>(false, [rect]()
		{
			auto editor = GetSubmodule<Editor>();
			if (!editor)
			{
				return false;
			}

			editor->SetViewport(rect);
			return true;
		});
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
#if defined(_WIN32)
	SetEditorRenderTargetSize(width, height);
	SetEditorViewport(0, 0, width, height);
#elif !defined(__APPLE__)
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

	return ExecuteOnViewportOwner<bool>(false, [=]() mutable
	{
		viewportId = viewportId == 0 ? kPrimaryEditorViewportId : viewportId;
		const uint32_t remoteWidth = std::max(width, 1u);
		const uint32_t remoteHeight = std::max(height, 1u);

		auto binding = FindRemoteViewportBinding(viewportId);
		if (!binding)
		{
			binding = TSharedPtr<RemoteViewportBinding>::Make(MakeRemoteViewportDescriptor(viewportId, remoteWidth, remoteHeight));
			g_remoteViewportBindings[viewportId] = binding;
		}

		const RemoteViewportUpdate requested{ { windowPosX, windowPosY }, { remoteWidth, remoteHeight }, bVisible, bFocused };
		binding->m_pendingUpdate = requested;

#if defined(_WIN32)
		std::unique_lock bindingLock(binding->m_mutex, std::try_to_lock);
		if (!bindingLock.owns_lock()) return false;
#endif
		const auto pending = TakePendingRemoteViewportUpdate(binding);
		return pending && ApplyRemoteViewportUpdate(binding, *pending) && *pending == requested;
	});
}

bool App::DestroyEditorRemoteViewport(uint64_t viewportId)
{
	return ExecuteOnViewportOwner<bool>(false, [=]() mutable
	{
		viewportId = viewportId == 0 ? kPrimaryEditorViewportId : viewportId;
		auto binding = FindRemoteViewportBinding(viewportId);
		if (!binding)
		{
#if defined(__APPLE__)
			return g_pendingRemoteViewportHostHandles.Remove(viewportId);
#else
			return false;
#endif
		}

#if defined(_WIN32)
		std::unique_lock bindingLock(binding->m_mutex, std::try_to_lock);
		if (!bindingLock.owns_lock())
		{
			return false;
		}
#endif

		binding->Destroy();
		g_remoteViewportBindings.Remove(viewportId);
#if defined(__APPLE__)
		g_pendingRemoteViewportHostHandles.Remove(viewportId);
#endif
		RequestEditorInputReset(binding);
		return true;
	});
}

uint32_t App::GetEditorRemoteViewportState(uint64_t viewportId)
{
	return ExecuteOnViewportOwner<uint32_t>(static_cast<uint32_t>(SessionState::Created), [=]() mutable
	{
		viewportId = viewportId == 0 ? kPrimaryEditorViewportId : viewportId;
		auto binding = FindRemoteViewportBinding(viewportId);
		if (!binding)
		{
			return static_cast<uint32_t>(Sailor::EditorRemote::SessionState::Created);
		}

		return static_cast<uint32_t>(binding->m_binding.GetRuntimeSession().GetState());
	});
}

bool App::CaptureEditorRemoteViewportFrameEvidence(uint64_t viewportId, std::string& outDiagnostic)
{
#if defined(__APPLE__)
	outDiagnostic = "Viewport does not exist.";
	return ExecuteOnViewportOwner<bool>(false, [viewportId, &outDiagnostic]() mutable
	{
		viewportId = viewportId == 0 ? kPrimaryEditorViewportId : viewportId;
		auto binding = FindRemoteViewportBinding(viewportId);
		if (!binding)
		{
			return false;
		}
		auto result = binding->m_presenter.CaptureFrameEvidence(viewportId);
		outDiagnostic = result.IsOk() ? binding->m_presenter.BuildViewportSummary(viewportId) : result.m_message;
		return result.IsOk();
	});
#else
	outDiagnostic = "Viewport pixel evidence is only available on macOS.";
	return false;
#endif
}

uint32_t App::GetEditorRemoteViewportDiagnostics(uint64_t viewportId, char** diagnostics)
{
	if (!diagnostics)
	{
		return 0;
	}
	*diagnostics = nullptr;

	return ExecuteOnViewportOwner<uint32_t>(0u, [=]() mutable
	{
		viewportId = viewportId == 0 ? kPrimaryEditorViewportId : viewportId;
		auto binding = FindRemoteViewportBinding(viewportId);
		if (!binding)
		{
			diagnostics[0] = nullptr;
			return 0u;
		}

#if defined(_WIN32)
		std::unique_lock bindingLock(binding->m_mutex, std::try_to_lock);
		if (!bindingLock.owns_lock())
		{
			static constexpr const char* kBusyDiagnostics = "busy";
			constexpr size_t kBusyDiagnosticsLen = 4;
			diagnostics[0] = new char[kBusyDiagnosticsLen + 1];
			memcpy(diagnostics[0], kBusyDiagnostics, kBusyDiagnosticsLen + 1);
			return static_cast<uint32_t>(kBusyDiagnosticsLen);
		}
#endif

		auto info = binding->m_binding.GetRuntimeSession().GetDiagnostics();
#if defined(_WIN32)
		info.m_nativePresenterSummary = binding->m_presenter.BuildSummary(viewportId);
		const std::string surfaceSummary = binding->m_surfaceProvider.BuildSummary(
			viewportId,
			info.m_connectionEpoch,
			info.m_generation);
		if (!surfaceSummary.empty())
		{
			if (!info.m_nativePresenterSummary.empty())
			{
				info.m_nativePresenterSummary += " ";
			}
			info.m_nativePresenterSummary += surfaceSummary;
		}
#elif defined(__APPLE__)
		info.m_nativePresenterSummary = binding->m_presenter.BuildViewportSummary(viewportId);
		if (const auto* allocation = binding->m_surfaceProvider.FindAllocation({ viewportId, info.m_connectionEpoch, info.m_generation }))
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
					<< " copyToken=" << allocation->m_lastProducerCopyToken
					<< " cpuUploadedBytes=" << allocation->m_cpuUploadedBytes;
				info.m_nativePresenterSummary += macSource.str();
			}
		}

		const std::string probeSummary = binding->m_rendererFrameSourceProvider.GetLastProbeSummary();
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
	});
}

bool App::RetryEditorRemoteViewport(uint64_t viewportId)
{
	return ExecuteOnViewportOwner<bool>(false, [=]() mutable
	{
		viewportId = viewportId == 0 ? kPrimaryEditorViewportId : viewportId;
		auto binding = FindRemoteViewportBinding(viewportId);
		if (!binding)
		{
			return false;
		}

#if defined(_WIN32)
		std::unique_lock bindingLock(binding->m_mutex, std::try_to_lock);
		if (!bindingLock.owns_lock())
		{
			return false;
		}
#endif

		if (!BindRemoteViewportHost(binding)) return false;
		if (!binding->m_bIsCreated ||
			binding->m_binding.GetRuntimeSession().GetState() == Sailor::EditorRemote::SessionState::Recovering ||
			binding->m_binding.GetRuntimeSession().GetState() == Sailor::EditorRemote::SessionState::Lost)
		{
			RequestEditorInputReset(binding);
			return binding->Create();
		}
		return true;
	});
}

bool App::SetEditorRemoteViewportMacHostHandle(uint64_t viewportId, uint32_t hostHandleKind, uint64_t hostHandleValue)
{
#if defined(__APPLE__)
	viewportId = viewportId == 0 ? kPrimaryEditorViewportId : viewportId;
	Sailor::EditorRemote::MacNativeHostHandle hostHandle{};
	if (hostHandleValue != 0)
	{
		// The editor creates and owns the CAMetalLayer on its UI thread.
		// Accepting an NSView here would make native layer binding marshal
		// synchronously to that thread while the viewport binding is locked.
		if (hostHandleKind != static_cast<uint32_t>(
				Sailor::EditorRemote::MacNativeHostHandleKind::CAMetalLayer))
		{
			return false;
		}
		hostHandle = { Sailor::EditorRemote::MacNativeHostHandleKind::CAMetalLayer,
			static_cast<uintptr_t>(hostHandleValue) };
	}
	if (!App::GetSubmodule<Tasks::Scheduler>()) return false;
	// Retain on UI, then hand off without waiting for native GPU operations.
	Tasks::CreateTask("Set editor viewport host"_h,
		[viewportId, hostHandle = std::move(hostHandle)]() mutable
		{
			if (hostHandle.IsValid() || g_remoteViewportBindings.ContainsKey(viewportId))
			{
				g_pendingRemoteViewportHostHandles[viewportId] = std::move(hostHandle);
			}
			else g_pendingRemoteViewportHostHandles.Remove(viewportId);
		}, EThreadType::Editor)->Run();
	return true;
#else
	(void)viewportId;
	(void)hostHandleKind;
	(void)hostHandleValue;
	return false;
#endif
}

bool App::SetEditorRemoteViewportWindowsHost(
	uint64_t viewportId,
	void* swapChainPanelInspectable,
	float compositionScale)
{
#if defined(_WIN32)
	if (!GetInstance() || !HasEditor())
	{
		return false;
	}

	viewportId = viewportId == 0 ? kPrimaryEditorViewportId : viewportId;
	// The registry belongs to Editor; SwapChainPanel attachment stays on UI.
	auto binding = ExecuteOnViewportOwner<TSharedPtr<RemoteViewportBinding>>(nullptr,
		[viewportId]() { return FindRemoteViewportBinding(viewportId); });
	if (!binding)
	{
		return swapChainPanelInspectable == nullptr;
	}

	std::unique_lock bindingLock(binding->m_mutex, std::try_to_lock);
	if (!bindingLock.owns_lock() || binding->m_binding.GetRuntimeSession().GetState() == SessionState::Disposed)
	{
		return false;
	}

	return binding->m_presenter
		.BindNativeHost(swapChainPanelInspectable, compositionScale)
		.IsOk();
#else
	(void)viewportId;
	(void)swapChainPanelInspectable;
	(void)compositionScale;
	return false;
#endif
}

bool App::SendEditorRemoteViewportInput(uint64_t viewportId, uint32_t kind, float pointerX, float pointerY, float wheelDeltaX, float wheelDeltaY, uint32_t keyCode, uint32_t button, uint32_t modifiers, bool bPressed, bool bFocused, bool bCaptured)
{
	viewportId = viewportId == 0 ? kPrimaryEditorViewportId : viewportId;
	constexpr uint32_t validModifiers =
		static_cast<uint32_t>(InputModifier::Shift) |
		static_cast<uint32_t>(InputModifier::Control) |
		static_cast<uint32_t>(InputModifier::Alt) |
		static_cast<uint32_t>(InputModifier::Meta) |
		static_cast<uint32_t>(InputModifier::MouseLeft) |
		static_cast<uint32_t>(InputModifier::MouseRight) |
		static_cast<uint32_t>(InputModifier::MouseMiddle);
	if (kind < static_cast<uint32_t>(InputKind::PointerMove) ||
		kind > static_cast<uint32_t>(InputKind::Capture) ||
		(modifiers & ~validModifiers) != 0 ||
		!std::isfinite(pointerX) || !std::isfinite(pointerY) ||
		!std::isfinite(wheelDeltaX) || !std::isfinite(wheelDeltaY) ||
		(kind == static_cast<uint32_t>(InputKind::PointerButton) && button >= 3) ||
		(kind == static_cast<uint32_t>(InputKind::Key) && keyCode >= 256))
	{
		return false;
	}

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

	return ExecuteOnViewportOwner<bool>(false, [viewportId, input]() mutable
	{
		auto binding = FindRemoteViewportBinding(viewportId);
		if (!binding)
		{
			return false;
		}
		if (!binding->m_bIsCreated)
		{
			return false;
		}

		// Stamp and enqueue together so concurrent producers preserve session order.
		std::lock_guard inputLock(g_pendingEditorInputMutex);
		auto& runtimeSession = binding->m_binding.GetRuntimeSession();
		if (!runtimeSession.StampAndHandleInput(input).IsOk())
		{
			return false;
		}

		g_pendingEditorInput.Add(QueuedEditorInput{ binding, input });
		return true;
	});
}
