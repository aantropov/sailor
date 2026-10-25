#pragma once
#include "Core/Defines.h"
#include "Containers/Map.h"
#include "Containers/Vector.h"
#include "RHI/Types.h"

namespace Sailor::RHI
{
	struct PackedDrawGroup;
}

namespace Sailor::Framegraph
{
	struct TextureBindingCacheKey
	{
		TextureBindingCacheKey() = default;
		// Borrow canonical scene-resource indices for lookup; own them before insertion.
		explicit TextureBindingCacheKey(const TVector<uint32_t>& lookupTextures) :
			m_lookupTextures(&lookupTextures)
		{
		}

		void Materialize()
		{
			if (m_lookupTextures)
			{
				m_requestedTextures = *m_lookupTextures;
				m_lookupTextures = nullptr;
			}
		}

		const TVector<uint32_t>& GetTextures() const
		{
			return m_lookupTextures ? *m_lookupTextures : m_requestedTextures;
		}

		bool operator==(const TextureBindingCacheKey& rhs) const
		{
			return GetTextures() == rhs.GetTextures();
		}

		size_t GetHash() const
		{
			const auto& textures = GetTextures();
			size_t result = Fnv1aOffsetBasis;
			HashCombine(result, textures.Num());
			for (uint32_t texture : textures)
			{
				HashCombine(result, texture);
			}
			return result;
		}

	private:
		TVector<uint32_t> m_requestedTextures{};
		const TVector<uint32_t>* m_lookupTextures = nullptr;
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

	struct TextureBindingGroups
	{
		TVector<TVector<uint32_t>> m_textures;
		TVector<uint32_t> m_requestGroups;
	};

	struct TextureBindingGroupingCache
	{
		TVector<RHI::RHIShaderBindingSetPtr> m_sources;
		TextureBindingGroups m_plan;
	};
}

namespace Sailor::Framegraph::Details
{
	static constexpr uint32_t MaxTextureSlotsPerBatch = 1024u;
	static constexpr uint64_t MaxTextureBindingCacheUnusedFrames = 5u;

	inline const TVector<uint32_t>& GetDefaultRequestedTextures()
	{
		static const TVector<uint32_t> DefaultRequestedTextures{ 0u };
		return DefaultRequestedTextures;
	}

	// A cached fallback remains usable even when the current request could not be prepared.
	SAILOR_API RHI::RHIShaderBindingSetPtr GetTextureBindingSet(
		TextureBindingCache& cache,
		const TVector<uint32_t>& requestedTextures,
		uint64_t frame,
		uint32_t& outSupportedMeshesPerBatch,
		bool& outCurrent);

	SAILOR_API void EvictTextureBindingCache(
		TextureBindingCache& cache,
		uint64_t frame);

	SAILOR_API TextureBindingGroups BuildTextureBindingGroups(
		const TVector<TVector<uint32_t>>& requests);

	SAILOR_API void ApplyTextureBindingGroups(
		TextureBindingCache& cache,
		TextureBindingGroupingCache& groupingCache,
		TVector<RHI::PackedDrawGroup>& draws,
		uint64_t frame);

	SAILOR_API TVector<uint32_t> BuildDenseTextureRemap(
		const TVector<uint32_t>& globalTextureIndices);

	SAILOR_API bool CanReuseRenderSceneTextureBindings(
		uint64_t cachedSourceRevision,
		uint64_t currentSourceRevision,
		bool bHasCachedBindings,
		const TVector<uint64_t>* cachedSlotRevisions = nullptr,
		const TVector<uint64_t>* currentSlotRevisions = nullptr);

	SAILOR_API uint64_t CalculateTextureDependencyRevision(
		const TVector<uint32_t>& requestedTextures);
}
