#include "RHIFrameGraph.h"
#include "RHI/SceneView.h"
#include "RHI/Renderer.h"
#include "RHI/GraphicsDriver.h"
#include "RHI/Shader.h"
#include "RHI/VertexDescription.h"
#include "RHI/RenderTarget.h"
#include "RHI/Cubemap.h"
#include "RHI/CommandList.h"
#include "FrameGraph/LightCullingNode.h"
#include "AssetRegistry/Texture/TextureImporter.h"
#include "Tasks/Tasks.h"

#include <atomic>
#include <limits>

using namespace Sailor;
using namespace Sailor::RHI;

namespace
{
	constexpr uint64_t InvalidContentHash = (std::numeric_limits<uint64_t>::max)();

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

	class RHIViewSubmissionResources final : public RHIFrameGraphSubmissionResource
	{
	public:
		void ResetForSubmission() override {}
		void InvalidateSubmission() override
		{
			m_shadowMatricesHash = InvalidContentHash;
			m_shadowIndicesHash = InvalidContentHash;
			m_shadowAtlasTilesHash = InvalidContentHash;
		}

		RHIShaderBindingSetPtr m_lightsBindings{};
		RHIShaderBindingSetPtr m_lightsTemplate{};
		RHIShaderBindingSetPtr m_sharedLightsStorage{};
		RHIShaderBindingSetPtr m_frameBindings{};
		RHIShaderBindingSetPtr m_lightCullingBindings{};
		RHITexturePtr m_lightCullingDepth{};
		glm::ivec2 m_lightCullingViewportSize{};
		size_t m_shadowMatrixCapacity = 0u;
		size_t m_shadowIndexCapacity = 0u;
		size_t m_shadowAtlasTileCapacity = 0u;
		uint64_t m_lightsTemplateRevision = 0ull;
		size_t m_frameGraphSamplerHash = 0u;
		uint64_t m_shadowMatricesHash = InvalidContentHash;
		uint64_t m_shadowIndicesHash = InvalidContentHash;
		uint64_t m_shadowAtlasTilesHash = InvalidContentHash;
	};

	class RHISharedViewSubmissionResources final : public RHIFrameGraphSubmissionResource
	{
	public:
		void ResetForSubmission() override {}
		void InvalidateSubmission() override
		{
			m_uploadedLightingRevision = InvalidContentHash;
			m_uploadedAnimationRevision = InvalidContentHash;
			m_lightsSource.Clear();
			m_bonesSource.Clear();
		}

		RHIShaderBindingSetPtr m_lightsStorage{};
		RHIShaderBindingSetPtr m_boneBindings{};
		TSharedPtr<TVector<RHILightShaderData>> m_lightsSource{};
		TSharedPtr<TVector<glm::mat4>> m_bonesSource{};
		size_t m_lightCapacity = 0u;
		size_t m_boneCapacity = 0u;
		uint64_t m_uploadedLightingRevision = InvalidContentHash;
		uint64_t m_uploadedAnimationRevision = InvalidContentHash;
	};

	size_t GrowSubmissionCapacity(size_t currentCapacity, size_t requiredCapacity)
	{
		size_t result = (std::max)(size_t{ 1u }, currentCapacity);
		while (result < requiredCapacity)
		{
			const size_t next = result * 2u;
			if (next <= result)
			{
				return requiredCapacity;
			}
			result = next;
		}
		return result;
	}

	template<typename T>
	uint64_t HashSubmissionValues(const TVector<T>& values)
	{
		uint64_t result = 1469598103934665603ull;
		const auto* bytes = reinterpret_cast<const uint8_t*>(values.GetData());
		const size_t numBytes = values.Num() * sizeof(T);
		for (size_t index = 0u; index < numBytes; ++index)
		{
			result ^= bytes[index];
			result *= 1099511628211ull;
		}
		result ^= values.Num();
		result *= 1099511628211ull;
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
		RHISceneViewSnapshot& snapshot,
		bool bUploadSharedPayload)
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
				"frameData",
				sizeof(UboFrameData),
				0u,
				EShaderBindingType::UniformBuffer);
			driver->AddBufferToShaderBindings(
				resources->m_frameBindings,
				"previousFrameData",
				sizeof(UboFrameData),
				1u,
				EShaderBindingType::UniformBuffer);
		}
		snapshot.m_frameBindings = resources->m_frameBindings;

		auto linearDepthAttachment = owner->GetRenderTarget("LinearDepth");
		const glm::ivec2 lightCullingViewportSize = linearDepthAttachment ?
			linearDepthAttachment->GetExtent() : glm::ivec2{};
		const bool bRecreateLightCulling = linearDepthAttachment &&
			(!resources->m_lightCullingBindings ||
				resources->m_lightCullingDepth != linearDepthAttachment ||
				resources->m_lightCullingViewportSize != lightCullingViewportSize);
		if (bRecreateLightCulling)
		{
			const uint32_t numTilesX =
				(lightCullingViewportSize.x - 1) / LightCullingNode::TileSize + 1;
			const uint32_t numTilesY =
				(lightCullingViewportSize.y - 1) / LightCullingNode::TileSize + 1;
			const size_t numTiles = static_cast<size_t>(numTilesX) * numTilesY;
			resources->m_lightCullingBindings = driver->CreateShaderBindings();
			driver->AddSsboToShaderBindings(
				resources->m_lightCullingBindings,
				"culledLights",
				sizeof(uint32_t) * numTiles * LightCullingNode::LightsPerTile,
				1u,
				0u,
				true);
			driver->AddSsboToShaderBindings(
				resources->m_lightCullingBindings,
				"lightsGrid",
				sizeof(uint32_t) * numTiles * 2u,
				1u,
				1u,
				true);
			driver->AddSamplerToShaderBindings(
				resources->m_lightCullingBindings,
				"linearDepth",
				linearDepthAttachment,
				2u);
			resources->m_lightCullingBindings->RecalculateCompatibility();
			resources->m_lightCullingDepth = linearDepthAttachment;
			resources->m_lightCullingViewportSize = lightCullingViewportSize;
		}
		snapshot.m_rhiLightCullingData = resources->m_lightCullingBindings;

		auto lightsTemplate = snapshot.m_rhiLightsData;
		if (lightsTemplate == resources->m_lightsBindings && resources->m_lightsTemplate)
		{
			lightsTemplate = resources->m_lightsTemplate;
		}
		const uint64_t lightsTemplateRevision = lightsTemplate ?
			lightsTemplate->GetDescriptorRevision() : 0ull;
		const size_t numLights = snapshot.m_cpuLightsData ? snapshot.m_cpuLightsData->Num() : 0u;
		const size_t numShadowMatrices = snapshot.m_shadowMatrices.Num();
		const size_t numShadowIndices = snapshot.m_shadowIndices.Num();
		const size_t numShadowAtlasTiles = snapshot.m_shadowAtlasTiles.Num();
		const bool bRecreateLightStorage = !sharedResources->m_lightsStorage ||
			sharedResources->m_lightCapacity < numLights;
		if (bRecreateLightStorage)
		{
			sharedResources->m_lightCapacity = GrowSubmissionCapacity(
				sharedResources->m_lightCapacity,
				numLights);
			sharedResources->m_lightsStorage = driver->CreateShaderBindings();
			driver->AddSsboToShaderBindings(
				sharedResources->m_lightsStorage,
				"light",
				sizeof(RHILightShaderData),
				sharedResources->m_lightCapacity,
				0u,
				true);
			sharedResources->m_lightsStorage->RecalculateCompatibility();
			sharedResources->m_uploadedLightingRevision = InvalidContentHash;
			sharedResources->m_lightsSource.Clear();
		}
		if (bUploadSharedPayload && snapshot.m_cpuLightsData &&
			(sharedResources->m_lightsSource != snapshot.m_cpuLightsData ||
				sharedResources->m_uploadedLightingRevision != snapshot.m_lightingRevision))
		{
			if (!snapshot.m_cpuLightsData->IsEmpty())
			{
				commands->UpdateShaderBinding(
					transferCommandList,
					sharedResources->m_lightsStorage->GetOrAddShaderBinding("light"),
					snapshot.m_cpuLightsData->GetData(),
					snapshot.m_cpuLightsData->Num() * sizeof(RHILightShaderData),
					0u);
			}
			sharedResources->m_lightsSource = snapshot.m_cpuLightsData;
			sharedResources->m_uploadedLightingRevision = snapshot.m_lightingRevision;
		}

		size_t frameGraphSamplerHash = 0u;
		HashCombine(frameGraphSamplerHash, Sailor::GetHash(owner->GetSampler("g_irradianceCubemap")));
		HashCombine(frameGraphSamplerHash, Sailor::GetHash(owner->GetSampler("g_brdfSampler")));
		HashCombine(frameGraphSamplerHash, Sailor::GetHash(owner->GetSampler("g_envCubemap")));
		HashCombine(frameGraphSamplerHash, Sailor::GetHash(owner->GetRenderTarget("g_AO")));
		const bool bRecreateLights = !resources->m_lightsBindings ||
			bRecreateLightCulling ||
			resources->m_lightsTemplate != lightsTemplate ||
			resources->m_sharedLightsStorage != sharedResources->m_lightsStorage ||
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
			driver->AddShaderBinding(
				resources->m_lightsBindings,
				sharedResources->m_lightsStorage->GetOrAddShaderBinding("light"),
				"light",
				0u);
			if (resources->m_lightCullingBindings)
			{
				driver->AddShaderBinding(
					resources->m_lightsBindings,
					resources->m_lightCullingBindings->GetOrAddShaderBinding("culledLights"),
					"culledLights",
					1u);
				driver->AddShaderBinding(
					resources->m_lightsBindings,
					resources->m_lightCullingBindings->GetOrAddShaderBinding("lightsGrid"),
					"lightsGrid",
					2u);
			}
			driver->AddSsboToShaderBindings(
				resources->m_lightsBindings,
				"lightsMatrices",
				sizeof(glm::mat4),
				resources->m_shadowMatrixCapacity,
				6u,
				true);
			driver->AddSsboToShaderBindings(
				resources->m_lightsBindings,
				"shadowIndices",
				sizeof(uint32_t),
				resources->m_shadowIndexCapacity,
				7u,
				true);
			driver->AddSsboToShaderBindings(
				resources->m_lightsBindings,
				"shadowAtlasTiles",
				sizeof(uint32_t),
				resources->m_shadowAtlasTileCapacity,
				11u,
				true);
			CloneTextureBindings(lightsTemplate, resources->m_lightsBindings);
			if (auto texture = owner->GetSampler("g_irradianceCubemap"))
			{
				driver->AddSamplerToShaderBindings(
					resources->m_lightsBindings,
					"g_irradianceCubemap",
					texture,
					3u);
			}
			if (auto texture = owner->GetSampler("g_brdfSampler"))
			{
				driver->AddSamplerToShaderBindings(
					resources->m_lightsBindings,
					"g_brdfSampler",
					texture,
					4u);
			}
			if (auto texture = owner->GetSampler("g_envCubemap"))
			{
				driver->AddSamplerToShaderBindings(
					resources->m_lightsBindings,
					"g_envCubemap",
					texture,
					5u);
			}
			if (auto texture = owner->GetRenderTarget("g_AO"))
			{
				driver->AddSamplerToShaderBindings(
					resources->m_lightsBindings,
					"g_aoSampler",
					texture,
					8u);
			}
			if (auto texture = driver->GetDefaultTexture())
			{
				driver->AddSamplerToShaderBindings(
					resources->m_lightsBindings,
					"g_transmissionFramebufferSampler",
					texture,
					10u);
			}
			resources->m_lightsBindings->RecalculateCompatibility();
			resources->m_lightsTemplate = lightsTemplate;
			resources->m_sharedLightsStorage = sharedResources->m_lightsStorage;
			resources->m_lightsTemplateRevision = lightsTemplateRevision;
			resources->m_frameGraphSamplerHash = frameGraphSamplerHash;
			resources->m_shadowMatricesHash = InvalidContentHash;
			resources->m_shadowIndicesHash = InvalidContentHash;
			resources->m_shadowAtlasTilesHash = InvalidContentHash;
		}

		const uint64_t shadowMatricesHash = HashSubmissionValues(snapshot.m_shadowMatrices);
		if (resources->m_shadowMatricesHash != shadowMatricesHash)
		{
			if (!snapshot.m_shadowMatrices.IsEmpty())
			{
				commands->UpdateShaderBinding(
					transferCommandList,
					resources->m_lightsBindings->GetOrAddShaderBinding("lightsMatrices"),
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
					resources->m_lightsBindings->GetOrAddShaderBinding("shadowIndices"),
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
					resources->m_lightsBindings->GetOrAddShaderBinding("shadowAtlasTiles"),
					snapshot.m_shadowAtlasTiles.GetData(),
					snapshot.m_shadowAtlasTiles.Num() * sizeof(uint32_t),
					0u);
			}
			resources->m_shadowAtlasTilesHash = shadowAtlasTilesHash;
		}

		snapshot.m_rhiLightsData = resources->m_lightsBindings;

		const size_t numBoneMatrices = snapshot.m_cpuBoneMatrices ? snapshot.m_cpuBoneMatrices->Num() : 0u;
		if (numBoneMatrices == 0u)
		{
			snapshot.m_boneMatrices.Clear();
			return;
		}

		const bool bRecreateBones = !sharedResources->m_boneBindings ||
			sharedResources->m_boneCapacity < numBoneMatrices;
		if (bRecreateBones)
		{
			sharedResources->m_boneCapacity = GrowSubmissionCapacity(
				sharedResources->m_boneCapacity,
				numBoneMatrices);
			sharedResources->m_boneBindings = driver->CreateShaderBindings();
			driver->AddSsboToShaderBindings(
				sharedResources->m_boneBindings,
				"bones",
				sizeof(glm::mat4),
				sharedResources->m_boneCapacity,
				0u,
				true);
			sharedResources->m_boneBindings->RecalculateCompatibility();
			sharedResources->m_uploadedAnimationRevision = InvalidContentHash;
			sharedResources->m_bonesSource.Clear();
		}

		if (bUploadSharedPayload &&
			(sharedResources->m_bonesSource != snapshot.m_cpuBoneMatrices ||
				sharedResources->m_uploadedAnimationRevision != snapshot.m_animationRevision))
		{
			commands->UpdateShaderBinding(
				transferCommandList,
				sharedResources->m_boneBindings->GetOrAddShaderBinding("bones"),
				snapshot.m_cpuBoneMatrices->GetData(),
				numBoneMatrices * sizeof(glm::mat4),
				0u);
			sharedResources->m_bonesSource = snapshot.m_cpuBoneMatrices;
			sharedResources->m_uploadedAnimationRevision = snapshot.m_animationRevision;
		}
		snapshot.m_boneMatrices = sharedResources->m_boneBindings;
	}
}

void RHIFrameGraph::Clear()
{
	m_samplers.Clear();
	m_graph.Clear();
	m_values.Clear();
	m_renderTargets.Clear();
	m_surfaces.Clear();
}

FrameGraphNodePtr RHIFrameGraph::GetGraphNode(const std::string& tag)
{
	const size_t index = m_graph.FindIf([&](const auto& lhs) { return lhs->GetTag() == tag; });
	if (index != -1)
	{
		return m_graph[index];
	}

	return nullptr;
}

void RHIFrameGraph::SetSampler(const std::string& name, RHI::RHITexturePtr sampler)
{
	m_samplers[name] = sampler;
}

void RHIFrameGraph::SetRenderTarget(const std::string& name, RHI::RHIRenderTargetPtr sampler)
{
	m_renderTargets[name] = sampler;
}

void RHIFrameGraph::SetSurface(const std::string& name, RHI::RHISurfacePtr surface)
{
	m_surfaces[name] = surface;
}

RHI::UboFrameData RHIFrameGraph::FillFrameData(RHI::RHICommandListPtr transferCmdList, RHI::RHISceneViewSnapshot& snapshot, const RHI::UboFrameData& previousFrame, float deltaTime, float worldTime) const
{
	SAILOR_PROFILE_FUNCTION();

	RHI::UboFrameData frameData;

	if (!snapshot.m_frameBindings)
	{
		snapshot.m_frameBindings = Sailor::RHI::Renderer::GetDriver()->CreateShaderBindings();
		Sailor::RHI::Renderer::GetDriver()->AddBufferToShaderBindings(snapshot.m_frameBindings, "frameData", sizeof(RHI::UboFrameData), 0, RHI::EShaderBindingType::UniformBuffer);
		Sailor::RHI::Renderer::GetDriver()->AddBufferToShaderBindings(snapshot.m_frameBindings, "previousFrameData", sizeof(RHI::UboFrameData), 1, RHI::EShaderBindingType::UniformBuffer);
	}

	frameData.m_cameraPosition = snapshot.m_cameraTransform.m_position;
	frameData.m_projection = snapshot.m_camera->GetProjectionMatrix();
	frameData.m_invProjection = snapshot.m_camera->GetInvProjection();
	frameData.m_cameraZNearZFar = vec2(snapshot.m_camera->GetZNear(), snapshot.m_camera->GetZFar());
	frameData.m_currentTime = worldTime;
	frameData.m_deltaTime = deltaTime;
	frameData.m_view = snapshot.m_camera->GetViewMatrix();
	frameData.m_viewportSize = glm::ivec2(App::GetMainWindow()->GetRenderArea().x, App::GetMainWindow()->GetRenderArea().y);

	RHI::Renderer::GetDriverCommands()->UpdateShaderBinding(transferCmdList, snapshot.m_frameBindings->GetOrAddShaderBinding("frameData"), &frameData, sizeof(frameData));
	RHI::Renderer::GetDriverCommands()->UpdateShaderBinding(transferCmdList, snapshot.m_frameBindings->GetOrAddShaderBinding("previousFrameData"), &previousFrame, sizeof(previousFrame));

	return frameData;
}

TVector<Sailor::Tasks::TaskPtr<void, void>> RHIFrameGraph::Prepare(RHI::RHISceneViewPtr rhiSceneView)
{
	TVector<Sailor::Tasks::TaskPtr<void, void>> res;

	auto frameRefPtr = this->ToRefPtr<RHIFrameGraph>();
	for (auto& snapshot : rhiSceneView->m_snapshots)
	{
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

bool RHIFrameGraph::Process(RHI::RHISceneViewPtr rhiSceneView,
	TVector<RHI::RHICommandListPtr>& outTransferCommandLists,
	TVector<RHI::RHICommandListPtr>& outCommandLists,
	RHISemaphorePtr inSignalSemaphore,
	RHISemaphorePtr& outWaitSemaphore)
{
	SAILOR_PROFILE_FUNCTION();
	m_drawCallStats = {};

	auto renderer = App::GetSubmodule<RHI::Renderer>();
	auto& driver = RHI::Renderer::GetDriver();
	auto driverCommands = renderer->GetDriverCommands();
	RHISemaphorePtr frameGraphChainSemaphore = inSignalSemaphore;
	auto submissionProgress = TSharedPtr<RHISubmissionProgress>::Make();
	submissionProgress->SetLastSuccessfulSemaphore(inSignalSemaphore);
	outWaitSemaphore = inSignalSemaphore;

	if (!rhiSceneView->m_snapshots.IsEmpty() &&
		rhiSceneView->m_snapshots[0].m_submissionContext)
	{
		auto resourceUploadCommandList = renderer->GetDriver()->CreateCommandList(
			false,
			RHI::ECommandListQueue::Compute);
		driver->SetDebugName(resourceUploadCommandList, "FrameGraph:SharedResourceUpload");
		driverCommands->BeginCommandList(resourceUploadCommandList, true);
		PrepareViewSubmissionResources(
			this,
			resourceUploadCommandList,
			rhiSceneView->m_snapshots[0],
			true);
		const bool bHasSharedResourceUploads =
			resourceUploadCommandList->GetNumRecordedCommands() > 0u;
		driverCommands->EndCommandList(resourceUploadCommandList);

		if (bHasSharedResourceUploads)
		{
			auto resourceReadySemaphore = driver->CreateWaitSemaphore();
			auto resourceUploadFence = RHIFencePtr::Make();
			driver->SetDebugName(resourceReadySemaphore, "FrameGraph:SharedResourceReady");
			driver->SetDebugName(resourceUploadFence, "FrameGraph:SharedResourceUpload");
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
		m_postEffectPlane = renderer->GetDriver()->CreateMesh();
		m_postEffectPlane->m_vertexDescription = RHI::Renderer::GetDriver()->GetOrAddVertexDescription<RHI::VertexP3N3UV2C4>();
		m_postEffectPlane->m_bounds = Math::AABB(vec3(0), vec3(1, 1, 1));

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

		RHI::Renderer::GetDriver()->UpdateMesh(m_postEffectPlane, &ndcQuad[0], ndcQuad.Num() * sizeof(VertexP3N3UV2C4), &indices[0], sizeof(uint32_t) * indices.Num());
	}

	for (auto& snapshot : rhiSceneView->m_snapshots)
	{
		SAILOR_PROFILE_SCOPE("Process snapshot");

		auto cmdList = renderer->GetDriver()->CreateCommandList(false, RHI::ECommandListQueue::Graphics);
		auto transferCmdList = renderer->GetDriver()->CreateCommandList(false, RHI::ECommandListQueue::Compute);

		driver->SetDebugName(cmdList, "FrameGraph:Graphics");
		driver->SetDebugName(transferCmdList, "FrameGraph:Transfer");

		driverCommands->BeginCommandList(cmdList, true);
		driverCommands->BeginDebugRegion(cmdList, "FrameGraph:Graphics", glm::vec4(0.75f, 1.0f, 0.75f, 0.1f));

		driverCommands->BeginCommandList(transferCmdList, true);
		driverCommands->BeginDebugRegion(transferCmdList, "FrameGraph:Transfer", glm::vec4(0.75f, 0.75f, 1.0f, 0.1f));

		PrepareViewSubmissionResources(this, transferCmdList, snapshot, false);

		driverCommands->BeginDebugRegion(transferCmdList, "Fill Frame Data", DebugContext::Color_CmdTransfer);
		{
			m_prevFrameData = FillFrameData(transferCmdList, snapshot, m_prevFrameData, rhiSceneView->m_deltaTime, rhiSceneView->m_currentTime);
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

			driverCommands->BeginDebugRegion(cmdList, "FrameGraph:Decrease barriers count", glm::vec4(1.0f, 0.75f, 0.75f, 0.1f));

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

		driver->StartGpuTracking();

		for (auto& node : m_graph)
		{
			node->Process(frameRefPtr, transferCmdList, cmdList, snapshot);
			m_drawCallStats += node->GetDrawCallStats();

			const uint32_t numRecordedCommands = transferCmdList->GetNumRecordedCommands() + cmdList->GetNumRecordedCommands();
			const uint32_t gpuCost = transferCmdList->GetGPUCost() + cmdList->GetGPUCost();
			if (gpuCost > MaxGpuCost || numRecordedCommands > MaxRecordedCommands)
			{
				SAILOR_PROFILE_SCOPE("Chaining command lists");

				driverCommands->EndDebugRegion(cmdList);
				driverCommands->EndCommandList(cmdList);

				driverCommands->EndDebugRegion(transferCmdList);
				driverCommands->EndCommandList(transferCmdList);

				// Create tasks
				{
					SAILOR_PROFILE_SCOPE("Create RHI submit cmd lists tasks");
					RHI::RHISemaphorePtr newChainSemaphore = driver->CreateWaitSemaphore();
					driver->SetDebugName(newChainSemaphore, "FrameGraph: newChainSemaphore");

					tasks.RemoveAll([](const auto& task) { return task == nullptr || task->IsFinished(); });

					auto submitCmdList1 = Tasks::CreateTask("Submit chaining cmd lists",
						[=]()
						{
							if (!submitsSucceeded->load(std::memory_order_acquire))
							{
								return;
							}

							auto fence = RHIFencePtr::Make();
							RHI::Renderer::GetDriver()->SetDebugName(fence, std::format("Submit chaining cmd lists"));
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
					driver->SetDebugName(chainSemaphore, "FrameGraph: chainSemaphore");

					auto submitCmdList2 = Tasks::CreateTask("Submit chaining cmd lists",
						[=]()
						{
							if (!submitsSucceeded->load(std::memory_order_acquire))
							{
								return;
							}

							auto fence = RHIFencePtr::Make();
							RHI::Renderer::GetDriver()->SetDebugName(fence, std::format("Submit chaining cmd lists"));
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

					driver->SetDebugName(cmdList, "FrameGraph:Graphics");
					driver->SetDebugName(transferCmdList, "FrameGraph:Transfer");

					driverCommands->BeginCommandList(cmdList, true);
					driverCommands->BeginDebugRegion(cmdList, "FrameGraph:Graphics", glm::vec4(0.75f, 1.0f, 0.75f, 0.1f));

					driverCommands->BeginCommandList(transferCmdList, true);
					driverCommands->BeginDebugRegion(transferCmdList, "FrameGraph:Transfer", glm::vec4(0.75f, 0.75f, 1.0f, 0.1f));
				}
			}
			//TODO: Submit Transfer command lists
		}

		driverCommands->EndDebugRegion(cmdList);
		driverCommands->EndCommandList(cmdList);

		driverCommands->EndDebugRegion(transferCmdList);
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
	return true;
}

RHI::RHITexturePtr RHIFrameGraph::GetSampler(const std::string& name)
{
	if (!m_samplers.ContainsKey(name))
	{
		return RHITexturePtr();
	}

	return m_samplers[name];
}

RHI::RHIRenderTargetPtr RHIFrameGraph::GetRenderTarget(const std::string& name)
{
	if (!m_renderTargets.ContainsKey(name))
	{
		return nullptr;
	}

	return m_renderTargets[name];
}

RHI::RHISurfacePtr RHIFrameGraph::GetSurface(const std::string& name)
{
	if (!m_surfaces.ContainsKey(name))
	{
		return nullptr;
	}

	return m_surfaces[name];
}
