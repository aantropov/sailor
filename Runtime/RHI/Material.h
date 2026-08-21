#pragma once
#include "Memory/RefPtr.hpp"
#include "Types.h"
#include "Containers/Map.h"
#include "GraphicsDriver/Vulkan/VulkanApi.h"
#include "GraphicsDriver/Vulkan/VulkanPipeline.h"
#include <array>
#include <atomic>

namespace Sailor::RHI
{
	class RHIShaderBindingSet : public RHIResource, public IDelayedInitialization
	{
	public:

#if defined(SAILOR_BUILD_WITH_VULKAN)
		struct
		{
			GraphicsDriver::Vulkan::VulkanDescriptorSetPtr m_descriptorSet;
		} m_vulkan;
#endif

		SAILOR_API void UpdateLayoutShaderBinding(const ShaderLayoutBinding& layout);
		SAILOR_API void SetLayoutShaderBindings(TVector<RHI::ShaderLayoutBinding> layoutBindings);
		SAILOR_API const TVector<RHI::ShaderLayoutBinding>& GetLayoutBindings() const { return m_layoutBindings; }
		SAILOR_API RHI::RHIShaderBindingPtr& GetOrAddShaderBinding(const std::string& binding);

		SAILOR_API void RemoveShaderBinding(const std::string& binding);
		SAILOR_API const auto& GetShaderBindings() const { return m_shaderBindings; }

		static SAILOR_API void ParseParameter(const std::string& parameter, std::string& outBinding, std::string& outVariable);

		SAILOR_API bool HasBinding(const std::string& binding) const;
		SAILOR_API bool HasParameter(const std::string& parameter) const;

		SAILOR_API bool NeedsStorageBuffer() const { return m_bNeedsStorageBuffer; }
		SAILOR_API uint32_t GetStorageInstanceIndex(const std::string& binding) const;

		SAILOR_API size_t GetCompatibilityHashCode() const { return m_compatibilityHashCode; }
		SAILOR_API void RecalculateCompatibility();
		SAILOR_API uint64_t GetDescriptorRevision() const { return m_descriptorRevision.load(std::memory_order_acquire); }
		SAILOR_API void AdvanceDescriptorRevision() { m_descriptorRevision.fetch_add(1, std::memory_order_release); }
		SAILOR_API uint32_t GetVariableDescriptorCount() const { return m_variableDescriptorCount.load(std::memory_order_acquire); }
		SAILOR_API void SetVariableDescriptorCount(uint32_t count) { m_variableDescriptorCount.store(count, std::memory_order_release); }

	protected:

		SAILOR_API bool PerInstanceDataStoredInSsbo() const;

		TVector<RHI::ShaderLayoutBinding> m_layoutBindings;
		TConcurrentMap<std::string, RHI::RHIShaderBindingPtr> m_shaderBindings;
		bool m_bNeedsStorageBuffer = false;
		size_t m_compatibilityHashCode = 0;
		std::atomic<uint64_t> m_descriptorRevision{ 0 };
		std::atomic<uint32_t> m_variableDescriptorCount{ 0 };
	};

	/**
	 * Immutable descriptor/storage generation captured by draw packets. Keeping the
	 * version alive also keeps its material SSBO allocation and descriptor set alive
	 * until the owning render submission flight completes.
	 */
	class RHIMaterialVersion final : public RHIResource
	{
	public:
		RHIMaterialVersion(
			uint64_t versionId,
			RHIShaderBindingSetPtr bindings,
			uint64_t publicationRevision = 0ull) :
			m_versionId(versionId),
			m_bindings(std::move(bindings)),
			m_publicationRevision(publicationRevision)
		{}

		uint64_t GetVersionId() const { return m_versionId; }
		uint64_t GetPublicationRevision() const { return m_publicationRevision; }
		RHIShaderBindingSetPtr GetBindings() const { return m_bindings; }

	private:
		uint64_t m_versionId = 0ull;
		RHIShaderBindingSetPtr m_bindings{};
		uint64_t m_publicationRevision = 0ull;
	};

	using RHIMaterialVersionPtr = TRefPtr<RHIMaterialVersion>;

	class RHIMaterial : public RHIResource
	{
	public:
#if defined(SAILOR_BUILD_WITH_VULKAN)
		struct Vulkan
		{
			GraphicsDriver::Vulkan::VulkanGraphicsPipelinePtr GetOrAddPipeline(const TVector<VkFormat>& colorAttachments, VkFormat depthStencilAttachment);
			TVector<GraphicsDriver::Vulkan::VulkanGraphicsPipelinePtr> m_pipelines{};
			SpinLock m_pipelinesLock;

		} m_vulkan{};
#endif
		SAILOR_API RHIMaterial(RenderState renderState, RHIShaderPtr vertexShader, RHIShaderPtr fragmentShader) :
			m_renderState(std::move(renderState)),
			m_vertexShader(vertexShader),
			m_fragmentShader(fragmentShader)
		{}

		SAILOR_API bool IsReady() const;

		SAILOR_API const RHI::RenderState& GetRenderState() const { return m_renderState; }

		SAILOR_API RHIShaderPtr GetVertexShader() const { return m_vertexShader; }
		SAILOR_API RHIShaderPtr GetFragmentShader() const { return m_fragmentShader; }

		SAILOR_API RHIShaderBindingSetPtr GetBindings() const;
		SAILOR_API RHIMaterialVersionPtr GetVersion() const;
		SAILOR_API RHIMaterialVersionPtr GetVersionForSubmission(uint64_t submissionId) const;
		SAILOR_API void SetBindings(RHIShaderBindingSetPtr bindings);
		SAILOR_API void StageBindings(RHIShaderBindingSetPtr bindings);
		SAILOR_API bool TryPublishPendingBindings();
		SAILOR_API static uint64_t GetGlobalPublishedVersionRevision();
		SAILOR_API static uint64_t BeginSubmissionVersionCapture(uint64_t submissionId);
		SAILOR_API static void EndSubmissionVersionCapture(uint64_t submissionId);

	protected:
		void PublishVersionLocked(
			RHIMaterialVersionPtr version,
			uint64_t oldestActiveRevision);
		RHIMaterialVersionPtr ResolveVersionLocked(uint64_t publicationRevision) const;
		void PrunePublishedVersionsLocked(uint64_t oldestActiveRevision);

		RHI::RenderState m_renderState;

		RHIShaderPtr m_vertexShader;
		RHIShaderPtr m_fragmentShader;

		mutable SpinLock m_versionLock;
		RHIMaterialVersionPtr m_version{};
		RHIMaterialVersionPtr m_pendingVersion{};
		TVector<RHIMaterialVersionPtr> m_publishedVersions{};
		struct SubmissionVersionCapture
		{
			uint64_t m_submissionId = 0ull;
			uint64_t m_publicationRevision = 0ull;
			// Compatibility fallback for callers that did not register a global
			// submission cutoff. Renderer submissions use only the revision.
			RHIMaterialVersionPtr m_uncutVersion{};
		};
		// One extra entry beyond the required three flights avoids an immediate
		// modulo collision while the next CPU submission begins.
		static constexpr size_t NumSubmissionVersionCaptures = 4u;
		mutable std::array<SubmissionVersionCapture, NumSubmissionVersionCaptures>
			m_submissionVersionCaptures{};

		friend class IGraphicsDriver;
	};
};
