#include "FrameGraph/EnvironmentSource.h"

#include "AssetRegistry/AssetRegistry.h"
#include "Containers/Hash.h"
#include "Math/Math.h"
#include "Sailor.h"
#include <format>

using namespace Sailor;

bool Sailor::CaptureEnvironmentSource(std::string_view environmentMap,
	const SkyParameters* sky, EnvironmentSource& outSource, std::string& outDiagnostic)
{
	outSource = {};
	outDiagnostic.clear();
	if (!environmentMap.empty())
	{
		const auto* asset = App::GetSubmodule<AssetRegistry>()->GetAssetInfoPtr<TextureAssetInfoPtr>(environmentMap);
		if (!asset || !TextureImporter::CaptureCpuDecodeRequest(*asset, outSource.m_texture))
		{
			outDiagnostic = std::format("cannot capture environment texture '{}'", environmentMap);
			return false;
		}
		outSource.m_type = EEnvironmentSource::Texture;
		outSource.m_format = asset->GetFormat();
	}
	else if (sky)
	{
		outSource.m_type = EEnvironmentSource::Sky;
		outSource.m_sky = *sky;
	}
	return true;
}

uint64_t EnvironmentSource::GetRevision() const
{
	uint64_t hash = Fnv1aOffsetBasis;
	HashValue(hash, m_type);
	switch (m_type)
	{
	case EEnvironmentSource::Texture:
		HashString(hash, m_texture.m_fileId.ToString());
		HashValue(hash, m_format);
		HashValue(hash, m_texture.m_glbTextureIndex);
		HashValue(hash, m_texture.m_bGenerateMips);
		for (const auto& source : m_texture.m_sourceRevisions)
		{
			HashString(hash, source.m_first);
			HashValue(hash, source.m_second->m_modificationTimeNanoseconds);
		}
		break;
	case EEnvironmentSource::Sky:
		for (const glm::vec3 value : { glm::vec3(m_sky.m_lightDirection),
			glm::vec3(m_sky.m_sunIlluminance), glm::vec3(m_sky.m_groundRadiance) })
		{
			HashValue(hash, value.x);
			HashValue(hash, value.y);
			HashValue(hash, value.z);
		}
		HashValue(hash, Math::AllFinite(m_sky.m_groundRadiance));
		HashValue(hash, m_skyIndirectIntensity);
		break;
	case EEnvironmentSource::Constant:
		HashValue(hash, m_constant.x);
		HashValue(hash, m_constant.y);
		HashValue(hash, m_constant.z);
		break;
	}
	return hash;
}
