#pragma once

#include "AssetRegistry/Texture/TextureImporter.h"
#include "FrameGraph/SkyParameters.h"

namespace Sailor
{
	enum class EEnvironmentSource : uint8_t
	{
		Constant,
		Sky,
		Texture
	};

	// Incident lighting only. Local reflections contain outgoing scene radiance.
	struct SAILOR_SHARED_API EnvironmentSource
	{
		EEnvironmentSource m_type = EEnvironmentSource::Constant;
		TextureImporter::CpuDecodeRequest m_texture;
		RHI::ETextureFormat m_format = RHI::ETextureFormat::UNDEFINED;
		SkyParameters m_sky{};
		glm::vec3 m_constant{ 0.03f };
		float m_skyIndirectIntensity = 1.0f;

		uint64_t GetRevision() const;
	};

	SAILOR_SHARED_API bool CaptureEnvironmentSource(const std::string& environmentMap,
		const SkyParameters* sky, EnvironmentSource& outSource, std::string& outDiagnostic);
}
