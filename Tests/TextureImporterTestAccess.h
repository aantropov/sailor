#pragma once
#include "AssetRegistry/Texture/TextureImporter.h"
#include <utility>

namespace Sailor
{
	class TextureImporterTestAccess
	{
	public:
		static bool RegisterSampler(TextureImporter& importer, RHI::RHITexturePtr texture, size_t& index)
		{
			return importer.RegisterTextureSamplerBinding(std::move(texture), index);
		}

		using Decoder = bool (*)(const TextureImporter::CpuDecodeRequest&, TextureImporter::ByteCode&,
			int32_t&, int32_t&, uint32_t&);
		static Decoder ExchangeDecoder(TextureImporter& importer, Decoder decoder)
		{
			return std::exchange(importer.m_decodeTexture, decoder);
		}
		static Tasks::ITaskPtr GetLastAccess(TextureImporter& importer, FileId id)
		{
			TextureImporter::TextureEntry entry;
			importer.m_textures.TryGet(id, entry);
			return entry.m_lastAccess;
		}
	};
}
