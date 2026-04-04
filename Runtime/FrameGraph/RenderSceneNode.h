#pragma once
#include "Core/Defines.h"
#include "Memory/RefPtr.hpp"
#include "Engine/Object.h"
#include "RHI/Types.h"
#include "FrameGraph/BaseFrameGraphNode.h"
#include "FrameGraph/FrameGraphNode.h"
#include "RHI/Batch.hpp"

namespace Sailor::Framegraph
{
	class RenderSceneNode : public TFrameGraphNode<RenderSceneNode>
	{
	public:

		class PerInstanceData
		{
		public:

			glm::mat4 model;
			vec4 sphereBounds;
			uint32_t materialInstance = 0;
			uint32_t skeletonOffset = 0;
			uint32_t bIsCulled = 0;
			uint32_t padding = 0;

			bool operator==(const PerInstanceData& rhs) const { return this->materialInstance == rhs.materialInstance && this->model == rhs.model; }

			size_t GetHash() const
			{
				hash<glm::mat4> p;
				return p(model);
			}
		};

		SAILOR_API static const char* GetName() { return m_name; }

		SAILOR_API virtual Sailor::Tasks::TaskPtr<void, void> Prepare(RHI::RHIFrameGraphPtr frameGraph, const RHI::RHISceneViewSnapshot& sceneView) override;
		SAILOR_API virtual void Process(RHI::RHIFrameGraphPtr frameGraph, RHI::RHICommandListPtr transferCommandList, RHI::RHICommandListPtr commandLists, const RHI::RHISceneViewSnapshot& sceneView) override;
		SAILOR_API virtual void Clear() override;
		SAILOR_API RHI::ESortingOrder GetSortingOrder() const;

	protected:

		// macOS/MoltenVK has a much tighter practical limit on bound texture descriptors.
		// We cache texture binding sets and split batches so they fit those limits.
		struct TextureBindingCacheKey
		{
			TVector<uint32_t> m_requestedTextures;

			bool operator==(const TextureBindingCacheKey& rhs) const { return m_requestedTextures == rhs.m_requestedTextures; }

			size_t GetHash() const
			{
				size_t hash = 0;
				for (const uint32_t textureIndex : m_requestedTextures)
				{
					HashCombine(hash, textureIndex);
				}
				return hash;
			}
		};

		struct TextureBindingCacheEntry
		{
			RHI::RHIShaderBindingSetPtr m_textureBindings;
			uint32_t m_textureSetSize = 1;
			uint64_t m_lastUsedFrame = 0;
		};

		RHI::RHIShaderBindingSetPtr GetTextureBindingSet(const TSet<uint32_t>& requestedTextures, uint64_t frame, uint32_t& outSupportedMeshesPerBatch);
		void EvictTextureBindingCache(uint64_t frame);
		static uint32_t CalculatePlannedTextureSlotCount(const TVector<uint32_t>& requestedTextures);

		static const char* m_name;

		uint32_t m_numMeshes = 0;
		SpinLock m_syncSharedResources;
		RHI::TDrawCalls<PerInstanceData> m_drawCalls;
		TSet<RHI::RHIBatch> m_batches;
		TVector<RHI::RHIBufferPtr> m_indirectBuffers;

		RHI::RHIShaderBindingSetPtr m_perInstanceData;
		size_t m_sizePerInstanceData = 0;

		// Culling
		TVector<RHI::RHIShaderBindingSetPtr> m_cullingIndirectBufferBinding;
		ShaderSetPtr m_pComputeMeshCullingShader{};
		RHI::RHIShaderBindingSetPtr m_computeMeshCullingBindings{};

		// Shared cache across platforms; macOS relies on it most because of descriptor pressure.
		TMap<TextureBindingCacheKey, TextureBindingCacheEntry> m_textureBindingCache;

		// macOS/MoltenVK has a much tighter practical limit on the amount of bound texture descriptors.
		// RenderScene uses these constants to keep batch-local texture sets within stable limits.
		static constexpr uint32_t MaxTextureSlotsPerBatch = 1024u;
		static constexpr uint64_t MaxTextureBindingCacheUnusedFrames = 5u;
	};

	template class TFrameGraphNode<RenderSceneNode>;
};
