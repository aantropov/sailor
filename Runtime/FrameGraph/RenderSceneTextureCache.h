#pragma once
#include "Core/Defines.h"
#include "Containers/Map.h"
#include "Containers/Set.h"
#include "Containers/Vector.h"
#include "RHI/Types.h"

namespace Sailor::Framegraph
{
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
		RHI::RHIBufferPtr m_textureRemapBuffer;
		uint32_t m_textureSetSize = 1;
		uint64_t m_lastUsedFrame = 0;
		uint64_t m_sourceDescriptorRevision = 0;
		TVector<uint64_t> m_sourceSlotRevisions;
	};

	using TextureBindingCache = TMap<TextureBindingCacheKey, TextureBindingCacheEntry>;
}

namespace Sailor::Framegraph::Details
{
	static constexpr uint32_t MaxTextureSlotsPerBatch = 1024u;
	static constexpr uint64_t MaxTextureBindingCacheUnusedFrames = 5u;

	SAILOR_API RHI::RHIShaderBindingSetPtr GetTextureBindingSet(
		TextureBindingCache& cache,
		const TSet<uint32_t>& requestedTextures,
		uint64_t frame,
		uint32_t& outSupportedMeshesPerBatch);

	SAILOR_API void EvictTextureBindingCache(
		TextureBindingCache& cache,
		uint64_t frame);

	SAILOR_API TVector<uint32_t> BuildDenseTextureRemap(
		const TVector<uint32_t>& globalTextureIndices);

	SAILOR_API bool CanReuseRenderSceneTextureBindings(
		uint64_t cachedSourceRevision,
		uint64_t currentSourceRevision,
		bool bHasCachedBindings,
		const TVector<uint64_t>* cachedSlotRevisions = nullptr,
		const TVector<uint64_t>* currentSlotRevisions = nullptr);
}
