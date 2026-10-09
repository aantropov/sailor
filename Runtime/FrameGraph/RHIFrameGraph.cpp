#include "RHIFrameGraph.h"
#include "Containers/Hash.h"
#include "RHI/SceneView.h"
#include "RHI/Renderer.h"
#include "RHI/GraphicsDriver.h"
#include "RHI/ViewSubmissionResources.h"
#include "RHI/Shader.h"
#include "RHI/VertexDescription.h"
#include "RHI/RenderTarget.h"
#include "RHI/Surface.h"
#include "RHI/Cubemap.h"
#include "RHI/CommandList.h"
#include "FrameGraph/EnvironmentNode.h"
#include "AssetRegistry/Texture/TextureImporter.h"
#include "Settings/GraphicsSettings.h"
#include "Tasks/Tasks.h"
#include "Core/LogMacros.h"

#include <atomic>
#include <limits>

using namespace Sailor;
using namespace Sailor::RHI;

namespace
{
	class RHISubmissionProgress final
	{
	public:
		void SetLastSuccessfulSemaphore(RHISemaphorePtr semaphore)
		{
			m_lock.Lock();
			m_lastSuccessfulSemaphore = std::move(semaphore);
			m_lock.Unlock();
		}

		RHISemaphorePtr GetLastSuccessfulSemaphore() const
		{
			m_lock.Lock();
			auto result = m_lastSuccessfulSemaphore;
			m_lock.Unlock();
			return result;
		}

	private:
		mutable SpinLock m_lock;
		RHISemaphorePtr m_lastSuccessfulSemaphore{};
	};

	template<typename T>
	uint64_t HashSubmissionValues(const TVector<T>& values)
	{
		const size_t numBytes = values.Num() * sizeof(T);
		uint64_t result = HashBytes(values.GetData(), numBytes);
		HashValue(result, values.Num());
		return result;
	}

	void CloneTextureBindings(
		const RHIShaderBindingSetPtr& source,
		RHIShaderBindingSetPtr& destination)
	{
		if (!source || !destination)
		{
			return;
		}

		auto& driver = Renderer::GetDriver();
		for (const auto& entry : source->GetShaderBindings())
		{
			const auto& binding = entry.m_second;
			if (!binding || binding->GetTextureBindings().IsEmpty())
			{
				continue;
			}

			const auto& layout = binding->GetLayout();
			if (layout.m_type != EShaderBindingType::CombinedImageSampler)
			{
				continue;
			}

			driver->AddSamplerToShaderBindings(
				destination,
				entry.m_first,
				binding->GetTextureBindings(),
				layout.m_binding,
				layout.m_bVariableDescriptorCount,
				layout.m_arrayCount);
		}
	}

	void PrepareViewSubmissionResources(
		RHIFrameGraph* owner,
		RHICommandListPtr transferCommandList,
		RHISceneViewSnapshot& snapshot)
	{
		if (!snapshot.m_submissionContext)
		{
			return;
		}

		auto resources = snapshot.m_submissionContext->GetOrAddFrameGraphResources<RHIViewSubmissionResources>(
			owner,
			snapshot.m_cameraIndex,
			0u);
		auto sharedResources = snapshot.m_submissionContext->GetOrAddFrameGraphResources<RHISharedViewSubmissionResources>(
			owner,
			(std::numeric_limits<uint32_t>::max)(),
			1u);
		auto& driver = Renderer::GetDriver();
		auto commands = App::GetSubmodule<Renderer>()->GetDriverCommands();
		if (!resources->m_frameBindings)
		{
			resources->m_frameBindings = driver->CreateShaderBindings();
			driver->AddBufferToShaderBindings(
				resources->m_frameBindings,
				"frameData"_h,
				sizeof(UboFrameData),
				0u,
				EShaderBindingType::UniformBuffer);
		}
		snapshot.m_frameBindings = resources->m_frameBindings;

		auto lightsTemplate = snapshot.m_rhiLightsData;
		if (lightsTemplate == resources->m_lightsBindings && resources->m_lightsTemplate)
		{
			lightsTemplate = resources->m_lightsTemplate;
		}
		const uint64_t lightsTemplateRevision = lightsTemplate ?
			lightsTemplate->GetDescriptorRevision() : 0ull;
		const size_t numShadowMatrices = snapshot.m_shadowMatrices.Num();
		const size_t numShadowIndices = snapshot.m_shadowIndices.Num();
		const size_t numShadowAtlasTiles = snapshot.m_shadowAtlasTiles.Num();

		size_t frameGraphSamplerHash = 0u;
		HashCombine(frameGraphSamplerHash, Sailor::GetHash(owner->GetSampler("g_irradianceCubemap"_h)));
		HashCombine(frameGraphSamplerHash, Sailor::GetHash(owner->GetSampler("g_brdfSampler"_h)));
		HashCombine(frameGraphSamplerHash, Sailor::GetHash(owner->GetSampler("g_envCubemap"_h)));
		HashCombine(frameGraphSamplerHash, Sailor::GetHash(owner->GetSampler("g_sheenEnvCubemap"_h)));
		HashCombine(frameGraphSamplerHash, Sailor::GetHash(owner->GetSampler("g_localEnvCubemap"_h)));
		HashCombine(frameGraphSamplerHash, Sailor::GetHash(owner->GetSampler("g_localSheenEnvCubemap"_h)));
		HashCombine(frameGraphSamplerHash, Sailor::GetHash(owner->GetRenderTarget("g_AO"_h)));
		const bool bRecreateLights = !resources->m_lightsBindings ||
			resources->m_lightsTemplate != lightsTemplate ||
			resources->m_sharedLightsStorage != sharedResources->m_lightsStorage ||
			resources->m_sharedGlobalIlluminationStorage !=
				sharedResources->m_globalIlluminationStorage ||
			resources->m_lightsTemplateRevision != lightsTemplateRevision ||
			resources->m_frameGraphSamplerHash != frameGraphSamplerHash ||
			resources->m_shadowMatrixCapacity < numShadowMatrices ||
			resources->m_shadowIndexCapacity < numShadowIndices ||
			resources->m_shadowAtlasTileCapacity < numShadowAtlasTiles;

		if (bRecreateLights)
		{
			resources->m_shadowMatrixCapacity = GrowSubmissionCapacity(resources->m_shadowMatrixCapacity, numShadowMatrices);
			resources->m_shadowIndexCapacity = GrowSubmissionCapacity(resources->m_shadowIndexCapacity, numShadowIndices);
			resources->m_shadowAtlasTileCapacity = GrowSubmissionCapacity(resources->m_shadowAtlasTileCapacity, numShadowAtlasTiles);
			resources->m_lightsBindings = driver->CreateShaderBindings();
			driver->AddBufferToShaderBindings(resources->m_lightsBindings, "localReflection"_h,
				sizeof(LocalReflectionParameters), 20u, EShaderBindingType::UniformBuffer);
			driver->AddShaderBinding(
				resources->m_lightsBindings,
				sharedResources->m_lightsStorage->GetOrAddShaderBinding("light"_h),
				"light"_h,
				0u);
			driver->AddSsboToShaderBindings(
				resources->m_lightsBindings,
				"lightsMatrices"_h,
				sizeof(glm::mat4),
				resources->m_shadowMatrixCapacity,
				6u,
				true);
			driver->AddSsboToShaderBindings(
				resources->m_lightsBindings,
				"shadowIndices"_h,
				sizeof(uint32_t),
				resources->m_shadowIndexCapacity,
				7u,
				true);
			driver->AddSsboToShaderBindings(
				resources->m_lightsBindings,
				"shadowAtlasTiles"_h,
				sizeof(uint32_t),
				resources->m_shadowAtlasTileCapacity,
				11u,
				true);
			driver->AddShaderBinding(
				resources->m_lightsBindings,
				sharedResources->m_globalIlluminationStorage
					->GetOrAddShaderBinding("globalIlluminationHeader"_h),
				"globalIlluminationHeader"_h,
				12u);
			driver->AddShaderBinding(
				resources->m_lightsBindings,
				sharedResources->m_globalIlluminationStorage
					->GetOrAddShaderBinding("globalIlluminationBvh"_h),
				"globalIlluminationBvh"_h,
				13u);
			driver->AddShaderBinding(
				resources->m_lightsBindings,
				sharedResources->m_globalIlluminationStorage
					->GetOrAddShaderBinding("globalIlluminationBricks"_h),
				"globalIlluminationBricks"_h,
				14u);
			driver->AddShaderBinding(
				resources->m_lightsBindings,
				sharedResources->m_globalIlluminationStorage
					->GetOrAddShaderBinding("globalIlluminationProbes"_h),
				"globalIlluminationProbes"_h,
				15u);
			driver->AddShaderBinding(
				resources->m_lightsBindings,
				sharedResources->m_globalIlluminationStorage
					->GetOrAddShaderBinding("globalIlluminationCoefficients"_h),
				"globalIlluminationCoefficients"_h,
				16u);
			driver->AddShaderBinding(
				resources->m_lightsBindings,
				sharedResources->m_globalIlluminationStorage
					->GetOrAddShaderBinding("globalIlluminationStates"_h),
				"globalIlluminationStates"_h,
				17u);
			CloneTextureBindings(lightsTemplate, resources->m_lightsBindings);
			if (auto texture = owner->GetSampler("g_irradianceCubemap"_h))
			{
				driver->AddSamplerToShaderBindings(
					resources->m_lightsBindings,
					"g_irradianceCubemap"_h,
					texture,
					3u);
			}
			if (auto texture = owner->GetSampler("g_brdfSampler"_h))
			{
				driver->AddSamplerToShaderBindings(
					resources->m_lightsBindings,
					"g_brdfSampler"_h,
					texture,
					4u);
			}
			if (auto texture = owner->GetSampler("g_envCubemap"_h))
			{
				driver->AddSamplerToShaderBindings(
					resources->m_lightsBindings,
					"g_envCubemap"_h,
					texture,
					5u);
			}
			auto sheenEnvironment = owner->GetSampler("g_sheenEnvCubemap"_h);
			if (!sheenEnvironment)
			{
				sheenEnvironment = owner->GetSampler("g_envCubemap"_h);
			}
			if (sheenEnvironment)
			{
				driver->AddSamplerToShaderBindings(
					resources->m_lightsBindings,
					"g_sheenEnvCubemap"_h,
					sheenEnvironment,
					19u);
			}
			if (auto texture = owner->GetRenderTarget("g_AO"_h))
			{
				driver->AddSamplerToShaderBindings(resources->m_lightsBindings, "g_aoSampler"_h, texture, 8u);
			}
			auto localEnvironment = owner->GetSampler("g_localEnvCubemap"_h);
			if (!localEnvironment) localEnvironment = owner->GetSampler("g_envCubemap"_h);
			if (localEnvironment)
				driver->AddSamplerToShaderBindings(resources->m_lightsBindings, "g_localEnvCubemap"_h, localEnvironment, 21u);
			auto localSheen = owner->GetSampler("g_localSheenEnvCubemap"_h);
			if (!localSheen) localSheen = sheenEnvironment;
			if (localSheen)
			{
				driver->AddSamplerToShaderBindings(
					resources->m_lightsBindings,
					"g_localSheenEnvCubemap"_h, localSheen, 22u);
			}
			resources->m_lightsBindings->RecalculateCompatibility();
			resources->m_lightsTemplate = lightsTemplate;
			resources->m_sharedLightsStorage = sharedResources->m_lightsStorage;
			resources->m_sharedGlobalIlluminationStorage =
				sharedResources->m_globalIlluminationStorage;
			resources->m_lightsTemplateRevision = lightsTemplateRevision;
			resources->m_frameGraphSamplerHash = frameGraphSamplerHash;
			resources->m_shadowMatricesHash = InvalidContentHash;
			resources->m_shadowIndicesHash = InvalidContentHash;
			resources->m_shadowAtlasTilesHash = InvalidContentHash;
		}

		LocalReflectionParameters localParameters{};
		if (auto environment = owner->GetGraphNode("Environment"_h).DynamicCast<EnvironmentNode>())
			localParameters = environment->GetLocalReflectionParameters();
		commands->UpdateShaderBinding(transferCommandList,
			resources->m_lightsBindings->GetOrAddShaderBinding("localReflection"_h),
			&localParameters, sizeof(localParameters), 0u);

		const uint64_t shadowMatricesHash = HashSubmissionValues(snapshot.m_shadowMatrices);
		if (resources->m_shadowMatricesHash != shadowMatricesHash)
		{
			if (!snapshot.m_shadowMatrices.IsEmpty())
			{
				commands->UpdateShaderBinding(
					transferCommandList,
					resources->m_lightsBindings->GetOrAddShaderBinding("lightsMatrices"_h),
					snapshot.m_shadowMatrices.GetData(),
					snapshot.m_shadowMatrices.Num() * sizeof(glm::mat4),
					0u);
			}
			resources->m_shadowMatricesHash = shadowMatricesHash;
		}

		const uint64_t shadowIndicesHash = HashSubmissionValues(snapshot.m_shadowIndices);
		if (resources->m_shadowIndicesHash != shadowIndicesHash)
		{
			if (!snapshot.m_shadowIndices.IsEmpty())
			{
				commands->UpdateShaderBinding(
					transferCommandList,
					resources->m_lightsBindings->GetOrAddShaderBinding("shadowIndices"_h),
					snapshot.m_shadowIndices.GetData(),
					snapshot.m_shadowIndices.Num() * sizeof(uint32_t),
					0u);
			}
			resources->m_shadowIndicesHash = shadowIndicesHash;
		}

		const uint64_t shadowAtlasTilesHash = HashSubmissionValues(snapshot.m_shadowAtlasTiles);
		if (resources->m_shadowAtlasTilesHash != shadowAtlasTilesHash)
		{
			if (!snapshot.m_shadowAtlasTiles.IsEmpty())
			{
				commands->UpdateShaderBinding(
					transferCommandList,
					resources->m_lightsBindings->GetOrAddShaderBinding("shadowAtlasTiles"_h),
					snapshot.m_shadowAtlasTiles.GetData(),
					snapshot.m_shadowAtlasTiles.Num() * sizeof(uint32_t),
					0u);
			}
			resources->m_shadowAtlasTilesHash = shadowAtlasTilesHash;
		}

		snapshot.m_rhiLightsData = resources->m_lightsBindings;

		snapshot.m_boneMatrices = sharedResources->m_boneBindings;
	}
}

RHIGlobalIlluminationRenderStats RHIFrameGraph::GetGlobalIlluminationRenderStats() const
{
	m_globalIlluminationStatsLock.Lock();
	const auto result = m_globalIlluminationStats;
	m_globalIlluminationStatsLock.Unlock();
	return result;
}

void RHIFrameGraph::PublishGlobalIlluminationRenderStats(const RHIGlobalIlluminationRenderStats& stats)
{
	m_globalIlluminationStatsLock.Lock();
	m_globalIlluminationStats = stats;
	m_globalIlluminationStatsLock.Unlock();
}

void RHIFrameGraph::Clear()
{
	PublishGlobalIlluminationRenderStats({});
	ResetCurrentDepthPyramids();
	m_samplers.Clear();
	m_graph.Clear();
	m_values.Clear();
	m_renderTargets.Clear();
	m_surfaces.Clear();
	m_msaaSources.Clear();
	m_msaaSurfaces.Clear();
	m_numStaticMsaaSources = 0;
	m_boundSurfaces.Clear();
	m_boundNodes.Clear();
	m_externalRenderPasses.Clear();
	++m_surfaceRevision;
}

FrameGraphNodePtr RHIFrameGraph::GetGraphNode(StringHash tag)
{
	const size_t index = m_graph.FindIf([&](const auto& lhs) { return lhs->GetTag() == tag; });
	if (index != -1)
	{
		return m_graph[index];
	}

	return nullptr;
}

void RHIFrameGraph::SetSampler(StringHash name, RHI::RHITexturePtr sampler)
{
	m_samplers[name] = sampler;
}

void RHIFrameGraph::SetRenderTarget(StringHash name, RHI::RHIRenderTargetPtr sampler)
{
	m_renderTargets[name] = sampler;
}

void RHIFrameGraph::SetSurface(StringHash name, RHI::RHISurfacePtr surface)
{
	auto& current = m_surfaces[name];
	if (current != surface)
	{
		current = std::move(surface);
		++m_surfaceRevision;
	}
}

glm::ivec2 RHIFrameGraph::GetSceneRenderExtent()
{
	if (const auto debugDraw = GetGraphNode("DebugDraw"_h))
	{
		if (const auto colorAttachment = debugDraw->GetResolvedAttachment("color"_h, this))
		{
			const glm::ivec2 extent = colorAttachment->GetExtent();
			return glm::ivec2(
				(std::max)(extent.x, 1),
				(std::max)(extent.y, 1));
		}
	}

	if (const auto mainSurface = GetSurface("Main"_h))
	{
		const auto colorAttachment = mainSurface->GetResolved() ?
			mainSurface->GetResolved() : mainSurface->GetTarget();
		if (colorAttachment)
		{
			const glm::ivec2 extent = colorAttachment->GetExtent();
			return glm::ivec2(
				(std::max)(extent.x, 1),
				(std::max)(extent.y, 1));
		}
	}

	if (const auto mainTarget = GetRenderTarget("Main"_h))
	{
		const glm::ivec2 extent = mainTarget->GetExtent();
		return glm::ivec2(
			(std::max)(extent.x, 1),
			(std::max)(extent.y, 1));
	}

	const glm::ivec2 viewportExtent = App::GetMainWindow()->GetRenderArea();
	const Settings::GraphicsExtent fallbackExtent =
		Settings::ResolveRenderDimensions(
			static_cast<uint32_t>((std::max)(viewportExtent.x, 1)),
			static_cast<uint32_t>((std::max)(viewportExtent.y, 1)),
			App::GetActiveGraphicsSettings().m_resolutionFactor);
	return glm::ivec2(
		static_cast<int32_t>(fallbackExtent.m_width),
		static_cast<int32_t>(fallbackExtent.m_height));
}

void RHIFrameGraph::FillFrameData(RHI::RHICommandListPtr transferCmdList, RHI::RHISceneViewSnapshot& snapshot)
{
	SAILOR_PROFILE_FUNCTION();

	const auto frameData = snapshot.GetFrameData(GetSceneRenderExtent());

	if (!snapshot.m_frameBindings)
	{
		snapshot.m_frameBindings = Sailor::RHI::Renderer::GetDriver()->CreateShaderBindings();
		Sailor::RHI::Renderer::GetDriver()->AddBufferToShaderBindings(snapshot.m_frameBindings, "frameData"_h, sizeof(RHI::UboFrameData), 0, RHI::EShaderBindingType::UniformBuffer);
	}

	RHI::Renderer::GetDriverCommands()->UpdateShaderBinding(transferCmdList, snapshot.m_frameBindings->GetOrAddShaderBinding("frameData"_h), &frameData, sizeof(frameData));
	// Frame uploads and compute consumers share this command list.
	Renderer::GetDriverCommands()->MemoryBarrier(transferCmdList, static_cast<EAccessFlags>(EAccessBit::TransferWrite_Bit),
		static_cast<EAccessFlags>(EAccessBit::UniformRead_Bit) | static_cast<EAccessFlags>(EAccessBit::ShaderRead_Bit));
}

TVector<Sailor::Tasks::ITaskPtr> RHIFrameGraph::Prepare(RHI::RHISceneViewPtr rhiSceneView)
{
	TVector<Sailor::Tasks::ITaskPtr> res;
	// Recording belongs to this submission even when its DebugDraw pass is omitted.
	for (const auto& task : rhiSceneView->m_debugDraw)
	{
		res.Add(task);
	}

	auto frameRefPtr = this->ToRefPtr<RHIFrameGraph>();
	for (auto& snapshot : rhiSceneView->m_snapshots)
	{
		snapshot.PrepareLods(snapshot.m_camera->GetViewMatrix(), snapshot.m_camera->GetProjectionMatrix());
		for (auto& node : m_graph)
		{
			auto task = node->Prepare(frameRefPtr, snapshot);
			if (task.IsValid())
			{
				res.Emplace(std::move(task));
			}
		}
	}

	return res;
}

bool RHIFrameGraph::PrepareRenderTargets()
{
	SAILOR_PROFILE_FUNCTION();
	const auto samples = App::GetSubmodule<Renderer>()->GetMsaaSamples();
	bool bindingsChanged = m_boundMsaaSamples != samples || m_boundNodes.Num() != m_graph.Num();
	for (size_t i = 0; i < m_graph.Num() && !bindingsChanged; ++i)
	{
		bindingsChanged = m_boundNodes[i].m_first != m_graph[i] ||
			m_boundNodes[i].m_second != m_graph[i]->m_resourceRevision;
	}
	if (!bindingsChanged && m_externalRenderPasses.IsEmpty() && m_boundSurfaceRevision == m_surfaceRevision &&
		m_msaaSources.Num() == m_msaaSurfaces.Num()) return true;

	const auto collectTargets = [&](FrameGraphNodePtr node, TVector<RHIRenderTargetPtr>& sources)
	{
		const auto outputs = node->GetMsaaOutputs();
		auto primary = node->GetRHIResource(outputs.front(), this);
		const auto primarySurface = primary.DynamicCast<RHISurface>();
		if (!primary || (primarySurface && !primarySurface->NeedsResolve()))
		{
			return;
		}
		for (const auto name : outputs)
		{
			auto resource = node->GetRHIResource(name, this);
			auto surface = resource.DynamicCast<RHISurface>();
			auto target = surface ? surface->GetResolved() : resource.DynamicCast<RHIRenderTarget>();
			if (!target || target->GetMsaaSamples() != EMsaaSamples::Samples_1) continue;
			if (surface && surface->NeedsResolve()) m_msaaSurfaces[target.GetRawPtr()] = surface;
			else
			{
				const RHISurfacePtr* prepared = nullptr;
				if (m_msaaSurfaces.Find(target.GetRawPtr(), prepared) && !(*prepared)->NeedsResolve())
					m_msaaSurfaces.Remove(target.GetRawPtr());
			}
			if (!sources.Contains(target)) sources.Add(target);
		}
	};

	if (bindingsChanged)
	{
		m_msaaSources.Clear(false);
		m_boundSurfaces.Clear(false);
		m_boundNodes.Clear(false);
		m_externalRenderPasses.Clear(false);
		m_boundMsaaSamples = samples;
		for (auto& node : m_graph)
		{
			m_boundNodes.Emplace(node, node->m_resourceRevision);
			for (const auto& parameter : node->m_resourceParams)
			{
				auto resource = *parameter.m_second;
				if (auto surface = resource.DynamicCast<RHISurface>(); surface && !m_boundSurfaces.Contains(surface))
				{
					m_boundSurfaces.Add(surface);
				}
			}
			const auto outputs = node->GetMsaaOutputs();
			if (outputs.empty() || samples == EMsaaSamples::Samples_1)
			{
				continue;
			}
			const bool bHasExternalOutput = std::any_of(outputs.begin(), outputs.end(),
				[&](StringHash name) { return node->m_unresolvedResourceParams.ContainsKey(name); });
			if (bHasExternalOutput)
			{
				m_externalRenderPasses.Add(node);
			}
			else
			{
				collectTargets(node, m_msaaSources);
			}
		}
		m_numStaticMsaaSources = m_msaaSources.Num();
	}

	// Keep the static prefix; revisit only passes with external outputs.
	m_msaaSources.Resize(m_numStaticMsaaSources);
	for (auto& node : m_externalRenderPasses) collectTargets(node, m_msaaSources);
	m_boundSurfaceRevision = m_surfaceRevision;

	// A pass may bind a Surface while another binds its resolved texture.
	// Keep the prepared MSAA pair when a 1x Surface is used as a secondary MRT.
	const auto useSurface = [&](RHISurfacePtr surface)
	{
		if (surface)
		{
			if (!m_msaaSources.Contains(surface->GetResolved()))
			{
				m_msaaSources.Add(surface->GetResolved());
				m_msaaSurfaces[surface->GetResolved().GetRawPtr()] = surface;
			}
			else if (surface->NeedsResolve())
			{
				m_msaaSurfaces[surface->GetResolved().GetRawPtr()] = surface;
			}
		}
	};
	for (const auto& surface : m_surfaces) useSurface(*surface.m_second);
	for (auto& surface : m_boundSurfaces) useSurface(surface);

	if (m_msaaSources.IsEmpty())
	{
		if (!m_msaaSurfaces.IsEmpty()) m_msaaSurfaces.Clear();
		return true;
	}

	if (m_msaaSources.Num() == m_msaaSurfaces.Num() &&
		std::all_of(m_msaaSources.begin(), m_msaaSources.end(),
			[&](const auto& source) { return m_msaaSurfaces.ContainsKey(source.GetRawPtr()); }))
	{
		return true;
	}

	auto previous = std::move(m_msaaSurfaces);
	for (auto& source : m_msaaSources)
	{
		const RHISurfacePtr* surface = nullptr;
		auto prepared = previous.Find(source.GetRawPtr(), surface) ?
			*surface : Renderer::GetDriver()->CreateSurface(source);
		if (prepared) m_msaaSurfaces[source.GetRawPtr()] = std::move(prepared);
	}
	return m_msaaSources.Num() == m_msaaSurfaces.Num();
}

bool RHIFrameGraph::Process(RHI::RHISceneViewPtr rhiSceneView,
	TVector<RHI::RHICommandListPtr>& outTransferCommandLists,
	TVector<RHI::RHICommandListPtr>& outCommandLists,
	RHISemaphorePtr inSignalSemaphore,
	RHISemaphorePtr& outWaitSemaphore)
{
	SAILOR_PROFILE_FUNCTION();
	m_drawCallStats = {};
	RHIGlobalIlluminationRenderStats globalIlluminationRenderStats;
	outWaitSemaphore = inSignalSemaphore;
	if (!PrepareRenderTargets()) return false;

	auto renderer = App::GetSubmodule<RHI::Renderer>();
	auto& driver = RHI::Renderer::GetDriver();
	auto driverCommands = renderer->GetDriverCommands();
	RHISemaphorePtr frameGraphChainSemaphore = inSignalSemaphore;
	auto submissionProgress = TSharedPtr<RHISubmissionProgress>::Make();
	submissionProgress->SetLastSuccessfulSemaphore(inSignalSemaphore);

	if (!rhiSceneView->m_snapshots.IsEmpty() &&
		rhiSceneView->m_snapshots[0].m_submissionContext)
	{
		auto resourceUploadCommandList = renderer->GetDriver()->CreateCommandList(
			false,
			RHI::ECommandListQueue::Compute);
		driver->SetDebugName(resourceUploadCommandList, "FrameGraph:SharedResourceUpload"_h);
		driverCommands->BeginCommandList(resourceUploadCommandList, true);
		const auto& snapshot = rhiSceneView->m_snapshots[0];
		auto sharedResources = snapshot.m_submissionContext->GetOrAddFrameGraphResources<RHISharedViewSubmissionResources>(
			this, (std::numeric_limits<uint32_t>::max)(), 1u);
		UploadSharedLighting(resourceUploadCommandList, snapshot, *sharedResources);
		globalIlluminationRenderStats = UploadGlobalIllumination(resourceUploadCommandList, snapshot, *sharedResources);
		UploadSharedBones(resourceUploadCommandList, snapshot, *sharedResources);
		const bool bHasSharedResourceUploads =
			resourceUploadCommandList->GetNumRecordedCommands() > 0u;
		driverCommands->EndCommandList(resourceUploadCommandList);

		if (bHasSharedResourceUploads)
		{
			auto resourceReadySemaphore = driver->CreateWaitSemaphore();
			auto resourceUploadFence = RHIFencePtr::Make();
			driver->SetDebugName(resourceReadySemaphore, "FrameGraph:SharedResourceReady"_h);
			driver->SetDebugName(resourceUploadFence, "FrameGraph:SharedResourceUpload"_h);
			if (!driver->SubmitCommandList(
					resourceUploadCommandList,
					resourceUploadFence,
					resourceReadySemaphore,
					frameGraphChainSemaphore))
			{
				SAILOR_LOG_ERROR("RHIFrameGraph::Process: failed to submit shared resource upload command buffer.");
				return false;
			}

			frameGraphChainSemaphore = resourceReadySemaphore;
			submissionProgress->SetLastSuccessfulSemaphore(resourceReadySemaphore);
		}
	}

	if (!m_postEffectPlane)
	{
		auto plane = driver->CreateMesh();
		plane->m_vertexDescription = driver->GetOrAddVertexDescription<VertexP3N3UV2C4>();
		plane->m_bounds = Math::AABB(vec3(0), vec3(1, 1, 1));

		TVector<VertexP3N3UV2C4> ndcQuad(4);
		ndcQuad[0].m_texcoord = vec2(0.0f, 0.0f);
		ndcQuad[1].m_texcoord = vec2(1.0f, 0.0f);
		ndcQuad[2].m_texcoord = vec2(0.0f, 1.0f);
		ndcQuad[3].m_texcoord = vec2(1.0f, 1.0f);

		ndcQuad[0].m_position = vec3(-1.0f, -1.0f, 0.0f);
		ndcQuad[1].m_position = vec3(1.0f, -1.0f, 0.0f);
		ndcQuad[2].m_position = vec3(-1.0f, 1.0f, 0.0f);
		ndcQuad[3].m_position = vec3(1.0f, 1.0f, 0.0f);

		const TVector<uint32_t> indices = { 0, 1, 2, 2, 1, 3 };

		// Chain the quad upload before this frame's draws.
		auto upload = driver->CreateCommandList(false, ECommandListQueue::Graphics);
		driver->SetDebugName(upload, "FrameGraph:FullscreenQuadUpload"_h);
		driverCommands->BeginCommandList(upload, true);
		plane->m_vertexBuffer = driver->CreateBuffer(upload, ndcQuad.GetData(), ndcQuad.Num() * sizeof(VertexP3N3UV2C4),
			EBufferUsageBit::VertexBuffer_Bit, EMemoryPropertyBit::DeviceLocal);
		plane->m_indexBuffer = driver->CreateBuffer(upload, indices.GetData(), indices.Num() * sizeof(uint32_t),
			EBufferUsageBit::IndexBuffer_Bit, EMemoryPropertyBit::DeviceLocal);
		driverCommands->EndCommandList(upload);
		auto ready = driver->CreateWaitSemaphore();
		auto initialized = RHIFencePtr::Make();
		driver->TrackDelayedInitialization(plane.GetRawPtr(), initialized);
		if (!driver->SubmitCommandList(upload, initialized, ready, frameGraphChainSemaphore))
		{
			outWaitSemaphore = frameGraphChainSemaphore;
			return false;
		}
		m_postEffectPlane = std::move(plane);
		frameGraphChainSemaphore = ready;
		submissionProgress->SetLastSuccessfulSemaphore(ready);
	}

	for (auto& snapshot : rhiSceneView->m_snapshots)
	{
		SAILOR_PROFILE_SCOPE("Process snapshot");
		ResetCurrentDepthPyramids();

		auto cmdList = renderer->GetDriver()->CreateCommandList(false, RHI::ECommandListQueue::Graphics);
		auto transferCmdList = renderer->GetDriver()->CreateCommandList(false, RHI::ECommandListQueue::Compute);

		driver->SetDebugName(cmdList, "FrameGraph:Graphics"_h);
		driver->SetDebugName(transferCmdList, "FrameGraph:Transfer"_h);

		driverCommands->BeginCommandList(cmdList, true);
		uint32_t graphicsGpuFrameTimeRange =
			driver->BeginGpuFrameTimeRange(cmdList);
		driverCommands->BeginDebugRegion(cmdList, "FrameGraph:Graphics"_h, glm::vec4(0.75f, 1.0f, 0.75f, 0.1f));
		driverCommands->BeginCommandList(transferCmdList, true);
		uint32_t transferGpuFrameTimeRange =
			driver->BeginGpuFrameTimeRange(transferCmdList);
		driverCommands->BeginDebugRegion(transferCmdList, "FrameGraph:Transfer"_h, glm::vec4(0.75f, 0.75f, 1.0f, 0.1f));

		PrepareViewSubmissionResources(this, transferCmdList, snapshot);

		driverCommands->BeginDebugRegion(transferCmdList, "Fill Frame Data"_h, DebugContext::Color_CmdTransfer);
		{
			FillFrameData(transferCmdList, snapshot);
		}
		driverCommands->EndDebugRegion(transferCmdList);

		RHI::RHISemaphorePtr chainSemaphore = frameGraphChainSemaphore;
		auto submitsSucceeded = TSharedPtr<std::atomic_bool>::Make(true);

		TVector<Tasks::ITaskPtr> tasks;
		tasks.Reserve(2);

		auto frameRefPtr = this->ToRefPtr<RHIFrameGraph>();

		// Self balancing barriers
		{
			SAILOR_PROFILE_SCOPE("Change default image layout to decrease barriers count");

			driverCommands->BeginDebugRegion(cmdList, "FrameGraph:Decrease barriers count"_h, glm::vec4(1.0f, 0.75f, 0.75f, 0.1f));

			for (const auto& stat : m_lastFrameGpuStats.m_barriers)
			{
				RHITexturePtr texture = stat.m_first;

				// We don't pay attention to depth stencil targets 
				// since they must be in the DepthStencilAttachmentStencil in the end of frame
				if (RHI::IsDepthFormat(texture->GetFormat()))
					continue;

				EImageLayout bestLayout = stat.m_first->GetDefaultLayout();

				uint max = 1;
				if (auto cubemap = texture.DynamicCast<RHICubemap>())
				{
					max = 6 * std::max(1u, cubemap->GetMipLevels());
				}
				else if (auto renderTarget = texture.DynamicCast<RHIRenderTarget>())
				{
					max = std::max(1u, renderTarget->GetMipLevels());
				}

				for (const auto& layout : *stat.Second())
				{
					if (*layout.Second() > max)
					{
						bestLayout = layout.m_first;
						max = *layout.Second();
					}
				}

				if (texture->GetDefaultLayout() != bestLayout)
				{
					driverCommands->ImageMemoryBarrier(cmdList, texture, bestLayout);
					texture->ForceSetDefaultLayout(bestLayout);
				}
			}

			driverCommands->EndDebugRegion(cmdList);

			m_lastFrameGpuStats.m_barriers.Clear();
		}

		const bool bExecuteQueries = driver->StartGpuTracking();

		uint32_t nodeIndex = 0u;
		for (auto& node : m_graph)
		{
			uint32_t graphicsTimestamp = RHI::InvalidGpuTimestampQuery;
			uint32_t computeTimestamp = RHI::InvalidGpuTimestampQuery;
			if (bExecuteQueries)
			{
				const auto timingName = node->GetGpuTimingName(nodeIndex);

				graphicsTimestamp = driverCommands->BeginGpuTimestamp(
					cmdList,
					timingName);
				computeTimestamp = driverCommands->BeginGpuTimestamp(
					transferCmdList,
					timingName);
			}

			node->Process(frameRefPtr, transferCmdList, cmdList, snapshot);
			if (bExecuteQueries)
			{
				driverCommands->EndGpuTimestamp(
					transferCmdList,
					computeTimestamp);
				driverCommands->EndGpuTimestamp(
					cmdList,
					graphicsTimestamp);
			}
			m_drawCallStats += node->GetDrawCallStats();
			++nodeIndex;

			const uint32_t numRecordedCommands = transferCmdList->GetNumRecordedCommands() + cmdList->GetNumRecordedCommands();
			const uint32_t gpuCost = transferCmdList->GetGPUCost() + cmdList->GetGPUCost();
			if (gpuCost > MaxGpuCost || numRecordedCommands > MaxRecordedCommands)
			{
				SAILOR_PROFILE_SCOPE("Chaining command lists");

				driverCommands->EndDebugRegion(cmdList);
				driver->EndGpuFrameTimeRange(
					cmdList,
					graphicsGpuFrameTimeRange);
				driverCommands->EndCommandList(cmdList);

				driverCommands->EndDebugRegion(transferCmdList);
				driver->EndGpuFrameTimeRange(
					transferCmdList,
					transferGpuFrameTimeRange);
				driverCommands->EndCommandList(transferCmdList);

				// Create tasks
				{
					SAILOR_PROFILE_SCOPE("Create RHI submit cmd lists tasks");
					RHI::RHISemaphorePtr newChainSemaphore = driver->CreateWaitSemaphore();
					driver->SetDebugName(newChainSemaphore, "FrameGraph: newChainSemaphore"_h);

					tasks.RemoveAll([](const auto& task) { return task == nullptr || task->IsFinished(); });

					auto submitCmdList1 = Tasks::CreateTask("Submit chaining cmd lists"_h,
						[=]()
						{
							if (!submitsSucceeded->load(std::memory_order_acquire))
							{
								return;
							}

							auto fence = RHIFencePtr::Make();
							RHI::Renderer::GetDriver()->SetDebugName(fence, "Submit chaining cmd lists"_h);
							if (!RHI::Renderer::GetDriver()->SubmitCommandList(transferCmdList, fence, newChainSemaphore, chainSemaphore))
							{
								SAILOR_LOG_ERROR("RHIFrameGraph::Process: failed to submit a chained transfer command buffer.");
								submitsSucceeded->store(false, std::memory_order_release);
							}
							else
							{
								submissionProgress->SetLastSuccessfulSemaphore(newChainSemaphore);
							}
						}, EThreadType::RHI);

					if (tasks.Num() > 0)
					{
						submitCmdList1->Join(tasks[tasks.Num() - 1]);
					}

					submitCmdList1->Run();

					chainSemaphore = driver->CreateWaitSemaphore();
					driver->SetDebugName(chainSemaphore, "FrameGraph: chainSemaphore"_h);

					auto submitCmdList2 = Tasks::CreateTask("Submit chaining cmd lists"_h,
						[=]()
						{
							if (!submitsSucceeded->load(std::memory_order_acquire))
							{
								return;
							}

							auto fence = RHIFencePtr::Make();
							RHI::Renderer::GetDriver()->SetDebugName(fence, "Submit chaining cmd lists"_h);
							if (!RHI::Renderer::GetDriver()->SubmitCommandList(cmdList, fence, chainSemaphore, newChainSemaphore))
							{
								SAILOR_LOG_ERROR("RHIFrameGraph::Process: failed to submit a chained graphics command buffer.");
								submitsSucceeded->store(false, std::memory_order_release);
							}
							else
							{
								submissionProgress->SetLastSuccessfulSemaphore(chainSemaphore);
							}
						}, EThreadType::RHI);

					submitCmdList2->Join(submitCmdList1);
					submitCmdList2->Run();

					tasks.AddRange({ submitCmdList1, submitCmdList2 });
				}

				// New command lists
				{
					SAILOR_PROFILE_SCOPE("Create new command lists");

					cmdList = renderer->GetDriver()->CreateCommandList(false, RHI::ECommandListQueue::Graphics);
					transferCmdList = renderer->GetDriver()->CreateCommandList(false, RHI::ECommandListQueue::Compute);

					driver->SetDebugName(cmdList, "FrameGraph:Graphics"_h);
					driver->SetDebugName(transferCmdList, "FrameGraph:Transfer"_h);

					driverCommands->BeginCommandList(cmdList, true);
					graphicsGpuFrameTimeRange =
						driver->BeginGpuFrameTimeRange(cmdList);
					driverCommands->BeginDebugRegion(cmdList, "FrameGraph:Graphics"_h, glm::vec4(0.75f, 1.0f, 0.75f, 0.1f));

					driverCommands->BeginCommandList(transferCmdList, true);
					transferGpuFrameTimeRange =
						driver->BeginGpuFrameTimeRange(transferCmdList);
					driverCommands->BeginDebugRegion(transferCmdList, "FrameGraph:Transfer"_h, glm::vec4(0.75f, 0.75f, 1.0f, 0.1f));
				}
			}
			//TODO: Submit Transfer command lists
		}

		driverCommands->EndDebugRegion(cmdList);
		driver->EndGpuFrameTimeRange(
			cmdList,
			graphicsGpuFrameTimeRange);
		driverCommands->EndCommandList(cmdList);

		driverCommands->EndDebugRegion(transferCmdList);
		driver->EndGpuFrameTimeRange(
			transferCmdList,
			transferGpuFrameTimeRange);
		driverCommands->EndCommandList(transferCmdList);

		{
			SAILOR_PROFILE_SCOPE("Wait for submitting of chaining command lists");
			for (auto& task : tasks)
			{
				task->Wait();
			}
		}

		if (!submitsSucceeded->load(std::memory_order_acquire))
		{
			m_lastFrameGpuStats = driver->FinishGpuTracking();
			outWaitSemaphore = submissionProgress->GetLastSuccessfulSemaphore();
			return false;
		}

		m_lastFrameGpuStats = driver->FinishGpuTracking();

		frameGraphChainSemaphore = chainSemaphore;
		outCommandLists.Emplace(std::move(cmdList));
		outTransferCommandLists.Emplace(transferCmdList);
	}

	outWaitSemaphore = frameGraphChainSemaphore;
	PublishGlobalIlluminationRenderStats(globalIlluminationRenderStats);
	return true;
}

RHI::RHIResourcePtr RHIFrameGraph::GetResource(StringHash name) const
{
	const RHISurfacePtr* surface = nullptr;
	if (m_surfaces.Find(name, surface) && *surface) return *surface;
	if (const auto target = GetRenderTarget(name)) return ResolveResource(target);
	return ResolveResource(GetSampler(name));
}

RHI::RHIResourcePtr RHIFrameGraph::ResolveResource(RHI::RHIResourcePtr resource) const
{
	if (const auto target = resource.DynamicCast<RHIRenderTarget>())
	{
		const RHISurfacePtr* surface = nullptr;
		if (m_msaaSurfaces.Find(target.GetRawPtr(), surface)) return *surface;
	}
	return resource;
}

RHI::RHITexturePtr RHIFrameGraph::GetSampler(StringHash name) const
{
	if (!m_samplers.ContainsKey(name))
	{
		return RHITexturePtr();
	}

	return m_samplers[name];
}

RHI::RHIRenderTargetPtr RHIFrameGraph::GetRenderTarget(StringHash name) const
{
	if (!m_renderTargets.ContainsKey(name))
	{
		return nullptr;
	}

	return m_renderTargets[name];
}

RHI::RHISurfacePtr RHIFrameGraph::GetSurface(StringHash name) const
{
	const RHISurfacePtr* surface = nullptr;
	if (m_surfaces.Find(name, surface) && *surface) return *surface;
	const auto target = GetRenderTarget(name);
	return target ? ResolveResource(target).DynamicCast<RHISurface>() : RHISurfacePtr{};
}
