#pragma once
#include "Core/Defines.h"
#include "Containers/Map.h"
#include "Containers/Set.h"
#include "Containers/Vector.h"
#include "RHI/Types.h"

#include <bitset>

namespace Sailor::Framegraph
{
	class TextureDependencyCollector
	{
	public:
		static constexpr size_t MaxTrackedTextures = 8192u;

		void Reset()
		{
			m_seen.reset();
			m_indices.Clear(false);
			Insert(0u);
			m_bSorted = true;
		}

		void Insert(uint32_t textureIndex)
		{
			if (textureIndex >= MaxTrackedTextures || m_seen.test(textureIndex))
			{
				return;
			}
			m_seen.set(textureIndex);
			m_indices.Add(textureIndex);
			m_bSorted = false;
		}

		const TVector<uint32_t>& GetIndices()
		{
			if (!m_bSorted)
			{
				m_indices.Sort();
				m_bSorted = true;
			}
			return m_indices;
		}

	private:
		std::bitset<MaxTrackedTextures> m_seen{};
		TVector<uint32_t> m_indices{};
		bool m_bSorted = true;
	};

	struct TextureBindingCacheKey
	{
		TextureBindingCacheKey() = default;
		explicit TextureBindingCacheKey(const TSet<uint32_t>& lookupTextures) :
			m_lookupTextures(&lookupTextures)
		{
		}

		void Materialize()
		{
			if (!m_lookupTextures)
			{
				return;
			}

			m_requestedTextures.Clear(false);
			m_requestedTextures.Reserve(GetNumRequestedTextures());
			ForEachRequestedTexture([&](uint32_t textureIndex)
				{
					m_requestedTextures.Add(textureIndex);
				});
			m_requestedTextures.Sort();
			m_lookupTextures = nullptr;
		}

		bool operator==(const TextureBindingCacheKey& rhs) const
		{
			if (GetNumRequestedTextures() != rhs.GetNumRequestedTextures())
			{
				return false;
			}

			bool bEqual = true;
			ForEachRequestedTexture([&](uint32_t textureIndex)
				{
					bEqual = bEqual && rhs.ContainsRequestedTexture(textureIndex);
				});
			rhs.ForEachRequestedTexture([&](uint32_t textureIndex)
				{
					bEqual = bEqual && ContainsRequestedTexture(textureIndex);
				});
			return bEqual;
		}

		size_t GetHash() const
		{
			uint64_t xorHash = 0ull;
			uint64_t sumHash = 0ull;
			ForEachRequestedTexture([&](uint32_t textureIndex)
				{
					uint64_t elementHash =
						static_cast<uint64_t>(textureIndex) + 0x9e3779b97f4a7c15ull;
					elementHash =
						(elementHash ^ (elementHash >> 30u)) *
						0xbf58476d1ce4e5b9ull;
					elementHash =
						(elementHash ^ (elementHash >> 27u)) *
						0x94d049bb133111ebull;
					elementHash ^= elementHash >> 31u;
					xorHash ^= elementHash;
					sumHash += elementHash;
				});
			size_t result = 1469598103934665603ull;
			HashCombine(
				result,
				GetNumRequestedTextures(),
				xorHash,
				sumHash);
			return result;
		}

		TVector<uint32_t> m_requestedTextures{};

	private:
		template<typename TCallback>
		void ForEachRequestedTexture(TCallback&& callback) const
		{
			callback(0u);
			if (m_lookupTextures)
			{
				for (uint32_t textureIndex : *m_lookupTextures)
				{
					if (textureIndex > 0u &&
						textureIndex < TextureDependencyCollector::MaxTrackedTextures)
					{
						callback(textureIndex);
					}
				}
				return;
			}

			for (uint32_t textureIndex : m_requestedTextures)
			{
				if (textureIndex > 0u &&
					textureIndex < TextureDependencyCollector::MaxTrackedTextures)
				{
					callback(textureIndex);
				}
			}
		}

		size_t GetNumRequestedTextures() const
		{
			size_t result = 1u;
			if (m_lookupTextures)
			{
				for (uint32_t textureIndex : *m_lookupTextures)
				{
					result += textureIndex > 0u &&
						textureIndex < TextureDependencyCollector::MaxTrackedTextures;
				}
				return result;
			}

			for (uint32_t textureIndex : m_requestedTextures)
			{
				result += textureIndex > 0u &&
					textureIndex < TextureDependencyCollector::MaxTrackedTextures;
			}
			return result;
		}

		bool ContainsRequestedTexture(uint32_t textureIndex) const
		{
			if (textureIndex == 0u)
			{
				return true;
			}
			if (textureIndex >= TextureDependencyCollector::MaxTrackedTextures)
			{
				return false;
			}
			return m_lookupTextures ?
				m_lookupTextures->Contains(textureIndex) :
				m_requestedTextures.Contains(textureIndex);
		}

		const TSet<uint32_t>* m_lookupTextures = nullptr;
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

	inline const TSet<uint32_t>& GetDefaultRequestedTextures()
	{
		static const TSet<uint32_t> DefaultRequestedTextures{ 0u };
		return DefaultRequestedTextures;
	}

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

	SAILOR_API uint64_t CalculateTextureDependencyRevision(
		const TVector<uint32_t>& requestedTextures);
}
