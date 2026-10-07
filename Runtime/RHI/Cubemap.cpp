#include "Cubemap.h"

using namespace Sailor;
using namespace Sailor::RHI;

RHITexturePtr RHICubemap::GetFace(uint32_t face, uint32_t mipLevel) const
{
	if (!HasMipMaps() && mipLevel > 0)
	{
		return nullptr;
	}

	if (mipLevel == 0)
	{
		return m_faces[face];
	}

	return m_mipLevels[mipLevel - 1]->m_faces[face];
}

RHICubemapPtr RHICubemap::GetMipLevel(uint32_t mipLevel) const
{
	if (mipLevel == 0)
	{
		return const_cast<RHICubemap*>(this)->ToRefPtr<RHICubemap>();
	}

	if (!HasMipMaps())
	{
		return nullptr;
	}

	return m_mipLevels[mipLevel - 1];
}
