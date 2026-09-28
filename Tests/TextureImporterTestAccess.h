#pragma once
#include "AssetRegistry/Texture/TextureImporter.h"
#include <utility>

namespace Sailor
{
	class TextureImporterTestAccess
	{
	public:
		using Decoder = bool (*)(const TextureImporter::CpuDecodeRequest&, TextureImporter::ByteCode&,
			int32_t&, int32_t&, uint32_t&);
		static Decoder ExchangeDecoder(TextureImporter& importer, Decoder decoder)
		{
			return std::exchange(importer.m_decodeTexture, decoder);
		}
	};
}
