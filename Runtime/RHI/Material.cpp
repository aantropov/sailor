#include "Material.h"
#include "Types.h"
#include "Shader.h"
#include "RHI/Texture.h"
#include "GraphicsDriver/Vulkan/VulkanApi.h"
#include "GraphicsDriver/Vulkan/VulkanPipeline.h"

#include <limits>

using namespace Sailor;
using namespace Sailor::RHI;
using namespace Sailor::GraphicsDriver::Vulkan;

namespace
{
	std::atomic<uint64_t> g_rhiMaterialVersion{ 1ull };
	std::atomic<uint64_t> g_publishedRhiMaterialRevision{ 1ull };
	SpinLock g_rhiMaterialPublicationLock;
	struct ActiveMaterialSubmissionCapture
	{
		uint64_t m_submissionId = 0ull;
		uint64_t m_revision = 0ull;
	};
	// CPU packet construction is serialized today, while the renderer supports
	// up to three flights. Keep one spare slot without allocating per frame.
	std::array<ActiveMaterialSubmissionCapture, 4u>
		g_activeMaterialSubmissionCaptures{};
	TVector<RHIMaterialPtr> g_materialsWithRetainedPublicationHistory;

	uint64_t GetOldestActiveMaterialRevisionLocked()
	{
		uint64_t result = (std::numeric_limits<uint64_t>::max)();
		for (const auto& capture : g_activeMaterialSubmissionCaptures)
		{
			if (capture.m_submissionId != 0ull)
			{
				result = (std::min)(result, capture.m_revision);
			}
		}
		return result;
	}

	bool FindActiveMaterialRevisionLocked(
		uint64_t submissionId,
		uint64_t& outRevision)
	{
		for (const auto& capture : g_activeMaterialSubmissionCaptures)
		{
			if (capture.m_submissionId == submissionId)
			{
				outRevision = capture.m_revision;
				return true;
			}
		}
		return false;
	}
}

GraphicsDriver::Vulkan::VulkanGraphicsPipelinePtr RHIMaterial::Vulkan::GetOrAddPipeline(const TVector<VkFormat>& colorAttachments, VkFormat depthStencilAttachment)
{
	SAILOR_PROFILE_FUNCTION();
	m_pipelinesLock.Lock();

	uint32_t stateIndex = 0;
	const VkFormat stencilAttachmentFormat = (VulkanApi::ComputeAspectFlagsForFormat(depthStencilAttachment) & VK_IMAGE_ASPECT_STENCIL_BIT) ? depthStencilAttachment : VK_FORMAT_UNDEFINED;

	for (auto& p : m_pipelines)
	{
		stateIndex = 0;
		for (auto& state : p->m_pipelineStates)
		{
			if (const auto pDynamicState = state.DynamicCast<GraphicsDriver::Vulkan::VulkanStateDynamicRendering>())
			{
				if (pDynamicState->Fits(colorAttachments, depthStencilAttachment, stencilAttachmentFormat))
				{
					auto result = p;
					m_pipelinesLock.Unlock();
					return result;
				}

				break;
			}

			stateIndex++;
		}
	}

	auto device = VulkanApi::GetInstance()->GetMainDevice();

	TVector<VulkanPipelineStatePtr> states = m_pipelines[0]->m_pipelineStates;

	states[stateIndex] = GraphicsDriver::Vulkan::VulkanStateDynamicRenderingPtr::Make(colorAttachments, depthStencilAttachment, stencilAttachmentFormat);

	GraphicsDriver::Vulkan::VulkanGraphicsPipelinePtr pipeline = VulkanGraphicsPipelinePtr::Make(device,
		m_pipelines[0]->m_layout,
		m_pipelines[0]->m_stages,
		states,
		0);

	pipeline->m_renderPass = device->GetRenderPass();
	if (!pipeline->Compile())
	{
		m_pipelinesLock.Unlock();
		return nullptr;
	}

	m_pipelines.Emplace(std::move(pipeline));

	auto result = *m_pipelines.Last();
	m_pipelinesLock.Unlock();
	return result;
}

void RHIShaderBindingSet::RecalculateCompatibility()
{
	SAILOR_PROFILE_FUNCTION();

	m_compatibilityHashCode = 0;

	for (auto& shaderBinding : m_shaderBindings)
	{
		HashCombine(m_compatibilityHashCode, shaderBinding.Second()->GetCompatibilityHash());
	}
}

RHI::RHIShaderBindingPtr& RHIShaderBindingSet::GetOrAddShaderBinding(const std::string& binding)
{
	auto& pBinding = m_shaderBindings.At_Lock(binding);
	if (!pBinding)
	{
		pBinding = RHIShaderBindingPtr::Make();
	}
	m_shaderBindings.Unlock(binding);

	return pBinding;
}

void RHIShaderBindingSet::RemoveShaderBinding(const std::string& binding)
{
	m_shaderBindings.Remove(binding);
}

void RHIShaderBindingSet::UpdateLayoutShaderBinding(const ShaderLayoutBinding& layout)
{
	// We are able to rewrite m_binding and m_set
	const size_t index = m_layoutBindings.FindIf([&](const auto& lhs)
		{
			return lhs.m_name == layout.m_name && lhs.m_type == layout.m_type && lhs.m_arrayCount == layout.m_arrayCount;
		});

	if (index != -1)
	{
		m_layoutBindings[index] = layout;
		return;
	}

	m_layoutBindings.Add(layout);
}

bool RHIShaderBindingSet::PerInstanceDataStoredInSsbo() const
{
	return std::find_if(m_layoutBindings.begin(), m_layoutBindings.end(), [](const auto& binding) { return binding.m_type == EShaderBindingType::StorageBuffer; }) != m_layoutBindings.end();
}

uint32_t RHIShaderBindingSet::GetStorageInstanceIndex(const std::string& bindingName) const
{
	if (m_shaderBindings.ContainsKey(bindingName))
	{
		return m_shaderBindings[bindingName]->GetStorageInstanceIndex();
	}

	return 0;
}

void RHIShaderBindingSet::SetLayoutShaderBindings(TVector<RHI::ShaderLayoutBinding> layoutBindings)
{
	m_layoutBindings = std::move(layoutBindings);
	m_bNeedsStorageBuffer = PerInstanceDataStoredInSsbo();
}

void RHIShaderBindingSet::ParseParameter(const std::string& parameter, std::string& outBinding, std::string& outVariable)
{
	TVector<std::string> splittedString = Utils::SplitString(parameter, ".");
	outBinding = splittedString[0];
	outVariable = splittedString[1];
}

bool RHIShaderBindingSet::HasBinding(const std::string& binding) const
{
	auto it = std::find_if(m_layoutBindings.begin(), m_layoutBindings.end(), [&binding](const RHI::ShaderLayoutBinding& shaderLayoutBinding)
		{
			return shaderLayoutBinding.m_name == binding;
		});

	return it != m_layoutBindings.end();
}

bool RHIShaderBindingSet::HasParameter(const std::string& parameter) const
{
	TVector<std::string> splittedString = Utils::SplitString(parameter, ".");
	const std::string& binding = splittedString[0];
	const std::string& variable = splittedString[1];

	auto index = m_layoutBindings.FindIf([&binding](const RHI::ShaderLayoutBinding& shaderLayoutBinding)
		{
			return shaderLayoutBinding.m_name == binding;
		});

	if (index != -1)
	{
		if (m_layoutBindings[index].m_members.FindIf([&variable](const RHI::ShaderLayoutBindingMember& shaderLayoutBinding)
			{
				return shaderLayoutBinding.m_name == variable;
			}) != -1)
		{
			return true;
		}
	}

	return false;
}

SAILOR_API bool RHIMaterial::IsReady() const
{
	auto bindings = GetBindings();
	return m_vertexShader.IsValid() && m_fragmentShader.IsValid() && bindings.IsValid() && bindings->IsReady();
}

RHIShaderBindingSetPtr RHIMaterial::GetBindings() const
{
	m_versionLock.Lock();
	auto result = m_version ? m_version->GetBindings() : RHIShaderBindingSetPtr{};
	m_versionLock.Unlock();
	return result;
}

RHIMaterialVersionPtr RHIMaterial::GetVersion() const
{
	m_versionLock.Lock();
	auto result = m_version;
	m_versionLock.Unlock();
	return result;
}

RHIMaterialVersionPtr RHIMaterial::GetVersionForSubmission(uint64_t submissionId) const
{
	if (submissionId == 0ull)
	{
		return GetVersion();
	}

	const size_t captureIndex =
		static_cast<size_t>(submissionId % NumSubmissionVersionCaptures);
	m_versionLock.Lock();
	if (m_submissionVersionCaptures[captureIndex].m_submissionId == submissionId)
	{
		const auto& capture = m_submissionVersionCaptures[captureIndex];
		auto result = capture.m_uncutVersion ? capture.m_uncutVersion :
			ResolveVersionLocked(capture.m_publicationRevision);
		m_versionLock.Unlock();
		return result;
	}
	m_versionLock.Unlock();

	uint64_t publicationRevision = 0ull;
	g_rhiMaterialPublicationLock.Lock();
	const bool bHasActiveCapture =
		FindActiveMaterialRevisionLocked(submissionId, publicationRevision);
	g_rhiMaterialPublicationLock.Unlock();

	m_versionLock.Lock();
	auto& capture = m_submissionVersionCaptures[captureIndex];
	if (capture.m_submissionId != submissionId)
	{
		capture.m_submissionId = submissionId;
		capture.m_publicationRevision = bHasActiveCapture ?
			publicationRevision : 0ull;
		capture.m_uncutVersion = bHasActiveCapture ?
			RHIMaterialVersionPtr{} : m_version;
	}
	auto result = capture.m_uncutVersion ? capture.m_uncutVersion :
		ResolveVersionLocked(capture.m_publicationRevision);
	m_versionLock.Unlock();
	return result;
}

void RHIMaterial::PublishVersionLocked(
	RHIMaterialVersionPtr version,
	uint64_t oldestActiveRevision)
{
	m_version = std::move(version);
	m_publishedVersions.Add(m_version);
	PrunePublishedVersionsLocked(oldestActiveRevision);
	if (m_publishedVersions.Num() > 1u)
	{
		auto self = ToRefPtr<RHIMaterial>();
		if (!g_materialsWithRetainedPublicationHistory.Contains(self))
		{
			g_materialsWithRetainedPublicationHistory.Add(std::move(self));
		}
	}
}

RHIMaterialVersionPtr RHIMaterial::ResolveVersionLocked(
	uint64_t publicationRevision) const
{
	for (size_t index = m_publishedVersions.Num(); index > 0u; --index)
	{
		const auto& candidate = m_publishedVersions[index - 1u];
		if (candidate && candidate->GetPublicationRevision() <= publicationRevision)
		{
			return candidate;
		}
	}
	return {};
}

void RHIMaterial::PrunePublishedVersionsLocked(uint64_t oldestActiveRevision)
{
	if (m_publishedVersions.Num() <= 1u)
	{
		return;
	}

	if (oldestActiveRevision == (std::numeric_limits<uint64_t>::max)())
	{
		m_publishedVersions.RemoveAt(0u, m_publishedVersions.Num() - 1u);
		return;
	}

	size_t firstRequired = 0u;
	while (firstRequired < m_publishedVersions.Num() &&
		m_publishedVersions[firstRequired]->GetPublicationRevision() <
			oldestActiveRevision)
	{
		++firstRequired;
	}
	if (firstRequired > 0u)
	{
		// The newest version before the oldest active cutoff is still needed if
		// a submission first encounters this material after a later publication.
		--firstRequired;
		if (firstRequired > 0u)
		{
			m_publishedVersions.RemoveAt(0u, firstRequired);
		}
	}
}

void RHIMaterial::SetBindings(RHIShaderBindingSetPtr bindings)
{
	const uint64_t versionId = g_rhiMaterialVersion.fetch_add(1ull, std::memory_order_relaxed);
	g_rhiMaterialPublicationLock.Lock();
	const uint64_t publicationRevision =
		g_publishedRhiMaterialRevision.load(std::memory_order_relaxed) + 1ull;
	m_versionLock.Lock();
	PublishVersionLocked(
		RHIMaterialVersionPtr::Make(
			versionId,
			std::move(bindings),
			publicationRevision),
		GetOldestActiveMaterialRevisionLocked());
	m_pendingVersion.Clear();
	m_versionLock.Unlock();
	g_publishedRhiMaterialRevision.store(publicationRevision, std::memory_order_release);
	g_rhiMaterialPublicationLock.Unlock();
}

void RHIMaterial::StageBindings(RHIShaderBindingSetPtr bindings)
{
	const uint64_t versionId = g_rhiMaterialVersion.fetch_add(1ull, std::memory_order_relaxed);
	m_versionLock.Lock();
	m_pendingVersion = RHIMaterialVersionPtr::Make(versionId, std::move(bindings));
	m_versionLock.Unlock();
}

bool RHIMaterial::TryPublishPendingBindings()
{
	m_versionLock.Lock();
	auto pending = m_pendingVersion;
	m_versionLock.Unlock();
	if (!pending || !pending->GetBindings() || !pending->GetBindings()->IsReady())
	{
		return false;
	}

	g_rhiMaterialPublicationLock.Lock();
	m_versionLock.Lock();
	if (m_pendingVersion != pending)
	{
		m_versionLock.Unlock();
		g_rhiMaterialPublicationLock.Unlock();
		return false;
	}
	const uint64_t publicationRevision =
		g_publishedRhiMaterialRevision.load(std::memory_order_relaxed) + 1ull;
	PublishVersionLocked(
		RHIMaterialVersionPtr::Make(
			pending->GetVersionId(),
			pending->GetBindings(),
			publicationRevision),
		GetOldestActiveMaterialRevisionLocked());
	m_pendingVersion.Clear();
	m_versionLock.Unlock();
	g_publishedRhiMaterialRevision.store(publicationRevision, std::memory_order_release);
	g_rhiMaterialPublicationLock.Unlock();
	return true;
}

uint64_t RHIMaterial::GetGlobalPublishedVersionRevision()
{
	return g_publishedRhiMaterialRevision.load(std::memory_order_acquire);
}

uint64_t RHIMaterial::BeginSubmissionVersionCapture(uint64_t submissionId)
{
	if (submissionId == 0ull)
	{
		return GetGlobalPublishedVersionRevision();
	}

	g_rhiMaterialPublicationLock.Lock();
	const uint64_t revision =
		g_publishedRhiMaterialRevision.load(std::memory_order_relaxed);
	ActiveMaterialSubmissionCapture* freeCapture = nullptr;
	for (auto& capture : g_activeMaterialSubmissionCaptures)
	{
		if (capture.m_submissionId == submissionId)
		{
			freeCapture = &capture;
			break;
		}
		if (!freeCapture && capture.m_submissionId == 0ull)
		{
			freeCapture = &capture;
		}
	}
	check(freeCapture);
	if (freeCapture)
	{
		*freeCapture = { submissionId, revision };
	}
	g_rhiMaterialPublicationLock.Unlock();
	return revision;
}

void RHIMaterial::EndSubmissionVersionCapture(uint64_t submissionId)
{
	if (submissionId == 0ull)
	{
		return;
	}

	g_rhiMaterialPublicationLock.Lock();
	for (auto& capture : g_activeMaterialSubmissionCaptures)
	{
		if (capture.m_submissionId == submissionId)
		{
			capture = {};
			break;
		}
	}

	const uint64_t oldestActiveRevision =
		GetOldestActiveMaterialRevisionLocked();
	size_t writeIndex = 0u;
	for (size_t readIndex = 0u;
		readIndex < g_materialsWithRetainedPublicationHistory.Num();
		++readIndex)
	{
		auto& material = g_materialsWithRetainedPublicationHistory[readIndex];
		if (!material)
		{
			continue;
		}
		material->m_versionLock.Lock();
		material->PrunePublishedVersionsLocked(oldestActiveRevision);
		const bool bKeepRetained = material->m_publishedVersions.Num() > 1u;
		material->m_versionLock.Unlock();
		if (bKeepRetained)
		{
			if (writeIndex != readIndex)
			{
				g_materialsWithRetainedPublicationHistory[writeIndex] =
					std::move(material);
			}
			++writeIndex;
		}
	}
	g_materialsWithRetainedPublicationHistory.Resize(writeIndex);
	g_rhiMaterialPublicationLock.Unlock();
}
