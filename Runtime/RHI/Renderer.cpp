#include "AssetRegistry/AssetRegistry.h"
#include "Types.h"
#include "RHI/RenderTarget.h"
#include "Renderer.h"
#include "Mesh.h"
#include "CommandList.h"
#include "GraphicsDriver.h"
#include "VertexDescription.h"
#include "Engine/EngineLoop.h"
#include "Engine/GameObject.h"
#include "GraphicsDriver/Vulkan/VulkanApi.h"
#include "GraphicsDriver/Vulkan/VulkanDevice.h"
#include "Tasks/Scheduler.h"
#include "Memory/MemoryBlockAllocator.hpp"
#include "GraphicsDriver/Vulkan/VulkanGraphicsDriver.h"
#include "Components/TestComponent.h"
#include "Components/MeshRendererComponent.h"
#include "Components/SkyComponent.h"
#include "Engine/World.h"
#include "AssetRegistry/FrameGraph/FrameGraphImporter.h"
#include "AssetRegistry/Material/MaterialImporter.h"
#include "ECS/CameraECS.h"
#include "ECS/LightingECS.h"
#include "ECS/ReflectionsECS.h"
#include "ECS/ReflectionProbeRendering.h"
#include "ECS/GlobalIlluminationECS.h"
#include "ECS/LandscapeECS.h"
#include "ECS/AnimationECS.h"
#include "ECS/PathTracerECS.h"
#include "Settings/GraphicsSettings.h"
#include "FrameGraph/EditorReadbackNode.h"
#include "FrameGraph/CPUPathTracerNode.h"
#include "FrameGraph/SkyNode.h"
#include "Platform/Window.h"

using namespace Sailor;
using namespace Sailor::RHI;

struct Renderer::FrameSubmission
{
	struct CameraPass
	{
		uint32_t m_cameraIndex = 0u;
		RHIFrameGraphPtr m_frameGraph;
		RHIRenderTargetPtr m_output;
		glm::ivec2 m_renderExtent = glm::ivec2(1);
		Settings::EGraphicsQuality m_quality = Settings::EGraphicsQuality::Low;
		Settings::GraphicsQualityProfile m_qualityProfile;
		SkyParameters m_skyParameters;
		bool m_bWantsPresent = false;
		bool m_bUsesNativeOutput = false;
	};

	explicit FrameSubmission(uint64_t submissionId) :
		m_submissionId(submissionId),
		m_materialRevision(RHIMaterial::BeginSubmissionVersionCapture(submissionId))
	{}

	~FrameSubmission()
	{
		RHIMaterial::EndSubmissionVersionCapture(m_submissionId);
	}

	FrameSubmission(const FrameSubmission&) = delete;
	FrameSubmission& operator=(const FrameSubmission&) = delete;

	const uint64_t m_submissionId;
	const uint64_t m_materialRevision;
	uint64_t m_frameGraphResourceGeneration = 0;
	bool m_bHasSwapchainImage = false;
	bool m_bResourcesSucceeded = false;
	RHIFrameGraphPtr m_frameGraph;
	RHISceneViewPtr m_sceneView;
	RHIRenderSubmissionContextPtr m_context;
	TSharedPtr<LocalReflectionCaptureState> m_reflections;
	TRefPtr<LocalReflectionCaptureJob> m_reflectionCapture;
	TSharedPtr<ReflectionProbeRenderState> m_reflectionProbes;
	TVector<TRefPtr<LocalReflectionCaptureJob>> m_probeCaptures;
	FrameSubmissionResult m_result;
	TVector<CameraPass> m_cameraPasses;
	bool m_bWantsPresent = false;
};

void IDelayedInitialization::TraceVisit(class TRefPtr<RHIResource> visitor, bool& bShouldRemoveFromList)
{
	bShouldRemoveFromList = false;

	if (auto fence = visitor.StaticCast<RHI::RHIFence>())
	{
		const auto status = fence->GetStatus();
		if (status != RHI::EFenceStatus::Pending)
		{
			m_dependenciesLock.Lock();
			auto it = std::find_if(m_dependencies.begin(), m_dependencies.end(),
				[&fence](const auto& lhs)
				{
					return fence.GetRawPtr() == lhs.GetRawPtr();
				});

			if (it != std::end(m_dependencies))
			{
				if (status == RHI::EFenceStatus::Failed)
				{
					m_bHasInitializationFailed.store(true, std::memory_order_release);
				}
				std::iter_swap(it, m_dependencies.end() - 1);
				m_dependencies.RemoveLast();
				m_bHasDependencies.store(!m_dependencies.IsEmpty(), std::memory_order_release);
				bShouldRemoveFromList = true;
			}
			m_dependenciesLock.Unlock();
		}
	}
}

bool IDelayedInitialization::IsReady() const
{
	return !m_bHasDependencies.load(std::memory_order_acquire) && !HasInitializationFailed();
}

bool IDelayedInitialization::HasInitializationFailed() const
{
	return m_bHasInitializationFailed.load(std::memory_order_acquire);
}

Renderer::Renderer(Platform::Window* pViewport, RHI::EMsaaSamples msaaSamples, bool bIsDebug)
{
	m_pViewport = pViewport;
	m_msaaSamples = msaaSamples;
	m_bIsInitialized = false;

#if defined(SAILOR_BUILD_WITH_VULKAN)
	m_driverInstance = TUniquePtr<Sailor::GraphicsDriver::Vulkan::VulkanGraphicsDriver>::Make();
	m_driverInstance->Initialize(pViewport, msaaSamples, bIsDebug);
	auto* vulkanDriver = dynamic_cast<Sailor::GraphicsDriver::Vulkan::VulkanGraphicsDriver*>(m_driverInstance.GetRawPtr());
	if (!vulkanDriver || !vulkanDriver->IsInitialized())
	{
		SAILOR_LOG_ERROR("Renderer initialization failed: Vulkan backend is unavailable.");
		return;
	}
	m_msaaSamples = static_cast<EMsaaSamples>(
		Sailor::GraphicsDriver::Vulkan::VulkanApi::GetInstance()->GetMainDevice()->GetCurrentMsaaSamples());
#endif
	m_bIsInitialized = true;

	const uint32_t numFlightSlots = (std::max)(1u, m_driverInstance->GetMaxFramesInFlight());
	m_submissionContexts.Resize(numFlightSlots);
	for (auto& context : m_submissionContexts)
	{
		context = RHIRenderSubmissionContextPtr::Make();
	}

	// Create default Vertices descriptions cache 
	auto& vertexP3N3UV2C4 = m_driverInstance->GetOrAddVertexDescription<RHI::VertexP3N3UV2C4>();
	vertexP3N3UV2C4->SetVertexStride(sizeof(RHI::VertexP3N3UV2C4));
	vertexP3N3UV2C4->AddAttribute(RHI::RHIVertexDescription::DefaultPositionBinding, 0, RHI::EFormat::R32G32B32_SFLOAT, (uint32_t)Sailor::OffsetOf(&RHI::VertexP3N3UV2C4::m_position));
	vertexP3N3UV2C4->AddAttribute(RHI::RHIVertexDescription::DefaultNormalBinding, 0, RHI::EFormat::R32G32B32_SFLOAT, (uint32_t)Sailor::OffsetOf(&RHI::VertexP3N3UV2C4::m_normal));
	vertexP3N3UV2C4->AddAttribute(RHI::RHIVertexDescription::DefaultTexcoordBinding, 0, RHI::EFormat::R32G32_SFLOAT, (uint32_t)Sailor::OffsetOf(&RHI::VertexP3N3UV2C4::m_texcoord));
	vertexP3N3UV2C4->AddAttribute(RHI::RHIVertexDescription::DefaultColorBinding, 0, RHI::EFormat::R32G32B32A32_SFLOAT, (uint32_t)Sailor::OffsetOf(&RHI::VertexP3N3UV2C4::m_color));

	auto& vertexP3C4 = m_driverInstance->GetOrAddVertexDescription<RHI::VertexP3C4>();
	vertexP3C4->SetVertexStride(sizeof(RHI::VertexP3C4));
	vertexP3C4->AddAttribute(RHIVertexDescription::DefaultPositionBinding, 0, EFormat::R32G32B32_SFLOAT, (uint32_t)Sailor::OffsetOf(&RHI::VertexP3C4::m_position));
	vertexP3C4->AddAttribute(RHIVertexDescription::DefaultColorBinding, 0, EFormat::R32G32B32A32_SFLOAT, (uint32_t)Sailor::OffsetOf(&RHI::VertexP3C4::m_color));

	auto& vertexP2UV2C1 = m_driverInstance->GetOrAddVertexDescription<RHI::VertexP2UV2C1>();
	vertexP2UV2C1->SetVertexStride(sizeof(RHI::VertexP2UV2C1));
	vertexP2UV2C1->AddAttribute(RHIVertexDescription::DefaultPositionBinding, 0, EFormat::R32G32_SFLOAT, (uint32_t)Sailor::OffsetOf(&RHI::VertexP2UV2C1::m_position));
	vertexP2UV2C1->AddAttribute(RHIVertexDescription::DefaultTexcoordBinding, 0, EFormat::R32G32_SFLOAT, (uint32_t)Sailor::OffsetOf(&RHI::VertexP2UV2C1::m_uv));
	vertexP2UV2C1->AddAttribute(RHIVertexDescription::DefaultColorBinding, 0, EFormat::R8G8B8A8_UNORM, (uint32_t)Sailor::OffsetOf(&RHI::VertexP2UV2C1::m_color));

	auto& vertexP3N3T3B3UV2C4 = m_driverInstance->GetOrAddVertexDescription<RHI::VertexP3N3T3B3UV2C4>();
	vertexP3N3T3B3UV2C4->SetVertexStride(sizeof(RHI::VertexP3N3T3B3UV2C4));
	vertexP3N3T3B3UV2C4->AddAttribute(RHI::RHIVertexDescription::DefaultPositionBinding, 0, RHI::EFormat::R32G32B32_SFLOAT, (uint32_t)Sailor::OffsetOf(&RHI::VertexP3N3T3B3UV2C4::m_position));
	vertexP3N3T3B3UV2C4->AddAttribute(RHI::RHIVertexDescription::DefaultNormalBinding, 0, RHI::EFormat::R32G32B32_SFLOAT, (uint32_t)Sailor::OffsetOf(&RHI::VertexP3N3T3B3UV2C4::m_normal));
	vertexP3N3T3B3UV2C4->AddAttribute(RHI::RHIVertexDescription::DefaultTexcoordBinding, 0, RHI::EFormat::R32G32_SFLOAT, (uint32_t)Sailor::OffsetOf(&RHI::VertexP3N3T3B3UV2C4::m_texcoord));
	vertexP3N3T3B3UV2C4->AddAttribute(RHI::RHIVertexDescription::DefaultColorBinding, 0, RHI::EFormat::R32G32B32A32_SFLOAT, (uint32_t)Sailor::OffsetOf(&RHI::VertexP3N3T3B3UV2C4::m_color));
	vertexP3N3T3B3UV2C4->AddAttribute(RHI::RHIVertexDescription::DefaultTangentBinding, 0, RHI::EFormat::R32G32B32_SFLOAT, (uint32_t)Sailor::OffsetOf(&RHI::VertexP3N3T3B3UV2C4::m_tangent));
	vertexP3N3T3B3UV2C4->AddAttribute(RHI::RHIVertexDescription::DefaultBitangentBinding, 0, RHI::EFormat::R32G32B32_SFLOAT, (uint32_t)Sailor::OffsetOf(&RHI::VertexP3N3T3B3UV2C4::m_bitangent));

	auto& vertexSkinned = m_driverInstance->GetOrAddVertexDescription<RHI::VertexP3N3T3B3UV2C4I4W4>();
	vertexSkinned->SetVertexStride(sizeof(RHI::VertexP3N3T3B3UV2C4I4W4));
	vertexSkinned->AddAttribute(RHI::RHIVertexDescription::DefaultPositionBinding, 0, RHI::EFormat::R32G32B32_SFLOAT, (uint32_t)Sailor::OffsetOf(&RHI::VertexP3N3T3B3UV2C4I4W4::m_position));
	vertexSkinned->AddAttribute(RHI::RHIVertexDescription::DefaultNormalBinding, 0, RHI::EFormat::R32G32B32_SFLOAT, (uint32_t)Sailor::OffsetOf(&RHI::VertexP3N3T3B3UV2C4I4W4::m_normal));
	vertexSkinned->AddAttribute(RHI::RHIVertexDescription::DefaultTexcoordBinding, 0, RHI::EFormat::R32G32_SFLOAT, (uint32_t)Sailor::OffsetOf(&RHI::VertexP3N3T3B3UV2C4I4W4::m_texcoord));
	vertexSkinned->AddAttribute(RHI::RHIVertexDescription::DefaultColorBinding, 0, RHI::EFormat::R32G32B32A32_SFLOAT, (uint32_t)Sailor::OffsetOf(&RHI::VertexP3N3T3B3UV2C4I4W4::m_color));
	vertexSkinned->AddAttribute(RHI::RHIVertexDescription::DefaultTangentBinding, 0, RHI::EFormat::R32G32B32_SFLOAT, (uint32_t)Sailor::OffsetOf(&RHI::VertexP3N3T3B3UV2C4I4W4::m_tangent));
	vertexSkinned->AddAttribute(RHI::RHIVertexDescription::DefaultBitangentBinding, 0, RHI::EFormat::R32G32B32_SFLOAT, (uint32_t)Sailor::OffsetOf(&RHI::VertexP3N3T3B3UV2C4I4W4::m_bitangent));
	vertexSkinned->AddAttribute(RHI::RHIVertexDescription::DefaultBoneIdsBinding, 0, RHI::EFormat::R32G32B32A32_UINT, (uint32_t)Sailor::OffsetOf(&RHI::VertexP3N3T3B3UV2C4I4W4::m_boneIds));
	vertexSkinned->AddAttribute(RHI::RHIVertexDescription::DefaultBoneWeightsBinding, 0, RHI::EFormat::R32G32B32A32_SFLOAT, (uint32_t)Sailor::OffsetOf(&RHI::VertexP3N3T3B3UV2C4I4W4::m_boneWeights));
}

Renderer::~Renderer()
{
	if (m_bIsInitialized && m_driverInstance)
	{
		if (App::GetSubmodule<Tasks::Scheduler>())
		{
			WaitIdle();
		}
	}

	// Submission contexts retain materials, pipelines, shader modules, and the
	// Vulkan device that created them. Release every renderer-owned GPU resource
	// before destroying the driver/Vulkan instance. Otherwise the last device
	// reference can be released from a late shader-module destructor after the
	// instance has already been destroyed (MoltenVK crashes in that ordering).
	m_previousRenderFrame.Clear();
	m_previousSceneVersionRelease.Clear();
	m_editorReadback.Clear();
	m_frameGraph.Clear();
	m_skyNode.Clear();
	m_cachedSceneViews.Clear();
	m_cameraGraphCache.Clear();
	m_submissionContexts.Clear();
	m_driverInstance.Clear();
}

void Renderer::MemoryStats()
{
#if defined(SAILOR_BUILD_WITH_VULKAN)
	auto renderer = App::GetSubmodule<Renderer>();
	if (!renderer)
	{
		return;
	}
	renderer->UpdateMemoryStats();
	const auto& stats = renderer->GetStats();
	constexpr float BytesToMb = 1.0f / (1024.0f * 1024.0f);

	SAILOR_LOG("Memory consumption (GPU):");
	SAILOR_LOG("Materials: %.2fmb", stats.m_materialsMemoryUsage.load(std::memory_order_relaxed) * BytesToMb);
	SAILOR_LOG("General: %.2fmb", stats.m_generalMemoryUsage.load(std::memory_order_relaxed) * BytesToMb);
	SAILOR_LOG("Meshes: %.2fmb", stats.m_meshesMemoryUsage.load(std::memory_order_relaxed) * BytesToMb);
	SAILOR_LOG("Textures: %.2fmb", stats.m_texturesMemoryUsage.load(std::memory_order_relaxed) * BytesToMb);

#endif
}

void Renderer::UpdateMemoryStats()
{
#if defined(SAILOR_BUILD_WITH_VULKAN)
	auto driverInstance = GetDriver().DynamicCast<Sailor::GraphicsDriver::Vulkan::VulkanGraphicsDriver>();
	if (!driverInstance)
	{
		return;
	}

	size_t texturesOccupiedSpace = 0u;
	const auto& internalMemoryAllocators = VulkanApi::GetInstance()->GetMainDevice()->GetMemoryAllocators();
	for (const auto& allocator : internalMemoryAllocators)
	{
		texturesOccupiedSpace += allocator.m_second->GetOccupiedSpace();
	}

	const auto& materialAllocator = driverInstance->GetMaterialSsboAllocatorIfInitialized();
	const auto& generalAllocator = driverInstance->GetGeneralSsboAllocatorIfInitialized();
	const auto& meshAllocator = driverInstance->GetMeshSsboAllocatorIfInitialized();
	m_stats.m_materialsMemoryUsage.store(
		materialAllocator ? materialAllocator->GetOccupiedSpace() : 0u,
		std::memory_order_relaxed);
	m_stats.m_generalMemoryUsage.store(
		generalAllocator ? generalAllocator->GetOccupiedSpace() : 0u,
		std::memory_order_relaxed);
	m_stats.m_meshesMemoryUsage.store(
		meshAllocator ? meshAllocator->GetOccupiedSpace() : 0u,
		std::memory_order_relaxed);
	m_stats.m_texturesMemoryUsage.store(texturesOccupiedSpace, std::memory_order_relaxed);
#endif
}

GpuTimingSnapshot Renderer::GetGpuTimings() const
{
	m_gpuTimingsLock.Lock();
	GpuTimingSnapshot timings = m_gpuTimings;
	m_gpuTimingsLock.Unlock();
	if (m_bFrameGraphOutdated || timings.m_generation != m_gpuTimingGeneration.load(std::memory_order_acquire) ||
		App::GetRenderStatsMode() != Settings::ERenderStatsMode::RenderStatsAndQueries)
	{
		timings.m_bValid = false;
		timings.m_timings.Clear();
	}
	return timings;
}

void Renderer::PublishGpuTimings(const std::optional<GpuTimingResult>& timings)
{
	if (!m_timings.PublishGpuTimings(timings))
	{
		return;
	}

	GpuTimingSnapshot snapshot = m_timings.GetGpuTimings();
	m_gpuTimingsLock.Lock();
	m_gpuTimings = std::move(snapshot);
	m_gpuTimingsLock.Unlock();
}

void Renderer::InvalidateGpuTimings()
{
	m_timings.ResetGpuTimings(m_gpuTimingGeneration.fetch_add(1u, std::memory_order_acq_rel) + 1u);
	GpuTimingSnapshot snapshot = m_timings.GetGpuTimings();
	m_gpuTimingsLock.Lock();
	m_gpuTimings = std::move(snapshot);
	m_gpuTimingsLock.Unlock();
}

void Renderer::ResetFrameCadence()
{
	m_timings.ResetFrameCadence();
	m_stats.m_renderFps.store(0u, std::memory_order_relaxed);
	m_stats.m_presentFps.store(0u, std::memory_order_relaxed);
}

RHIGlobalIlluminationRenderStats
Renderer::GetGlobalIlluminationRenderStats() const
{
	m_globalIlluminationStatsLock.Lock();
	const auto result = m_globalIlluminationStats;
	m_globalIlluminationStatsLock.Unlock();
	return result;
}

void Renderer::UpdateGlobalIlluminationRenderStats(
	const RHIGlobalIlluminationRenderStats& stats)
{
	m_globalIlluminationStatsLock.Lock();
	m_globalIlluminationStats = stats;
	m_globalIlluminationStatsLock.Unlock();
}

RHI::EFormat Renderer::GetColorFormat() const
{
	if (!m_bIsInitialized || !m_driverInstance || !Renderer::GetDriver()->GetBackBuffer())
	{
		return RHI::EFormat::UNDEFINED;
	}

	return Renderer::GetDriver()->GetBackBuffer()->GetFormat();
}

RHI::EFormat Renderer::GetDepthFormat() const
{
	if (!m_bIsInitialized || !m_driverInstance || !Renderer::GetDriver()->GetDepthBuffer())
	{
		return RHI::EFormat::UNDEFINED;
	}

	return Renderer::GetDriver()->GetDepthBuffer()->GetFormat();
}

bool Renderer::BeginConditionalDestroy()
{
	m_bForceStop = true;

	if (!m_driverInstance)
	{
		return true;
	}

	WaitIdle();
	if (!m_driverInstance->BeginConditionalDestroy()) return false;
	m_bIsInitialized = false;

	m_frameGraph.Clear();
	m_skyNode.Clear();
	m_cachedSceneViews.Clear();
	m_submissionContexts.Clear();
	return true;
}

TUniquePtr<IGraphicsDriver>& Renderer::GetDriver()
{
	return App::GetSubmodule<Renderer>()->m_driverInstance;
}

IGraphicsDriverCommands* Renderer::GetDriverCommands()
{

#if defined(SAILOR_BUILD_WITH_VULKAN)
	return dynamic_cast<IGraphicsDriverCommands*>(App::GetSubmodule<Renderer>()->m_driverInstance.GetRawPtr());
#endif

	return nullptr;
}

void Renderer::FixLostDevice()
{
	if (!m_bIsInitialized || !m_driverInstance ||
		!m_driverInstance->ShouldFixLostDevice(m_pViewport))
	{
		return;
	}

	// Preparation can still hold the render task after image acquisition.
	// Submit that frame before replacing the swapchain and its semaphores.
	if (m_previousRenderFrame)
	{
		m_previousRenderFrame->Wait();
	}

	if (m_driverInstance->FixLostDevice(m_pViewport))
	{
		m_bFrameGraphOutdated = true;
	}
}

RHISceneViewPtr Renderer::GetOrAddSceneView(WorldPtr worldPtr)
{
	SAILOR_PROFILE_FUNCTION();

	RHISceneViewPtr res;
	auto& list = m_cachedSceneViews.At_Lock(worldPtr);
	auto it = list.FindIf([](const auto& el) { return el.m_second == true; });
	if (it != list.end())
	{
		res = (*it).m_first;
		(*it).m_second = false;

		m_cachedSceneViews.Unlock(worldPtr);

		return res;
	}

	auto newEl = TPair(RHISceneViewPtr::Make(), false);
	list.EmplaceBack(newEl);
	res = newEl.m_first;

	m_cachedSceneViews.Unlock(worldPtr);

	return res;
}

void Renderer::RemoveSceneView(WorldPtr worldPtr)
{
	m_cachedSceneViews.Remove(worldPtr);
	PruneCameraGraphEntries(worldPtr, 0ull);
}

FrameGraphPtr Renderer::ResolveCameraGraph(WorldPtr world, const CameraData& camera, uint64_t epoch, RHIRenderTargetPtr& outOutput, glm::ivec2& outRenderExtent)
{
	outRenderExtent = glm::ivec2(1);
	outOutput = nullptr;
	if (!world || !camera.GetInstanceId())
	{
		return {};
	}

	const InstanceId cameraId = camera.GetInstanceId();

	// Find existing entry
	CameraGraphEntry* pEntry = nullptr;
	for (auto& entry : m_cameraGraphCache)
	{
		if (entry.m_world == world && entry.m_cameraInstanceId == cameraId)
		{
			pEntry = &entry;
			break;
		}
	}

	if (pEntry)
	{
		pEntry->m_lastSeenEpoch = epoch;
	}

	// Determine output extent
	Settings::GraphicsExtent outputExtent{};
	RHITexturePtr targetTexture{};
	uint64_t targetRevision = 0ull;

	TexturePtr authoredTarget = camera.GetRenderTarget();
	if (authoredTarget)
	{
		RHITexturePtr resolvedTarget = camera.GetResolvedRenderTarget();
		if (!resolvedTarget || !resolvedTarget->IsReady())
		{
			return {};
		}

		RHIRenderTargetPtr renderTarget = resolvedTarget.DynamicCast<RHIRenderTarget>();
		if (!renderTarget)
		{
			return {};
		}

		auto extent = renderTarget->GetExtent();
		if (extent.x == 0 || extent.y == 0)
		{
			return {};
		}

		outputExtent.m_width = static_cast<uint32_t>(extent.x);
		outputExtent.m_height = static_cast<uint32_t>(extent.y);
		targetTexture = resolvedTarget;
		targetRevision = camera.GetResolvedRevision();
	}
	else
	{
		if (m_pViewport)
		{
			auto area = m_pViewport->GetRenderArea();
			if (area.x > 0 && area.y > 0)
			{
				outputExtent.m_width = static_cast<uint32_t>(area.x);
				outputExtent.m_height = static_cast<uint32_t>(area.y);
			}
			else
			{
				return {};
			}
		}
		else
		{
			return {};
		}
	}

	// Resolve config UID and revision
	FileId configUid = camera.GetRendererConfig();
	FileRevision configRevision{};

	if (configUid == FileId::Invalid)
	{
		const char* path = camera.GetEditorRole() ? "EditorCamera.renderer" : "DefaultRenderer.renderer";
		auto registry = App::GetSubmodule<AssetRegistry>();
		if (!registry)
		{
			return {};
		}
		auto info = registry->GetAssetInfoPtr<FrameGraphAssetInfoPtr>(path);
		if (!info)
		{
			return {};
		}
		configUid = info->GetFileId();
		std::string filepath = info->GetAssetFilepath();
		if (!Utils::TryGetFileRevision(filepath, configRevision))
		{
			return {};
		}
	}
	else
	{
		auto registry = App::GetSubmodule<AssetRegistry>();
		if (!registry)
		{
			return {};
		}
		auto info = registry->GetAssetInfoPtr<FrameGraphAssetInfoPtr>(configUid);
		if (!info)
		{
			return {};
		}
		std::string filepath = info->GetAssetFilepath();
		if (!Utils::TryGetFileRevision(filepath, configRevision))
		{
			return {};
		}
	}

	// Resolve quality
	Settings::EGraphicsQuality resolvedQuality = Settings::EGraphicsQuality::Low;
	Settings::GraphicsQualityProfile profile{};

	Settings::EGraphicsQualitySelection requestedQuality = camera.GetQualityPreset();
	if (requestedQuality == Settings::EGraphicsQualitySelection::ProjectDefault)
	{
		if (camera.GetEditorRole())
		{
			resolvedQuality = App::GetActiveGraphicsQuality();
		}
		else
		{
			resolvedQuality = App::GetGraphicsSettings().m_defaultQuality;
		}
	}
	else
	{
		resolvedQuality = Settings::ResolveQualitySelection(requestedQuality, App::GetGraphicsSettings().m_defaultQuality);
	}

	profile = App::GetGraphicsSettings().GetProfile(resolvedQuality);

	uint32_t actualMsaa = static_cast<uint32_t>(m_msaaSamples);

	// Check if cache hit
	if (pEntry)
	{
		bool bConfigChanged = (pEntry->m_configUid != configUid) || (pEntry->m_configRevision != configRevision);
		bool bTargetChanged = (pEntry->m_target != targetTexture) || (pEntry->m_targetRevision != targetRevision);
		bool bExtentChanged = (pEntry->m_outputExtent.m_width != outputExtent.m_width) || (pEntry->m_outputExtent.m_height != outputExtent.m_height);
		bool bQualityChanged = (pEntry->m_resolvedQuality != resolvedQuality);
		bool bFactorChanged = (pEntry->m_resolutionFactor != profile.m_resolutionFactor);
		bool bMsaaChanged = (pEntry->m_msaaSamples != actualMsaa);

		if (!bConfigChanged && !bTargetChanged && !bExtentChanged && !bQualityChanged && !bFactorChanged && !bMsaaChanged)
		{
			if (pEntry->m_graph && pEntry->m_output)
			{
				outOutput = pEntry->m_output;
				outRenderExtent = pEntry->m_renderExtent;
				return pEntry->m_graph;
			}
		}
	}

	// Build new graph
	FrameGraphPtr newGraph{};
	auto importer = App::GetSubmodule<FrameGraphImporter>();
	if (!importer)
	{
		return {};
	}

	if (!importer->Instantiate(configUid, newGraph, outputExtent, resolvedQuality, profile))
	{
		return {};
	}

	RHIRenderTargetPtr newOutput{};
	if (targetTexture)
	{
		newOutput = targetTexture.DynamicCast<RHIRenderTarget>();
	}
	else
	{
		newOutput = m_driverInstance->CreateRenderTarget(glm::ivec2(static_cast<int>(outputExtent.m_width), static_cast<int>(outputExtent.m_height)), 1, GetColorFormat());
	}
	if (!newOutput)
	{
		return {};
	}

	// Update or create entry
	if (!pEntry)
	{
		m_cameraGraphCache.Add(CameraGraphEntry{});
		pEntry = &m_cameraGraphCache[m_cameraGraphCache.Num() - 1];
		pEntry->m_world = world;
		pEntry->m_cameraInstanceId = cameraId;
	}

	pEntry->m_graph = newGraph;
	pEntry->m_output = newOutput;
	outOutput = newOutput;
	pEntry->m_renderExtent = newGraph->GetRHI()->GetSceneRenderExtent();
	outRenderExtent = pEntry->m_renderExtent;
	pEntry->m_target = targetTexture;
	pEntry->m_configUid = configUid;
	pEntry->m_configRevision = configRevision;
	pEntry->m_targetRevision = targetRevision;
	pEntry->m_resolvedQuality = resolvedQuality;
	pEntry->m_resolutionFactor = profile.m_resolutionFactor;
	pEntry->m_outputExtent = outputExtent;
	pEntry->m_msaaSamples = actualMsaa;
	pEntry->m_lastSeenEpoch = epoch;

	return newGraph;
}

void Renderer::PruneCameraGraphEntries(WorldPtr world, uint64_t epoch)
{
	for (size_t index = m_cameraGraphCache.Num(); index > 0; --index)
	{
		size_t i = index - 1;
		if (m_cameraGraphCache[i].m_world == world)
		{
			if (epoch == 0ull || m_cameraGraphCache[i].m_lastSeenEpoch != epoch)
			{
				m_cameraGraphCache.RemoveAt(i);
			}
		}
	}
}

void Renderer::QueueEditorReadback(ReadbackFramePtr frame)
{
	if (!frame) return;
	Tasks::CreateTask("Publish editor readback"_h, [this, frame = std::move(frame)]()
		{
			if (frame->m_generation == m_frameGraphResourceGeneration &&
				(!m_editorReadback || frame->m_frameIndex > m_editorReadback->m_frameIndex))
			{
				m_editorReadback = frame;
			}
		}, EThreadType::Main)->Run();
}

bool Renderer::EnsureFrameGraph()
{
	if (m_frameGraph && !m_bFrameGraphOutdated)
	{
		return true;
	}

	if (!m_bIsInitialized || !m_driverInstance || !m_pViewport || m_pViewport->IsIconic())
	{
		return false;
	}

	// Imported samplers must finish loading before the first graph can be built.
	m_driverInstance->TrackResources_ThreadSafe();
	RefreshGpuTimings();
	m_editorReadback.Clear();
	m_bHasEditorReadback = false;
	++m_frameGraphResourceGeneration;
	m_frameGraph.Clear();

	const char* frameGraphAssetPath = FrameGraphImporter::GetRendererAssetPath();
	if (auto frameGraphFileId = App::GetSubmodule<AssetRegistry>()->GetAssetInfoPtr<AssetInfoPtr>(frameGraphAssetPath))
	{
		App::GetSubmodule<FrameGraphImporter>()->Instantiate(frameGraphFileId->GetFileId(), m_frameGraph);
	}
	else
	{
		SAILOR_LOG_ERROR("Renderer::EnsureFrameGraph failed: %s asset info was not found.", frameGraphAssetPath);
	}

	m_bFrameGraphOutdated = false;
	if (m_frameGraph)
	{
		auto graph = m_frameGraph->GetRHI();
		auto readback = Framegraph::EditorReadbackNode::Find(*graph);
#if defined(__APPLE__) || defined(__linux__)
		// Mac and GTK hosts consume completed CPU frames; Windows copies the image on GPU.
		if (App::HasEditor() && !readback)
		{
			readback = TRefPtr<Framegraph::EditorReadbackNode>::Make();
			readback->SetTag("EditorReadback"_h);
			graph->GetGraph().Add(readback);
		}
#endif
		m_bHasEditorReadback = readback.IsValid();
	}
	return m_frameGraph.IsValid();
}

void Renderer::UpdateSkyParameters(WorldPtr world, RHIFrameGraphPtr graph)
{
	auto node = graph->GetGraphNode("Sky"_h).DynamicCast<Framegraph::SkyNode>();
	if (!node)
	{
		m_skyNode.Clear();
		return;
	}
	const auto sky = world->GetECS<LightingECS>()->GetSky();
	const SkyParameters parameters = sky ? sky->GetSkyParameters() : SkyParameters{};
	if (node == m_skyNode && parameters == m_publishedSkyParams) return;
	m_skyNode = node;
	m_publishedSkyParams = parameters;
	auto update = Tasks::CreateTask("Update sky parameters"_h, [node, parameters]() mutable
		{
			node->SetSkyParams(parameters);
		}, EThreadType::Render);
	// The previous frame may still be waiting for RHI preparation.
	if (m_previousRenderFrame) update->Join(m_previousRenderFrame);
	update->Run();
}

bool Renderer::PushFrame(const Sailor::FrameState& frame)
{
	SAILOR_PROFILE_FUNCTION();
	if (!m_bIsInitialized || !m_driverInstance)
	{
		return false;
	}

	if (m_bForceStop || !CanPrepareFrame())
	{
		return false;
	}

	if (m_previousRenderFrame.IsValid() && m_previousRenderFrame->IsFinished())
	{
		m_previousRenderFrame.Clear();
	}

	auto* world = frame.GetWorld();
	if (!world)
	{
		return false;
	}

	m_driverInstance->TrackResources_ThreadSafe();

	const uint64_t submissionId = m_nextSubmissionId.fetch_add(1ull, std::memory_order_relaxed);
	auto submission = TSharedPtr<FrameSubmission>::Make(submissionId);

	const auto& activeCameras = world->GetECS<CameraECS>()->GetActiveCameras();
	const uint32_t cameraCount = static_cast<uint32_t>(activeCameras.Num());

	size_t primaryPassIndex = (std::numeric_limits<size_t>::max)();
	FrameGraphPtr primaryGraph;
	bool bFoundEditorPrimary = false;
	bool bFoundNoTargetPrimary = false;

	for (uint32_t i = 0; i < cameraCount; ++i)
	{
		const auto& cameraData = activeCameras[i];
		RHIRenderTargetPtr outOutput;
		glm::ivec2 outExtent;

		FrameGraphPtr graph = ResolveCameraGraph(world, cameraData, submissionId, outOutput, outExtent);
		if (!graph)
		{
			continue;
		}

		FrameSubmission::CameraPass pass;
		pass.m_cameraIndex = i;
		pass.m_frameGraph = graph->GetRHI();
		pass.m_output = outOutput;
		pass.m_renderExtent = outExtent;

		// Resolve quality from App project settings, not cached graph mutable state
		const auto defaultTier = cameraData.GetEditorRole() ? App::GetActiveGraphicsQuality() : App::GetGraphicsSettings().m_defaultQuality;
		pass.m_quality = Settings::ResolveQualitySelection(cameraData.GetQualityPreset(), defaultTier);
		pass.m_qualityProfile = App::GetGraphicsSettings().GetProfile(pass.m_quality);

		// Copy sky parameters once from world LightingECS
		if (auto* lightingEcs = world->GetECS<LightingECS>())
		{
			const auto sky = lightingEcs->GetSky();
			pass.m_skyParameters = sky ? sky->GetSkyParameters() : SkyParameters{};
		}

		// Determine presentation intent
		const bool bHasPresentationIntent = pass.m_frameGraph->HasPresentationIntent();
		const bool bHasAuthoredTarget = static_cast<bool>(cameraData.GetRenderTarget());
		pass.m_bWantsPresent = bHasPresentationIntent && !bHasAuthoredTarget && !App::HasEditor();

		if (pass.m_bWantsPresent)
		{
			submission->m_bWantsPresent = true;
		}

		submission->m_cameraPasses.Add(pass);

		// Primary selection logic
		if (primaryPassIndex == (std::numeric_limits<size_t>::max)() ||
			(cameraData.GetEditorRole() && !bFoundEditorPrimary) ||
			(!bFoundEditorPrimary && !bFoundNoTargetPrimary && !bHasAuthoredTarget))
		{
			primaryPassIndex = submission->m_cameraPasses.Num() - 1;
			primaryGraph = graph;
			bFoundEditorPrimary = cameraData.GetEditorRole();
			bFoundNoTargetPrimary = !bHasAuthoredTarget;
		}
	}

	PruneCameraGraphEntries(world, submissionId);

	if (submission->m_cameraPasses.IsEmpty())
	{
		m_frameGraph.Clear();
		m_editorReadback.Clear();
		m_bHasEditorReadback = false;
		m_bFrameGraphOutdated = false;
		++m_frameGraphResourceGeneration;
		return false;
	}

	const auto& primaryPass = submission->m_cameraPasses[primaryPassIndex];
	const bool bPrimaryChanged = (m_frameGraph != primaryGraph);
	const bool bWasOutdated = m_bFrameGraphOutdated.exchange(false);

	if (bPrimaryChanged || bWasOutdated)
	{
		RefreshGpuTimings();
		m_editorReadback.Clear();
		++m_frameGraphResourceGeneration;
	}

	m_frameGraph = primaryGraph;
	submission->m_frameGraph = primaryPass.m_frameGraph;
	submission->m_frameGraphResourceGeneration = m_frameGraphResourceGeneration;

	m_bHasEditorReadback = Framegraph::EditorReadbackNode::Find(*submission->m_frameGraph).IsValid();

	CaptureSceneView(*submission, frame);

	auto render = Tasks::CreateTask("Render Frame"_h,
		[this, frame, submission]() mutable
		{
			// Release capture when execution ends, not when the retained task dies.
			auto current = std::move(submission);
			if (current->m_context)
			{
				RecordAndSubmitFrame(*current, frame);
				CompleteFrame(*current);
			}
			else
			{
				current->m_sceneView->CompleteSubmissionResources(false);
				if (current->m_reflections && current->m_reflectionCapture)
				{
					current->m_reflections->Cancel(current->m_reflectionCapture);
				}
				if (current->m_reflectionProbes)
				{
					for (const auto& capture : current->m_probeCaptures)
					{
						current->m_reflectionProbes->Cancel(capture);
					}
				}
				ReturnSceneView(current->m_sceneView);
			}
		}, EThreadType::Render);

	auto prepare = Tasks::CreateTask("Prepare render submission flight"_h,
		[this, submission = std::move(submission), render]() mutable
		{
			auto current = std::move(submission);
			auto record = std::move(render);
			const bool bAcquired = AcquireSubmission(*current);
			if (bAcquired)
			{
				PrepareSceneView(*current);
				if (current->m_reflections)
				{
					current->m_reflections->Poll(current->m_frameGraph);
				}
				if (current->m_reflectionProbes)
				{
					current->m_reflectionProbes->Poll(current->m_frameGraph);
				}

				TVector<Sailor::Tasks::ITaskPtr> allTasks;
				for (const auto& task : current->m_sceneView->m_debugDraw)
				{
					allTasks.Add(task);
				}

				// Iterate all passes for preparation
				for (auto& pass : current->m_cameraPasses)
				{
					// Retain resources to prevent reuse before GPU flight fence
					current->m_context->RetainResource(pass.m_frameGraph);
					current->m_context->RetainResource(pass.m_output);

					// Set quality
					pass.m_frameGraph->SetCameraQuality(pass.m_quality, pass.m_qualityProfile);

					// Update Sky node from copied params
					if (auto skyNode = pass.m_frameGraph->GetGraphNode("Sky"_h).DynamicCast<Framegraph::SkyNode>())
					{
						skyNode->SetSkyParams(pass.m_skyParameters);
					}

					// Bind BackBuffer if needed
					pass.m_bUsesNativeOutput = pass.m_bWantsPresent && current->m_bHasSwapchainImage && (!m_pViewport || !m_pViewport->IsIconic());
					if (pass.m_bUsesNativeOutput)
					{
						pass.m_frameGraph->SetRenderTarget("BackBuffer"_h, m_driverInstance->GetBackBuffer());
					}
					else
					{
						pass.m_frameGraph->SetRenderTarget("BackBuffer"_h, pass.m_output);
					}

					// Prepare graph
					auto passTasks = pass.m_frameGraph->Prepare(current->m_sceneView, pass.m_cameraIndex);
					for (auto& task : passTasks)
					{
						if (!allTasks.Contains(task))
						{
							allTasks.Add(task);
						}
					}
				}

				// Handle Reflection/Probe captures with primary graph context (default all)
				if (current->m_reflectionCapture)
				{
					auto captureTasks = current->m_reflectionCapture->Prepare(current->m_frameGraph, current->m_context);
					for (auto& task : captureTasks)
					{
						if (!allTasks.Contains(task))
						{
							allTasks.Add(task);
						}
					}
				}
				for (auto& capture : current->m_probeCaptures)
				{
					auto captureTasks = capture->Prepare(current->m_frameGraph, current->m_context);
					for (auto& task : captureTasks)
					{
						if (!allTasks.Contains(task))
						{
							allTasks.Add(task);
						}
					}
				}

				for (auto& task : allTasks)
				{
					record->Join(task);
				}
				for (auto& task : allTasks)
				{
					task->Run();
				}
			}
			// Sample the next CPU frame after flight setup; node tasks and recording still overlap it.
			m_bIsFrameQueued.store(false, std::memory_order_release);
			record->Run();
		}, EThreadType::Render);

	if (m_previousRenderFrame)
	{
		prepare->Join(m_previousRenderFrame);
	}
	m_previousRenderFrame = render;
	m_bIsFrameQueued.store(true, std::memory_order_relaxed);
	prepare->Run();
	return true;
}

void Renderer::CaptureSceneView(FrameSubmission& submission, const Sailor::FrameState& frame)
{
	auto& rhiSceneView = submission.m_sceneView;
	auto& rhiFrameGraph = submission.m_frameGraph;
	auto* world = frame.GetWorld();
	SAILOR_PROFILE_SCOPE("Copy scene view to render thread");

	rhiSceneView = GetOrAddSceneView(world);

	rhiSceneView->m_world = world;
	rhiSceneView->m_renderMode = App::GetEditorRenderMode();
	world->GetECS<StaticMeshRendererECS>()->CopySceneView(rhiSceneView);
	world->GetECS<LandscapeECS>()->AppendSceneView(rhiSceneView);

	if (auto* pathTracerEcs = world->GetECS<PathTracerECS>())
	{
		// Pathtracing enabled if ANY pass contains enabled CPUPathTracerNode
		bool tracing = false;
		for (const auto& pass : submission.m_cameraPasses)
		{
			const auto& graph = pass.m_frameGraph->GetGraph();
			const bool bPassTracing = std::any_of(graph.begin(), graph.end(),
				[mode = rhiSceneView->m_renderMode](const auto& node)
				{
					const auto* tracer = dynamic_cast<const Framegraph::CPUPathTracerNode*>(node.GetRawPtr());
					return tracer && tracer->IsEnabled(mode);
				});
			if (bPassTracing)
			{
				tracing = true;
				break;
			}
		}
		pathTracerEcs->SetPathTracingEnabled(tracing);
		pathTracerEcs->CopySceneView(rhiSceneView);
	}

	rhiSceneView->m_globalIlluminationMode =
		EGlobalIlluminationMode::Baked;
	rhiSceneView->m_bGlobalIlluminationEnabled = true;
	rhiSceneView->m_globalIllumination.Clear();
	if (auto* globalIlluminationEcs = world->GetECS<GlobalIlluminationECS>())
	{
		rhiSceneView->m_globalIlluminationMode =
			globalIlluminationEcs->GetWorldSettings().m_mode;
		rhiSceneView->m_bGlobalIlluminationEnabled =
			globalIlluminationEcs->IsEnabled();
		rhiSceneView->m_globalIllumination =
			rhiSceneView->m_bGlobalIlluminationEnabled &&
			rhiSceneView->m_globalIlluminationMode !=
				EGlobalIlluminationMode::NoGI
				? globalIlluminationEcs->GetActiveSnapshot()
				: RHIGlobalIlluminationSnapshotPtr{};
	}

	world->GetECS<CameraECS>()->CopyCameraData(rhiSceneView);

	// Initialize and fill camera render extents
	const uint32_t cameraCount = static_cast<uint32_t>(rhiSceneView->m_cameras.Num());
	rhiSceneView->m_cameraRenderExtents.Resize(cameraCount);
	for (uint32_t i = 0; i < cameraCount; ++i)
	{
		rhiSceneView->m_cameraRenderExtents[i] = glm::ivec2(1);
	}
	for (const auto& pass : submission.m_cameraPasses)
	{
		if (pass.m_cameraIndex < cameraCount)
		{
			rhiSceneView->m_cameraRenderExtents[pass.m_cameraIndex] = pass.m_renderExtent;
		}
	}

	rhiSceneView->m_deltaTime = frame.GetDeltaTime();
	rhiSceneView->m_currentTime = world->GetTime();
	world->GetECS<AnimationECS>()->FillAnimationData(rhiSceneView);
	rhiSceneView->m_drawImGui = frame.GetDrawImGuiTask();

	glm::ivec2 primaryExtent(1);
	for (const auto& pass : submission.m_cameraPasses)
	{
		if (pass.m_frameGraph == submission.m_frameGraph)
		{
			primaryExtent = pass.m_renderExtent;
			break;
		}
	}
	rhiSceneView->PrepareDebugDrawCommandLists(world, primaryExtent);

	auto lighting = world->GetECS<LightingECS>();
	const uint32_t firstCaptureCamera = static_cast<uint32_t>(rhiSceneView->m_cameras.Num());
	submission.m_reflections = lighting->GetLocalReflectionState();
	if (submission.m_reflections)
	{
		submission.m_reflectionCapture = submission.m_reflections->Capture(rhiSceneView);
	}
	if (auto* reflections = world->GetECS<ReflectionsECS>())
	{
		submission.m_reflectionProbes = reflections->GetRenderState();
		if (submission.m_reflectionProbes)
		{
			rhiSceneView->m_reflections = submission.m_reflectionProbes->GetSnapshot();
			submission.m_probeCaptures = submission.m_reflectionProbes->Capture(rhiSceneView);
		}
	}
	if (rhiSceneView->m_cameras.Num() > firstCaptureCamera)
	{
		rhiSceneView->m_firstShadowCamera = firstCaptureCamera;
	}
	const uint32_t shadowSlot = static_cast<uint32_t>(
		(submission.m_submissionId - 1u) % m_submissionContexts.Num());
	lighting->FillLightingData(rhiSceneView, shadowSlot);
	rhiSceneView->PrepareSnapshots();
	// Extraction removes the appended camera tail, so detach jobs in reverse order.
	for (size_t index = submission.m_probeCaptures.Num(); index > 0u; --index)
	{
		submission.m_probeCaptures[index - 1u]->ExtractSnapshots(rhiSceneView);
	}
	if (submission.m_reflectionCapture)
	{
		submission.m_reflectionCapture->ExtractSnapshots(rhiSceneView);
	}
	for (const auto& pass : submission.m_cameraPasses)
	{
		if (pass.m_frameGraph != submission.m_frameGraph && pass.m_cameraIndex < rhiSceneView->m_snapshots.Num())
		{
			rhiSceneView->m_snapshots[pass.m_cameraIndex].m_drawImGui.Clear();
		}
	}

}

bool Renderer::AcquireSubmission(FrameSubmission& submission)
{
	auto& rhiFrameGraph = submission.m_frameGraph;
	uint32_t flightSlot = 0u;
	// Third argument ALWAYS true (deferred presentation)
	const bool bAcquired =
		m_driverInstance->BeginRenderSubmission(
			flightSlot,
			submission.m_bHasSwapchainImage,
			true);
	if (auto readback = Framegraph::EditorReadbackNode::Find(*rhiFrameGraph))
	{
		QueueEditorReadback(readback->TakeCompletedFrame());
	}

	if (bAcquired &&
		flightSlot < m_submissionContexts.Num())
	{
		auto context = m_submissionContexts[flightSlot];
		context->BeginSubmission(
			submission.m_submissionId,
			flightSlot,
			submission.m_materialRevision,
			submission.m_frameGraphResourceGeneration);
		submission.m_context = std::move(context);
	}
	else
	{
		ResetFrameCadence();
		InvalidateGpuTimings();
		SAILOR_LOG_ERROR(
			"Renderer::PushFrame: failed to acquire render submission flight slot.");
	}

	this->GetDriver()->TrackResources_ThreadSafe();
	return static_cast<bool>(submission.m_context);
}

void Renderer::PrepareSceneView(FrameSubmission& submission)
{
	auto& rhiSceneView = submission.m_sceneView;
	SAILOR_PROFILE_SCOPE("Prepare flight-local scene view");
	auto& context = submission.m_context;
	for (const auto& spatialVersion : rhiSceneView->m_sceneVersions)
	{
		if (!spatialVersion || !spatialVersion->m_scene ||
			!spatialVersion->m_sceneVersion)
		{
			continue;
		}

		auto scene = spatialVersion->m_scene;
		auto flightState = scene->PrepareFlight(
			context->GetFlightSlot(),
			spatialVersion->m_sceneVersion);
		context->RetainResource(scene);
		context->RetainResource(spatialVersion->m_sceneVersion);
		context->RetainResource(flightState);
		scene->CollectGarbage();
	}
	rhiSceneView->SetSubmissionContext(context);
}

void Renderer::RecordAndSubmitFrame(FrameSubmission& submission, const Sailor::FrameState& frame)
{
	auto& rhiSceneView = submission.m_sceneView;
	auto& rhiFrameGraph = submission.m_frameGraph;
	SAILOR_PROFILE_SCOPE("Render Frame");
	auto& frameSubmission = submission.m_result;
	const bool bGpuQueriesEnabled = App::GetRenderStatsMode() ==
		Settings::ERenderStatsMode::RenderStatsAndQueries &&
		m_driverInstance->SupportsGpuFrameTimeQueries();
	if (m_timings.GetGpuTimings().m_generation != m_gpuTimingGeneration.load(std::memory_order_acquire) ||
		m_profiledFrameGraphGeneration != submission.m_frameGraphResourceGeneration ||
		m_bGpuQueriesEnabled != bGpuQueriesEnabled)
	{
		m_profiledFrameGraphGeneration = submission.m_frameGraphResourceGeneration;
		m_bGpuQueriesEnabled = bGpuQueriesEnabled;
		InvalidateGpuTimings();
	}

	TVector<RHI::RHICommandListPtr> primaryCommandLists;
	TVector<RHI::RHICommandListPtr> transferCommandLists;

	auto updateFrameRHI = [&frame](RHISemaphorePtr& inOutChainSemaphore)
		{
			SAILOR_PROFILE_SCOPE("Submit & Wait frame command lists");
			for (uint32_t i = 0; i < FrameState::NumCommandLists; i++)
			{
				if (auto pCommandList = frame.GetCommandBuffer(i))
				{
					auto signalSemaphore = GetDriver()->CreateWaitSemaphore();
					auto fence = RHIFencePtr::Make();
					GetDriver()->SetDebugName(signalSemaphore, std::format("frameInstance CommandBuffer {}", i));
					GetDriver()->SetDebugName(fence, std::format("frameInstance CommandBuffer {}", i));

					if (!GetDriver()->SubmitCommandList(pCommandList, fence, signalSemaphore, inOutChainSemaphore))
					{
						SAILOR_LOG_ERROR("Renderer::PushFrame: failed to submit frame command buffer %u.", i);
						return false;
					}

					inOutChainSemaphore = signalSemaphore;
				}
			}

			return true;
		};

	const bool bHasSwapchainImage = submission.m_bHasSwapchainImage;
	bool bFrameGraphProcessed = false;
	bool bNativeOutputProcessed = false;
	RHISemaphorePtr chainSemaphore{};
	const bool bCanRenderFrame = !m_bForceStop;
	bool bFrameSubmitsSucceeded = true;
	bool bGpuFrameTimeQueryStarted = false;
	if (bCanRenderFrame && bGpuQueriesEnabled)
	{
		bGpuFrameTimeQueryStarted =
			m_driverInstance->BeginGpuFrameTimeQuery(m_timings.GetGpuTimings().m_generation);
	}

	if (bFrameSubmitsSucceeded && !m_bForceStop)
	{
		bFrameSubmitsSucceeded = updateFrameRHI(chainSemaphore);
	}
	DrawCallStats drawCallStats;
	RHIGlobalIlluminationRenderStats globalIlluminationStats;

	if (bFrameSubmitsSucceeded && submission.m_reflectionCapture)
	{
		if (bCanRenderFrame)
		{
			if (!submission.m_reflectionCapture->Submit(chainSemaphore))
			{
				bFrameSubmitsSucceeded = false;
				SAILOR_LOG_ERROR("Renderer::PushFrame: local reflection capture submission failed.");
			}
		}
		else
		{
			submission.m_reflectionCapture->m_bRetry = true;
		}
	}
	for (auto& capture : submission.m_probeCaptures)
	{
		if (bFrameSubmitsSucceeded && bCanRenderFrame)
		{
			if (!capture->Submit(chainSemaphore))
			{
				bFrameSubmitsSucceeded = false;
				SAILOR_LOG_ERROR("Renderer::PushFrame: reflection probe capture submission failed.");
			}
		}
		else
		{
			capture->m_bRetry = true;
		}
	}

	if (bFrameSubmitsSucceeded && bCanRenderFrame)
	{
		auto flushPending = [this](TVector<RHI::RHICommandListPtr>& transfers,
			TVector<RHI::RHICommandListPtr>& graphics,
			RHISemaphorePtr& chain) -> bool
		{
			uint32_t idx = 0;
			for (auto& cmd : transfers)
			{
				auto signal = GetDriver()->CreateWaitSemaphore();
				auto fence = RHIFencePtr::Make();
				GetDriver()->SetDebugName(signal, "Camera pass transfer"_h);
				GetDriver()->SetDebugName(fence, "Camera pass transfer"_h);
				if (!GetDriver()->SubmitCommandList(cmd, fence, signal, chain))
				{
					SAILOR_LOG_ERROR("Renderer::PushFrame: failed to submit camera pass transfer command buffer %u.", idx);
					return false;
				}
				chain = signal;
				idx++;
			}
			transfers.Clear();
			idx = 0;
			for (auto& cmd : graphics)
			{
				auto signal = GetDriver()->CreateWaitSemaphore();
				auto fence = RHIFencePtr::Make();
				GetDriver()->SetDebugName(signal, "Camera pass graphics"_h);
				GetDriver()->SetDebugName(fence, "Camera pass graphics"_h);
				if (!GetDriver()->SubmitCommandList(cmd, fence, signal, chain))
				{
					SAILOR_LOG_ERROR("Renderer::PushFrame: failed to submit camera pass graphics command buffer %u.", idx);
					return false;
				}
				chain = signal;
				idx++;
			}
			graphics.Clear();
			return true;
		};

		for (auto& pass : submission.m_cameraPasses)
		{
			if (!flushPending(transferCommandLists, primaryCommandLists, chainSemaphore))
			{
				bFrameSubmitsSucceeded = false;
				break;
			}

			RHISemaphorePtr outChain{};
			const bool bPassSucceeded = pass.m_frameGraph->Process(
				rhiSceneView,
				transferCommandLists,
				primaryCommandLists,
				chainSemaphore,
				outChain,
				true,
				pass.m_cameraIndex);
			chainSemaphore = outChain;

			if (!bPassSucceeded)
			{
				SAILOR_LOG_ERROR("Renderer::PushFrame: FrameGraph command buffer submission failed for camera %u.", pass.m_cameraIndex);
				bFrameSubmitsSucceeded = false;
				break;
			}

			bFrameGraphProcessed = true;
			bNativeOutputProcessed = bNativeOutputProcessed || pass.m_bUsesNativeOutput;
			drawCallStats += pass.m_frameGraph->GetDrawCallStats();

			if (pass.m_frameGraph == submission.m_frameGraph)
			{
				globalIlluminationStats = pass.m_frameGraph->GetGlobalIlluminationRenderStats();
			}

			if (!pass.m_bUsesNativeOutput)
			{
				auto cmd = GetDriver()->CreateCommandList(false, RHI::ECommandListQueue::Graphics);
				auto commands = GetDriverCommands();
				commands->BeginCommandList(cmd, true);
				commands->ImageMemoryBarrier(cmd, pass.m_output, EImageLayout::ShaderReadOnlyOptimal);
				pass.m_output->ForceSetDefaultLayout(EImageLayout::ShaderReadOnlyOptimal);
				commands->EndCommandList(cmd);
				primaryCommandLists.Add(cmd);
			}
		}
	}

	if (bFrameSubmitsSucceeded)
	{
		SAILOR_PROFILE_SCOPE("Submit transfer command lists");
		uint32_t i = 0;
		for (auto& cmdList : transferCommandLists)
		{
			auto signalSemaphore = GetDriver()->CreateWaitSemaphore();
			auto fence = RHIFencePtr::Make();
			GetDriver()->SetDebugName(signalSemaphore, std::format("rhiFrameGraph TransferCommandList {}", i));
			GetDriver()->SetDebugName(fence, std::format("rhiFrameGraph TransferCommandList {}", i));

			if (!GetDriver()->SubmitCommandList(cmdList, fence, signalSemaphore, chainSemaphore))
			{
				SAILOR_LOG_ERROR("Renderer::PushFrame: failed to submit FrameGraph transfer command buffer %u.", i);
				bFrameSubmitsSucceeded = false;
				break;
			}

			chainSemaphore = signalSemaphore;
			i++;
		}
	}

	if (bGpuFrameTimeQueryStarted)
	{
		if (bFrameSubmitsSucceeded)
		{
			m_driverInstance->EndGpuFrameTimeQuery();
		}
		else
		{
			m_driverInstance->CancelGpuFrameTimeQuery();
		}
	}

	TVector<RHISemaphorePtr> waitFrameUpdate;
	if (chainSemaphore)
	{
		waitFrameUpdate.Add(chainSemaphore);
		if (bFrameSubmitsSucceeded && submission.m_context)
		{
			submission.m_context->SetResourceReadySemaphore(chainSemaphore);
		}
	}

	if (!bFrameSubmitsSucceeded)
	{
		TVector<RHICommandListPtr> noCommandLists;
		const FrameSubmissionResult flightRelease = m_driverInstance->SubmitFrameWithoutPresent(
			noCommandLists,
			waitFrameUpdate);
		if (!flightRelease.m_bSubmitted)
		{
			SAILOR_LOG_ERROR("Renderer::PushFrame: failed to fence an acquired flight slot after a submit failure.");
		}
	}

	if (bFrameSubmitsSucceeded)
	{
		const bool bShouldPresent = submission.m_bWantsPresent && bHasSwapchainImage && !m_bForceStop && (!m_pViewport || !m_pViewport->IsIconic()) && bNativeOutputProcessed && bFrameGraphProcessed;
		const auto completion = submission.m_context->GetFrameCompletion();
		if (bShouldPresent)
		{
			frameSubmission = m_driverInstance->PresentFrame(frame, primaryCommandLists, waitFrameUpdate, completion);
		}
		else
		{
			frameSubmission = m_driverInstance->SubmitFrameWithoutPresent(primaryCommandLists, waitFrameUpdate, completion);
		}
	}

	const auto gpuTimingResult = m_driverInstance->TakeGpuTimingResult();
	if (bFrameGraphProcessed && frameSubmission.m_bSubmitted)
	{
		PublishGpuTimings(gpuTimingResult);
		if (m_timings.RecordFrame(RendererTimings::Clock::now(), frameSubmission, m_driverInstance->GetPresentedFrameCount()))
		{
			m_stats.m_renderFps.store(m_timings.GetRenderFps(), std::memory_order_relaxed);
			m_stats.m_presentFps.store(m_timings.GetPresentFps(), std::memory_order_relaxed);
#if defined(SAILOR_BUILD_WITH_VULKAN)
			size_t heapUsage = 0;
			size_t heapBudget = 0;

			VulkanApi::GetInstance()->GetMainDevice()->GetOccupiedVideoMemory(VkMemoryHeapFlagBits::VK_MEMORY_HEAP_DEVICE_LOCAL_BIT, heapBudget, heapUsage);

			m_stats.m_gpuHeapUsage = heapUsage;
			m_stats.m_gpuHeapBudget = heapBudget;
			m_stats.m_numSubmittedCommandBuffers = m_driverInstance->GetNumSubmittedCommandBuffers();
			UpdateMemoryStats();
#endif // SAILOR_BUILD_WITH_VULKAN
		}
	}
	else
	{
		ResetFrameCadence();
		InvalidateGpuTimings();
	}

	const bool bFrameCompleted = frameSubmission.m_bSubmitted;
	if (bFrameCompleted)
	{
		submission.m_bResourcesSucceeded = bFrameGraphProcessed &&
			(!submission.m_reflectionCapture || submission.m_reflectionCapture->m_bSubmitted);
		UpdateGlobalIlluminationRenderStats(
			globalIlluminationStats);
		m_stats.m_numBatches.store(drawCallStats.m_numBatches, std::memory_order_relaxed);
		m_stats.m_numInstances.store(drawCallStats.m_numInstances, std::memory_order_relaxed);
	}
	else
	{
		UpdateGlobalIlluminationRenderStats({});
		if (submission.m_context)
		{
			submission.m_context->InvalidateSubmissionResources();
		}
		m_stats.m_numBatches.store(0u, std::memory_order_relaxed);
		m_stats.m_numInstances.store(0u, std::memory_order_relaxed);
	}
}

void Renderer::CompleteFrame(FrameSubmission& submission)
{
	auto& rhiSceneView = submission.m_sceneView;
	auto& rhiFrameGraph = submission.m_frameGraph;
	if (!submission.m_result.m_bSubmitted)
	{
		if (auto completion = submission.m_context->GetFrameCompletion()) completion->MarkSubmissionFailed();
	}
	if (auto readback = Framegraph::EditorReadbackNode::Find(*rhiFrameGraph))
	{
		QueueEditorReadback(readback->TakeCompletedFrame());
	}
	rhiSceneView->CompleteSubmissionResources(
		submission.m_bResourcesSucceeded);

	SAILOR_PROFILE_SCOPE("Clear after Present");
	// Bound deferred ownership to one older frame. Normally its release
	// overlaps the next frame's setup and is already complete here.
	if (m_previousSceneVersionRelease)
	{
		SAILOR_PROFILE_SCOPE("Wait previous scene version release");
		m_previousSceneVersionRelease->Wait();
		m_previousSceneVersionRelease.Clear();
	}
	if (!rhiSceneView->m_sceneVersions.IsEmpty() ||
		!rhiSceneView->m_virtualSceneVersions.IsEmpty() ||
		rhiSceneView->m_retainedSceneVersions)
	{
		m_previousSceneVersionRelease = Tasks::CreateTask("Release submitted scene versions"_h,
			[spatial = std::move(rhiSceneView->m_sceneVersions),
				versions = std::move(rhiSceneView->m_virtualSceneVersions),
				retained = std::move(rhiSceneView->m_retainedSceneVersions)]() mutable
			{
				SAILOR_PROFILE_SCOPE("Reclaim submitted scene versions");
				spatial.Clear();
				versions.Clear();
				retained.Clear();
			}, EThreadType::Worker);
	}
	// Drop snapshot references before the worker runs, so the last
	// reference (and expensive root destruction) stays with the task.
	ReturnSceneView(rhiSceneView);
	if (m_previousSceneVersionRelease)
	{
		m_previousSceneVersionRelease->Run();
	}
	GetDriver()->CollectGarbage_RenderThread();
}

void Renderer::ReturnSceneView(RHISceneViewPtr& rhiSceneView)
{
	SAILOR_PROFILE_SCOPE("Return scene view to cache");
	rhiSceneView->Clear();
	auto& list = m_cachedSceneViews.At_Lock(rhiSceneView->m_world);
	auto it = list.FindIf([&](const auto& el) { return el.m_first == rhiSceneView; });
	if (it != list.end())
	{
		(*it).m_second = true;
	}

	m_cachedSceneViews.Unlock(rhiSceneView->m_world);
}

void Renderer::WaitIdle()
{
	if (m_previousRenderFrame.IsValid())
	{
		m_previousRenderFrame->Wait();
		m_previousRenderFrame.Clear();
	}
	if (m_previousSceneVersionRelease)
	{
		m_previousSceneVersionRelease->Wait();
		m_previousSceneVersionRelease.Clear();
	}

	if (m_driverInstance)
	{
		m_driverInstance->WaitIdle();
	}
}
