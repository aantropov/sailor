#include "Sailor.h"
#include "AssetRegistry/AssetRegistry.h"
#include "AssetRegistry/Shader/ShaderCompiler.h"
#include "AssetRegistry/Texture/TextureImporter.h"
#include "AssetRegistry/Model/ModelImporter.h"
#include "AssetRegistry/Animation/AnimationImporter.h"
#include "AssetRegistry/Animation/AnimationAssetInfo.h"
#include "AssetRegistry/Material/MaterialImporter.h"
#include "AssetRegistry/FrameGraph/FrameGraphImporter.h"
#include "AssetRegistry/Prefab/PrefabImporter.h"
#include "AssetRegistry/World/WorldPrefabImporter.h"
#if defined(_WIN32)
#include "Platform/Win32/ConsoleWindow.h"
#endif
#include "Platform/Win32/Input.h"
#include "GraphicsDriver/Vulkan/VulkanApi.h"
#include "GraphicsDriver/Vulkan/VulkanGraphicsDriver.h"
#include "Tasks/Scheduler.h"
#include "RHI/Buffer.h"
#include "RHI/Fence.h"
#include "RHI/RenderTarget.h"
#include "RHI/Renderer.h"
#include "Core/Submodule.h"
#include "Containers/Vector.h"
#include "Containers/Set.h"
#include <sstream>
#include "Containers/Map.h"
#include "Containers/List.h"
#include "Containers/Octree.h"
#include "Engine/EngineLoop.h"
#include <algorithm>
#include <filesystem>
#include <cstring>
#include <optional>
#include <unordered_map>
#include "Memory/MemoryBlockAllocator.hpp"
#include "ECS/ECS.h"
#include "FrameGraph/RHIFrameGraph.h"
#include "FrameGraph/FrameGraphNode.h"
#include "ECS/TransformECS.h"
#include "Submodules/RenderDocApi.h"
#include "Submodules/ImGuiApi.h"
#include "Raytracing/PathTracer.h"
#include "Submodules/Editor.h"
#include "Engine/InstanceId.h"
#include "Submodules/EditorRemote/RemoteViewportMacTransport.h"

#if defined(_WIN32)
#include <timeapi.h>
#endif

using namespace Sailor;
using namespace Sailor::RHI;

App* App::s_pInstance = nullptr;
std::string App::s_workspace = "../";

namespace
{
	using namespace Sailor::EditorRemote;

	constexpr ViewportId kPrimaryEditorViewportId = 1;

	std::optional<PixelFormat> ToRemotePixelFormat(RHI::EFormat format)
	{
		switch (format)
		{
		case RHI::EFormat::R8G8B8A8_UNORM:
			return PixelFormat::R8G8B8A8_UNorm;
		case RHI::EFormat::B8G8R8A8_UNORM:
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

#if defined(__APPLE__)
		const auto vulkanDeviceHandle = renderTarget->GetNativeDeviceHandle();
		if (vulkanDeviceHandle != 0)
		{
			uintptr_t vulkanSemaphoreHandle = 0;
			if (auto* driver = RHI::Renderer::GetDriver().DynamicCast<GraphicsDriver::Vulkan::VulkanGraphicsDriver>())
			{
				const auto dedicatedSemaphoreHandle = reinterpret_cast<uintptr_t>(driver->GetLastSubmittedSceneViewMainResolvedSemaphoreHandle());
				const auto fallbackSemaphoreHandle = reinterpret_cast<uintptr_t>(driver->GetLastSubmittedRenderFinishedSemaphoreHandle());
				bool usedDedicatedSemaphore = false;
				vulkanSemaphoreHandle = SelectMacVulkanSemaphoreForMetalExport(debugName, dedicatedSemaphoreHandle, fallbackSemaphoreHandle, usedDedicatedSemaphore);
			}

			uintptr_t crossApiSharedEventObject = 0;
			uint64_t crossApiAcquireValue = 0;
			CrossApiSyncKind crossApiSyncKind = CrossApiSyncKind::None;
			bool crossApiCpuWaited = false;
			auto syncResult = SynchronizeMacVulkanRenderTargetForMetalExport(vulkanDeviceHandle, vulkanSemaphoreHandle, crossApiSharedEventObject, crossApiAcquireValue, crossApiSyncKind, crossApiCpuWaited);
			if (syncResult.IsOk())
			{
				uintptr_t exportedTextureObject = 0;
				auto exportResult = ExportMacMetalTextureFromVulkanRenderTarget(
					vulkanDeviceHandle,
					renderTarget->GetNativeImageHandle(),
					renderTarget->GetNativeImageViewHandle(),
					*pixelFormat,
					exportedTextureObject);
				if (exportResult.IsOk() && exportedTextureObject != 0)
				{
					outSource.m_kind = MacRendererFrameSourceKind::RendererOwnedMetalTexture;
					outSource.m_textureObject = exportedTextureObject;
					outSource.m_crossApiSharedEventObject = crossApiSharedEventObject;
					outSource.m_crossApiAcquireValue = crossApiAcquireValue;
					outSource.m_crossApiSyncKind = crossApiSyncKind;
					outSource.m_crossApiCpuWaited = crossApiCpuWaited;
					outSource.m_releaseTextureObjectAfterUse = true;
					return true;
				}
			}
		}
#endif

		outSource.m_kind = MacRendererFrameSourceKind::RendererOwnedRenderTargetMetadata;
		outSource.m_cpuBytes = TryReadbackRendererTargetToBGRA8Bytes(renderTarget, *pixelFormat, outSource.m_bytesPerRow);
		return true;
	}

	class SailorRendererFrameSourceProvider final : public IMacRendererFrameSourceProvider
	{
	public:
		Failure AcquireFrameSource(const MacViewportSurfaceState& state, FrameIndex nextFrameIndex, MacRendererFrameSource& outSource) override
		{
			(void)state;
			(void)nextFrameIndex;

			auto* renderer = App::GetSubmodule<RHI::Renderer>();
			if (!renderer)
			{
				return Failure::Ok();
			}

			auto frameGraph = renderer->GetFrameGraph();
			auto rhiFrameGraph = frameGraph ? frameGraph->GetRHI() : nullptr;
			if (rhiFrameGraph)
			{
				if (TryFillRendererFrameSourceFromTarget("Renderer.SceneView.Main.Resolved", rhiFrameGraph->GetRenderTarget("Main"), outSource) ||
					TryFillRendererFrameSourceFromTarget("Renderer.SceneView.Secondary", rhiFrameGraph->GetRenderTarget("Secondary"), outSource) ||
					TryFillRendererFrameSourceFromTarget("Renderer.SceneView.BackBuffer", rhiFrameGraph->GetRenderTarget("BackBuffer"), outSource))
				{
					return Failure::Ok();
				}
			}

			auto backBuffer = renderer->GetDriver() ? renderer->GetDriver()->GetBackBuffer() : nullptr;
			TryFillRendererFrameSourceFromTarget("Renderer.Driver.BackBuffer", backBuffer, outSource);
			return Failure::Ok();
		}
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

	std::unordered_map<ViewportId, std::unique_ptr<RemoteViewportBinding>> g_remoteViewportBindings;

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
}

App* App::GetInstance()
{
	return s_pInstance;
}

const std::string& App::GetWorkspace()
{
	return s_workspace;
}

AppArgs ParseCommandLineArgs(const char** args, int32_t num)
{
	AppArgs params{};

	for (int32_t i = 1; i < num; i++)
	{
		std::string arg = args[i];

		if (arg == "--port")
		{
			params.m_editorPort = atoi(Utils::GetArgValue(args, i, num).c_str());
		}
		else if (arg == "--hwnd")
		{
			const unsigned long long hwndInt = std::stoull(Utils::GetArgValue(args, i, num));
			params.m_editorHwnd = reinterpret_cast<HWND>(hwndInt);
		}
		else if (arg == "--editor")
		{
			params.m_bIsEditor = true;
		}
		else if (arg == "--waitfordebugger")
		{
			params.m_bWaitForDebugger = true;
		}
		else if (arg == "--workspace")
		{
			params.m_workspace = Utils::GetArgValue(args, i, num);
		}
		else if (arg == "--noconsole")
		{
			params.m_bRunConsole = false;
		}
		else if (arg == "--world")
		{
			params.m_world = Utils::GetArgValue(args, i, num);
		}
		else if (arg == "--pathtracer")
		{
			params.m_bRunPathTracer = true;
		}
	}

	return params;
}

void App::Initialize(const char** commandLineArgs, int32_t num)
{
	SAILOR_PROFILE_FUNCTION();

	if (s_pInstance != nullptr)
	{
		return;
	}

#if defined(_WIN32)
	timeBeginPeriod(1);
#endif
	const AppArgs params = ParseCommandLineArgs(commandLineArgs, num);
	if (params.m_bWaitForDebugger)
	{
		int32_t timeout = 5000;
		while (!::IsDebuggerPresent())
		{
			::Sleep(100);
			timeout -= 100;
			if (timeout < 0)
			{
				exit(0);
			}
		}
	}

	s_pInstance = new App();
	s_pInstance->m_args = params;

#if defined(_WIN32)
	Win32::ConsoleWindow::Initialize(false);

	if (params.m_bRunConsole)
	{
		Win32::ConsoleWindow::GetInstance()->OpenWindow(L"Sailor Console");
	}
#endif

	if (!params.m_workspace.empty())
	{
		s_pInstance->s_workspace = Utils::SanitizeFilepath(params.m_workspace);
		if (!s_pInstance->s_workspace.ends_with("/"))
		{
			s_pInstance->s_workspace += "/";
		}
	}
	else
	{
		const std::filesystem::path cwd = std::filesystem::current_path();
		const bool hasContentInCwd = std::filesystem::exists(cwd / "Content");
		const bool hasContentInParent = std::filesystem::exists(cwd / ".." / "Content");

		if (hasContentInCwd)
		{
			s_pInstance->s_workspace = "./";
		}
		else if (!hasContentInParent)
		{
			SAILOR_LOG_ERROR("Content folder was not found from current working directory. Use --workspace <path>.");
		}
	}

#if defined(SAILOR_BUILD_WITH_RENDER_DOC) && defined(_DEBUG)
	if (!params.m_bIsEditor)
	{
		s_pInstance->AddSubmodule(TSubmodule<RenderDocApi>::Make());
	}
#endif

	bool bEnableRenderValidationLayers = params.m_bEnableRenderValidationLayers;
#if defined(_SHIPPING)
	// We don't enable Debug validation layers for Shipping (Release) configuration
	bEnableRenderValidationLayers = false;
#endif

	s_pInstance->m_pMainWindow = TUniquePtr<Win32::Window>::Make();

	std::string className = "SailorEngine";
	if (params.m_editorHwnd != 0)
	{
		className = std::format("SailorEditor PID{}", ::GetCurrentThreadId());
	}

	s_pInstance->m_pMainWindow->Create(className.c_str(), className.c_str(), 1024, 768, false, false, params.m_editorHwnd);

	if (params.m_bIsEditor)
	{
		s_pInstance->AddSubmodule(TSubmodule<Editor>::Make(params.m_editorHwnd, params.m_editorPort, s_pInstance->m_pMainWindow.GetRawPtr()));
	}

	s_pInstance->AddSubmodule(TSubmodule<Tasks::Scheduler>::Make())->Initialize();
	auto renderer = s_pInstance->AddSubmodule(TSubmodule<Renderer>::Make(s_pInstance->m_pMainWindow.GetRawPtr(), RHI::EMsaaSamples::Samples_1, bEnableRenderValidationLayers));
	if (!renderer->IsInitialized())
	{
		SAILOR_LOG_ERROR("App initialization aborted: renderer backend failed to initialize.");
		return;
	}

	auto assetRegistry = s_pInstance->AddSubmodule(TSubmodule<AssetRegistry>::Make());
	s_pInstance->AddSubmodule(TSubmodule<DefaultAssetInfoHandler>::Make(assetRegistry));

	auto textureInfoHandler = s_pInstance->AddSubmodule(TSubmodule<TextureAssetInfoHandler>::Make(assetRegistry));
	auto shaderInfoHandler = s_pInstance->AddSubmodule(TSubmodule<ShaderAssetInfoHandler>::Make(assetRegistry));
	auto modelInfoHandler = s_pInstance->AddSubmodule(TSubmodule<ModelAssetInfoHandler>::Make(assetRegistry));
	auto animationInfoHandler = s_pInstance->AddSubmodule(TSubmodule<AnimationAssetInfoHandler>::Make(assetRegistry));
	auto materialInfoHandler = s_pInstance->AddSubmodule(TSubmodule<MaterialAssetInfoHandler>::Make(assetRegistry));
	auto frameGraphInfoHandler = s_pInstance->AddSubmodule(TSubmodule<FrameGraphAssetInfoHandler>::Make(assetRegistry));
	auto prefabInfoHandler = s_pInstance->AddSubmodule(TSubmodule<PrefabAssetInfoHandler>::Make(assetRegistry));
	auto worldPrefabInfoHandler = s_pInstance->AddSubmodule(TSubmodule<WorldPrefabAssetInfoHandler>::Make(assetRegistry));

	s_pInstance->AddSubmodule(TSubmodule<TextureImporter>::Make(textureInfoHandler));
	s_pInstance->AddSubmodule(TSubmodule<ShaderCompiler>::Make(shaderInfoHandler));
	s_pInstance->AddSubmodule(TSubmodule<ModelImporter>::Make(modelInfoHandler));
	s_pInstance->AddSubmodule(TSubmodule<AnimationImporter>::Make(animationInfoHandler));
	s_pInstance->AddSubmodule(TSubmodule<MaterialImporter>::Make(materialInfoHandler));
	s_pInstance->AddSubmodule(TSubmodule<FrameGraphImporter>::Make(frameGraphInfoHandler));
	s_pInstance->AddSubmodule(TSubmodule<ECS::ECSFactory>::Make());
	s_pInstance->AddSubmodule(TSubmodule<FrameGraphBuilder>::Make());
	s_pInstance->AddSubmodule(TSubmodule<PrefabImporter>::Make(prefabInfoHandler));
	s_pInstance->AddSubmodule(TSubmodule<WorldPrefabImporter>::Make(worldPrefabInfoHandler));

	assetRegistry->ScanContentFolder();

	if (params.m_bRunPathTracer)
	{
		Raytracing::PathTracer::Params pathTracerParams{};
		Raytracing::PathTracer::ParseCommandLineArgs(pathTracerParams, commandLineArgs, num);

		if (pathTracerParams.m_pathToModel.empty())
		{
			SAILOR_LOG_ERROR("PathTracer mode requires --in <modelPath> and --out <imagePath>.");
		}
		else
		{
			Raytracing::PathTracer tracer;
			tracer.Run(pathTracerParams);
		}

		s_pInstance->m_bSkipMainLoop = true;
		return;
	}

	s_pInstance->AddSubmodule(TSubmodule<ImGuiApi>::Make((void*)s_pInstance->m_pMainWindow->GetHWND()));
	auto engineLoop = s_pInstance->AddSubmodule(TSubmodule<EngineLoop>::Make());

#ifdef SAILOR_EDITOR
	AssetRegistry::WriteTextFile(AssetRegistry::GetCacheFolder() + "EngineTypes.yaml", Reflection::ExportEngineTypes());
#endif

	auto worldParams = params.m_bIsEditor ? EngineLoop::EditorWorldMask : EngineLoop::DefaultWorldMask;
	TWeakPtr<World> pWorld;

	if (!params.m_world.empty())
	{
		auto worldFileId = assetRegistry->GetOrLoadFile(params.m_world);
		auto worldPrefab = assetRegistry->LoadAssetFromFile<WorldPrefab>(worldFileId);
		pWorld = engineLoop->InstantiateWorld(worldPrefab, worldParams);
	}
	else
	{
		pWorld = engineLoop->CreateEmptyWorld("New World", worldParams);
	}

	if (auto editor = App::GetSubmodule<Editor>())
	{
		editor->SetWorld(pWorld.Lock().GetRawPtr());
	}

	SAILOR_LOG("Sailor Engine initialized");
}

void App::Start()
{
	if (!s_pInstance)
	{
		return;
	}

	if (s_pInstance->m_bSkipMainLoop)
	{
		return;
	}

	auto scheduler = GetSubmodule<Tasks::Scheduler>();
	auto renderer = GetSubmodule<Renderer>();
	auto pEngineLoop = App::GetSubmodule<EngineLoop>();
	if (!scheduler || !renderer || !renderer->IsInitialized() || !pEngineLoop)
	{
		SAILOR_LOG_ERROR("App::Start skipped: engine subsystems are not fully initialized.");
		return;
	}

	auto& pMainWindow = s_pInstance->m_pMainWindow;

	pMainWindow->SetActive(true);
	pMainWindow->SetRunning(true);

	uint32_t frameCounter = 0U;
	Utils::Timer timer{};
	Utils::Timer trackEditor{};
	FrameState currentFrame{};
	FrameState lastFrame{};
	bool bCanCreateNewFrame = true;
	bool bFirstFrame = true;

	TMap<std::string, std::function<void()>> consoleVars;
	consoleVars["scan"] = std::bind(&AssetRegistry::ScanContentFolder, GetSubmodule<AssetRegistry>());
	consoleVars["memory.benchmark"] = &Memory::RunMemoryBenchmark;
	consoleVars["vector.benchmark"] = &Sailor::RunVectorBenchmark;
	consoleVars["set.benchmark"] = &Sailor::RunSetBenchmark;
	consoleVars["map.benchmark"] = &Sailor::RunMapBenchmark;
	consoleVars["list.benchmark"] = &Sailor::RunListBenchmark;
	consoleVars["octree.benchmark"] = &Sailor::RunOctreeBenchmark;
	consoleVars["stats.memory"] = &Sailor::RHI::Renderer::MemoryStats;

	FrameInputState systemInputState = (Sailor::FrameInputState)Win32::GlobalInput::GetInputState();

	while (pMainWindow->IsRunning())
	{
		timer.Start();
		trackEditor.Start();

#if defined(_WIN32)
		Win32::ConsoleWindow::GetInstance()->Update();

		char line[256];
		if (Win32::ConsoleWindow::GetInstance()->Read(line, 256) != 0)
		{
			std::string cmd = std::string(line);
			Utils::Trim(cmd);

			auto it = consoleVars.Find(cmd);
			if (it != consoleVars.end())
			{
				it.Value()();
			}
		}
#endif

		pMainWindow->ProcessSystemMessages();
		renderer->FixLostDevice();

		scheduler->ProcessTasksOnMainThread();

		if (systemInputState.IsKeyPressed(VK_ESCAPE) || !pMainWindow->IsParentWindowValid())
		{
			Stop();
			break;
		}

		if (systemInputState.IsKeyPressed(VK_F5))
		{
			GetSubmodule<AssetRegistry>()->ScanContentFolder();
			scheduler->WaitIdle({ EThreadType::Render, EThreadType::RHI });
			renderer->RefreshFrameGraph();
		}

#ifdef SAILOR_BUILD_WITH_RENDER_DOC
		if (auto renderDoc = App::GetSubmodule<RenderDocApi>())
		{
			if (systemInputState.IsKeyPressed(VK_F6) && !renderDoc->IsConnected())
			{
				renderDoc->LaunchRenderDocApp();
			}
		}
#endif

		if (bCanCreateNewFrame)
		{
			FrameInputState inputState = (Sailor::FrameInputState)Win32::GlobalInput::GetInputState();
			currentFrame = FrameState(pEngineLoop->GetWorld().GetRawPtr(),
				Utils::GetCurrentTimeMs(),
				inputState,
				pMainWindow->GetCenterPointClient(),
				bFirstFrame ? nullptr : &lastFrame);

			pEngineLoop->ProcessCpuFrame(currentFrame);
			bFirstFrame = false;
		}

		if ((bCanCreateNewFrame = renderer->PushFrame(currentFrame)))
		{
			lastFrame = currentFrame;

			//Frame successfully pushed
			frameCounter++;
		}

		pEngineLoop->ProcessPendingWorldExits();
		if (pEngineLoop->GetWorlds().IsEmpty())
		{
			Stop();
			break;
		}

		// Collect garbage
		uint32_t index = 0;
		while (auto submodule = GetSubmodule(index))
		{
			submodule->CollectGarbage();
			index++;
		}

		timer.Stop();
		trackEditor.Stop();

		if (trackEditor.ResultAccumulatedMs() > 1 && pMainWindow->IsShown())
		{
#ifdef SAILOR_EDITOR
			if (auto editor = App::GetSubmodule<Editor>())
			{
				auto rect = editor->GetViewport();
				pMainWindow->TrackParentWindowPosition(rect);
			}
#endif
			trackEditor.Clear();
		}

		if (timer.ResultAccumulatedMs() > 1000)
		{
			SAILOR_PROFILE_SCOPE("Track FPS");

			const Stats& stats = renderer->GetStats();

			char Buff[256];
			SAILOR_SNPRINTF(Buff, sizeof(Buff), "Sailor FPS: %u, GPU FPS: %u, CPU FPS: %u, VRAM Usage: %.2f/%.2fmb, CmdLists: %u", frameCounter,
				stats.m_gpuFps,
				(uint32_t)pEngineLoop->GetCpuFps(),
				(float)stats.m_gpuHeapUsage / (1024.0f * 1024.0f),
				(float)stats.m_gpuHeapBudget / (1024.0f * 1024.0f),
				stats.m_numSubmittedCommandBuffers
			);

			pMainWindow->SetWindowTitle(Buff);

			frameCounter = 0U;
			timer.Clear();
		}

		auto oldInputState = systemInputState;
		systemInputState = Win32::GlobalInput::GetInputState();
		systemInputState.TrackForChanges(oldInputState);

		SAILOR_PROFILE_END_FRAME();
	}

	pMainWindow->SetActive(false);
	pMainWindow->SetRunning(false);
}

void App::Stop()
{
	s_pInstance->m_pMainWindow->SetActive(false);
	s_pInstance->m_pMainWindow->SetRunning(false);
}

void App::Shutdown()
{
	if (!s_pInstance)
	{
		return;
	}

	auto scheduler = GetSubmodule<Tasks::Scheduler>();
	auto renderer = GetSubmodule<Renderer>();

	if (scheduler)
	{
		scheduler->ProcessTasksOnMainThread();
	}

	if (renderer)
	{
		renderer->BeginConditionalDestroy();
	}

	if (scheduler)
	{
		scheduler->WaitIdle({ EThreadType::Main, EThreadType::Worker, EThreadType::RHI, EThreadType::Render });
	}

	SAILOR_LOG("Sailor Engine Releasing");

#if defined(SAILOR_BUILD_WITH_RENDER_DOC)
	if (App::GetSubmodule<RenderDocApi>())
	{
		RemoveSubmodule<RenderDocApi>();
	}
#endif

	RemoveSubmodule<EngineLoop>();
	RemoveSubmodule<ECS::ECSFactory>();
	RemoveSubmodule<FrameGraphBuilder>();

	// We need to finish all tasks before release
	RemoveSubmodule<ImGuiApi>();

	if (scheduler)
	{
		scheduler->ProcessTasksOnMainThread();
	}

	RemoveSubmodule<FrameGraphImporter>();
	RemoveSubmodule<MaterialImporter>();
	RemoveSubmodule<ModelImporter>();
	RemoveSubmodule<AnimationImporter>();
	RemoveSubmodule<ShaderCompiler>();
	RemoveSubmodule<TextureImporter>();
	RemoveSubmodule<PrefabImporter>();
	RemoveSubmodule<WorldPrefabImporter>();

	RemoveSubmodule<AssetRegistry>();

	RemoveSubmodule<Tasks::Scheduler>();
	RemoveSubmodule<Renderer>();

#if defined(_WIN32)
	Win32::ConsoleWindow::Shutdown();
#endif

	// Remove all left submodules
	for (auto& pSubmodule : s_pInstance->m_submodules)
	{
		if (pSubmodule)
		{
			pSubmodule.Clear();
		}
	}

	delete s_pInstance;
	s_pInstance = nullptr;
}

bool App::IsRendererInitialized()
{
	if (!s_pInstance)
	{
		return false;
	}

	if (auto renderer = s_pInstance->GetSubmodule<RHI::Renderer>())
	{
		return renderer->IsInitialized();
	}

	return false;
}

bool App::HasEditor()
{
	if (!s_pInstance)
	{
		return false;
	}

	return s_pInstance->GetSubmodule<Editor>() != nullptr;
}

void App::SetEditorViewport(uint32_t windowPosX, uint32_t windowPosY, uint32_t width, uint32_t height)
{
	if (auto editor = s_pInstance ? s_pInstance->GetSubmodule<Editor>() : nullptr)
	{
		RECT rect{};
		rect.left = windowPosX;
		rect.right = windowPosX + width;
		rect.bottom = windowPosY + height;
		rect.top = windowPosY;

		editor->SetViewport(rect);
	}
}

bool App::UpsertEditorRemoteViewport(uint64_t viewportId, uint32_t windowPosX, uint32_t windowPosY, uint32_t width, uint32_t height, bool bVisible, bool bFocused)
{
#if !defined(__APPLE__)
		SetEditorViewport(windowPosX, windowPosY, width, height);
#endif

		if (!s_pInstance)
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
		auto& binding = g_remoteViewportBindings[viewportId];
		if (!binding)
		{
			binding = std::make_unique<RemoteViewportBinding>(MakeRemoteViewportDescriptor(viewportId, width, height));
		}

		RECT rect{};
		rect.left = windowPosX;
		rect.right = windowPosX + width;
		rect.top = windowPosY;
		rect.bottom = windowPosY + height;

		if (!binding->m_created)
		{
			binding->m_binding.Create();
			binding->m_created = true;
		}
		else if (binding->m_lastRect.right - binding->m_lastRect.left != static_cast<decltype(binding->m_lastRect.right)>(width) ||
			binding->m_lastRect.bottom - binding->m_lastRect.top != static_cast<decltype(binding->m_lastRect.bottom)>(height))
		{
			binding->m_binding.Resize(std::max(width, 1u), std::max(height, 1u));
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
		auto it = g_remoteViewportBindings.find(viewportId);
		if (it == g_remoteViewportBindings.end())
		{
			return false;
		}

		it->second->m_binding.Destroy();
		g_remoteViewportBindings.erase(it);
		return true;
}

uint32_t App::GetEditorRemoteViewportState(uint64_t viewportId)
{
		viewportId = viewportId == 0 ? kPrimaryEditorViewportId : viewportId;
		auto it = g_remoteViewportBindings.find(viewportId);
		if (it == g_remoteViewportBindings.end())
		{
			return static_cast<uint32_t>(Sailor::EditorRemote::SessionState::Created);
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
		auto it = g_remoteViewportBindings.find(viewportId);
		if (it == g_remoteViewportBindings.end())
		{
			diagnostics[0] = nullptr;
			return 0;
		}

		const auto& info = it->second->m_binding.GetRuntimeSession().GetDiagnostics();
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
		auto it = g_remoteViewportBindings.find(viewportId);
		if (it == g_remoteViewportBindings.end())
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
		auto& binding = g_remoteViewportBindings[viewportId];
		if (!binding)
		{
			binding = std::make_unique<RemoteViewportBinding>(MakeRemoteViewportDescriptor(viewportId, 1u, 1u));
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

uint32_t App::PullEditorMessages(char** messages, uint32_t num)
{
	auto editor = s_pInstance ? s_pInstance->GetSubmodule<Editor>() : nullptr;
	if (!editor || !messages)
	{
		return 0;
	}

	uint32_t numMsg = std::min((uint32_t)editor->NumMessages(), num);

	for (uint32_t i = 0; i < numMsg; i++)
	{
		std::string msg;
		if (editor->PullMessage(msg))
		{
			messages[i] = new char[msg.size() + 1];
			if (messages[i] == nullptr)
			{
				return i;
			}

			std::copy(msg.begin(), msg.end(), messages[i]);
			messages[i][msg.size()] = '\0';
		}
		else
		{
			return i;
		}
	}

	return numMsg;
}

uint32_t App::SerializeCurrentWorld(char** yamlNode)
{
	auto editor = s_pInstance ? s_pInstance->GetSubmodule<Editor>() : nullptr;
	if (!editor || !yamlNode)
	{
		return 0;
	}

	auto node = editor->SerializeWorld();

	if (!node.IsNull())
	{
		std::string serializedNode = YAML::Dump(node);
		size_t length = serializedNode.length();

		yamlNode[0] = new char[length + 1];
		memcpy(yamlNode[0], serializedNode.c_str(), length);
		yamlNode[0][length] = '\0';

		return static_cast<uint32_t>(length);
	}

	yamlNode[0] = nullptr;
	return 0;
}

bool App::UpdateEditorObject(const char* strInstanceId, const char* strYamlNode)
{
	auto editor = s_pInstance ? s_pInstance->GetSubmodule<Editor>() : nullptr;
	if (!editor || !strInstanceId || !strYamlNode)
	{
		return false;
	}

	InstanceId instanceId;
	YAML::Node instanceIdYaml = YAML::Load(strInstanceId);
	instanceId.Deserialize(instanceIdYaml);

	return editor->UpdateObject(instanceId, strYamlNode);
}

bool App::RenderPathTracedImage(const char* strOutputPath, const char* strInstanceId, uint32_t height, uint32_t samplesPerPixel, uint32_t maxBounces)
{
	if (!strOutputPath || strOutputPath[0] == '\0')
	{
		return false;
	}

	auto editor = s_pInstance ? s_pInstance->GetSubmodule<Editor>() : nullptr;
	if (!editor)
	{
		return false;
	}

	InstanceId instanceId{};
	if (strInstanceId && strInstanceId[0] != '\0')
	{
		YAML::Node instanceIdYaml = YAML::Load(strInstanceId);
		instanceId.Deserialize(instanceIdYaml);
	}

	const std::string outputPath = strOutputPath;
	const bool bSuccess = editor->RenderPathTracedImage(instanceId, outputPath, height, samplesPerPixel, maxBounces);
	editor->PushMessage(bSuccess ?
		("Path tracer export succeeded: " + outputPath) :
		("Path tracer export failed: " + outputPath));

	return bSuccess;
}

void App::ShowMainWindow(bool bShow)
{
	if (auto editor = s_pInstance ? s_pInstance->GetSubmodule<Editor>() : nullptr)
	{
		editor->ShowMainWindow(bShow);
	}
}

const std::string& App::GetLoadedWorldPath()
{
	static const std::string empty;
	return s_pInstance ? s_pInstance->m_args.m_world : empty;
}

TUniquePtr<Sailor::Win32::Window>& App::GetMainWindow()
{
	return s_pInstance->m_pMainWindow;
}

Sailor::Platform::Window* App::GetMainWindowPlatform()
{
	return s_pInstance ? static_cast<Sailor::Platform::Window*>(s_pInstance->m_pMainWindow.GetRawPtr()) : nullptr;
}

int32_t App::GetExitCode()
{
	return s_pInstance ? s_pInstance->m_exitCode : 0;
}

const char* App::GetBuildConfig()
{
#if defined(_DEBUG)
	return "Debug";
#elif defined(_SHIPPING)
	return "Release";
#else
	return "Unknown";
#endif
}

void App::SetExitCode(int32_t exitCode)
{
	if (s_pInstance)
	{
		s_pInstance->m_exitCode = (std::max)(s_pInstance->m_exitCode, exitCode);
	}
}
