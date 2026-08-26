#pragma once

#include "Core/Defines.h"
#include "Containers/Vector.h"
#include "Memory/SharedPtr.hpp"

#include <array>
#include <cstdint>
#include <string>
#include <string_view>

#include <glm/glm.hpp>

namespace Sailor
{
	inline constexpr uint32_t ProbeVolumeFormatVersion = 2u;
	inline constexpr std::string_view ProbeVolumeBakerVersionPrefix =
		"Sailor ProbeVolumeBaker/";
	inline constexpr std::string_view ProbeVolumeCurrentBakerVersion =
		"Sailor ProbeVolumeBaker/10";
	inline constexpr uint32_t ProbeVolumeSphericalHarmonicsOrder = 2u;
	inline constexpr uint32_t ProbeVolumeSphericalHarmonicsCoefficientCount = 9u;
	inline constexpr uint32_t ProbeVolumeVisibilityDirectionCount = 6u;
	inline constexpr uint32_t ProbeVolumeBlockedDirectionShift = 8u;
	inline constexpr uint32_t ProbeVolumeBlockedDirectionMask =
		((1u << ProbeVolumeVisibilityDirectionCount) - 1u) <<
		ProbeVolumeBlockedDirectionShift;
	inline constexpr uint32_t ProbeVolumeMaxRaysPerProbe = 65536u;
	inline constexpr uint32_t ProbeVolumeMaxBounceCount = 64u;
	inline constexpr uint32_t ProbeVolumeMaxSubdivisionLevel = 16u;

	inline bool IsProbeVolumeBakerVersionSupported(
		const std::string& version) noexcept
	{
		const std::string_view view(version);
		return !view.starts_with(ProbeVolumeBakerVersionPrefix) ||
			view == ProbeVolumeCurrentBakerVersion;
	}

	enum class EProbeVolumeCompression : uint32_t
	{
		Float32 = 0u
	};

	enum class EProbeVolumeSampleFlag : uint32_t
	{
		None = 0u,
		Valid = 1u << 0u,
		Relocated = 1u << 1u
	};

	constexpr uint32_t operator|(
		EProbeVolumeSampleFlag lhs,
		EProbeVolumeSampleFlag rhs) noexcept
	{
		return static_cast<uint32_t>(lhs) | static_cast<uint32_t>(rhs);
	}

	constexpr uint32_t ProbeVolumeBlockedDirectionBit(
		uint32_t directionIndex) noexcept
	{
		return directionIndex < ProbeVolumeVisibilityDirectionCount ?
			1u << (ProbeVolumeBlockedDirectionShift + directionIndex) : 0u;
	}

	constexpr bool IsProbeVolumeDirectionBlocked(
		uint32_t flags,
		uint32_t directionIndex) noexcept
	{
		return (flags & ProbeVolumeBlockedDirectionBit(directionIndex)) != 0u;
	}

	struct SAILOR_SHARED_API ProbeVolumeBakeSettings final
	{
		uint32_t m_raysPerProbe = 256u;
		uint32_t m_bounceCount = 3u;
		uint32_t m_randomSeed = 0u;
		uint32_t m_maxSubdivisionLevel = ProbeVolumeMaxSubdivisionLevel;
		float m_minProbeSpacing = 1.0f;
		float m_normalBias = 0.05f;
		float m_viewBias = 0.05f;
		float m_maxRayDistance = 1000.0f;
		float m_skyIndirectIntensity = 1.0f;
		bool m_bIncludeSky = true;
		bool m_bIncludeEmissive = true;
		bool m_bIncludeDirectLighting = true;
	};

	struct SAILOR_SHARED_API ProbeVolumeBrick final
	{
		glm::vec3 m_min{};
		glm::vec3 m_max{};
		uint32_t m_subdivisionLevel = 0u;
		uint32_t m_firstProbeIndex = 0u;
		uint32_t m_probeCount = 0u;
		glm::uvec3 m_probeCounts{ 1u };
	};

	struct SAILOR_SHARED_API ProbeVolumeSample final
	{
		glm::vec3 m_position{};
		glm::vec3 m_relocationOffset{};
		float m_validity = 1.0f;
		// Valid/relocated state plus six baked local-occluder bits for
		// +X, -X, +Y, -Y, +Z and -Z starting at bit 8.
		uint32_t m_flags = static_cast<uint32_t>(EProbeVolumeSampleFlag::Valid);
		std::array<glm::vec3, ProbeVolumeSphericalHarmonicsCoefficientCount> m_irradiance{};
		// Exact locally clamped clearance distance and its square for rays along
		// +X, -X, +Y, -Y, +Z and -Z. A matching flag bit means that exact ray hit
		// geometry inside this probe's owning-brick interpolation support.
		// They are transport data and are selected from the compatible layout;
		// they are never blended with lighting coefficients.
		std::array<glm::vec2, ProbeVolumeVisibilityDirectionCount> m_visibility{};
		// Fraction of rays in each signed-axis lobe that reached the environment
		// without intersecting scene geometry. Unlike the locally clamped moments
		// above, this traces to maxRayDistance and occludes only environment
		// specular; it does not attenuate the baked diffuse SH a second time.
		std::array<float, ProbeVolumeVisibilityDirectionCount>
			m_environmentVisibility{ 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f };
	};

	struct SAILOR_SHARED_API ProbeVolumeDiagnostics final
	{
		uint32_t m_invalidProbeCount = 0u;
		uint32_t m_relocatedProbeCount = 0u;
		float m_averageValidity = 0.0f;
		float m_bakeDurationSeconds = 0.0f;
		std::string m_message{};
	};

	struct SAILOR_SHARED_API ProbeVolumeData final
	{
		uint32_t m_formatVersion = ProbeVolumeFormatVersion;
		uint32_t m_shOrder = ProbeVolumeSphericalHarmonicsOrder;
		EProbeVolumeCompression m_compression = EProbeVolumeCompression::Float32;
		glm::vec3 m_volumeMin{};
		glm::vec3 m_volumeMax{};
		ProbeVolumeBakeSettings m_bakeSettings{};
		uint64_t m_layoutHash = 0u;
		uint64_t m_representationHash = 0u;
		uint64_t m_transportHash = 0u;
		uint64_t m_lightingHash = 0u;
		uint64_t m_sourceWorldHash = 0u;
		std::string m_stateName{};
		std::string m_bakerVersion{};
		ProbeVolumeDiagnostics m_diagnostics{};
		TVector<ProbeVolumeBrick> m_bricks{};
		TVector<ProbeVolumeSample> m_probes{};

		bool Validate(std::string& outDiagnostic) const;
		bool IsCompositionCompatibleWith(
			const ProbeVolumeData& rhs,
			std::string& outDiagnostic) const;
	};

	using ProbeVolumeDataPtr = TSharedPtr<ProbeVolumeData>;

	SAILOR_SHARED_API uint64_t ComputeProbeVolumeLayoutHash(
		const ProbeVolumeData& data) noexcept;
	SAILOR_SHARED_API uint64_t ComputeProbeVolumeRepresentationHash(
		uint32_t formatVersion,
		uint32_t shOrder,
		EProbeVolumeCompression compression) noexcept;
}
