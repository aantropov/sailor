#pragma once

#include "Containers/Vector.h"
#include "Core/Defines.h"
#include "Memory/SharedPtr.hpp"

#include <array>
#include <cstdint>
#include <string>
#include <string_view>

#include <glm/glm.hpp>

namespace Sailor
{
	inline constexpr uint32_t GIProbesFormatVersion = 1u;
	inline constexpr std::string_view GIProbesBakerVersionPrefix =
		"Sailor GIProbesBaker/";
	inline constexpr std::string_view GIProbesCurrentBakerVersion =
		"Sailor GIProbesBaker/1";
	inline constexpr uint32_t GIProbeSphericalHarmonicsOrder = 2u;
	inline constexpr uint32_t GIProbeSphericalHarmonicsCoefficientCount = 9u;
	inline constexpr uint32_t GIProbeVisibilityDirectionCount = 6u;
	inline constexpr uint32_t GIProbeBlockedDirectionShift = 8u;
	inline constexpr uint32_t GIProbeBlockedDirectionMask =
		((1u << GIProbeVisibilityDirectionCount) - 1u) <<
		GIProbeBlockedDirectionShift;
	inline constexpr uint32_t GIProbesMaxRaysPerProbe = 65536u;
	inline constexpr uint32_t GIProbesMaxBounceCount = 64u;
	inline constexpr uint32_t GIProbesMaxSubdivisionLevel = 16u;

	inline bool IsGIProbesBakerVersionSupported(
		const std::string& version) noexcept
	{
		const std::string_view view(version);
		return !view.starts_with(GIProbesBakerVersionPrefix) ||
			view == GIProbesCurrentBakerVersion;
	}

	enum class EGIProbesCompression : uint32_t
	{
		Float32 = 0u
	};

	enum class EGIProbeFlag : uint32_t
	{
		None = 0u,
		Valid = 1u << 0u,
		Relocated = 1u << 1u
	};

	constexpr uint32_t operator|(
		EGIProbeFlag lhs,
		EGIProbeFlag rhs) noexcept
	{
		return static_cast<uint32_t>(lhs) | static_cast<uint32_t>(rhs);
	}

	constexpr uint32_t GIProbeBlockedDirectionBit(
		uint32_t directionIndex) noexcept
	{
		return directionIndex < GIProbeVisibilityDirectionCount ?
			1u << (GIProbeBlockedDirectionShift + directionIndex) : 0u;
	}

	constexpr bool IsGIProbeDirectionBlocked(
		uint32_t flags,
		uint32_t directionIndex) noexcept
	{
		return (flags & GIProbeBlockedDirectionBit(directionIndex)) != 0u;
	}

	struct SAILOR_SHARED_API GIProbesBakeSettings final
	{
		uint32_t m_raysPerProbe = 256u;
		uint32_t m_bounceCount = 3u;
		uint32_t m_randomSeed = 0u;
		uint32_t m_maxSubdivisionLevel = GIProbesMaxSubdivisionLevel;
		float m_minProbeSpacing = 1.0f;
		float m_normalBias = 0.05f;
		float m_viewBias = 0.05f;
		float m_maxRayDistance = 1000.0f;
		float m_skyIndirectIntensity = 1.0f;
		bool m_bIncludeSky = true;
		bool m_bIncludeEmissive = true;
		bool m_bIncludeDirectLighting = true;
	};

	struct SAILOR_SHARED_API GIProbeBrick final
	{
		glm::vec3 m_min{};
		glm::vec3 m_max{};
		uint32_t m_subdivisionLevel = 0u;
		uint32_t m_firstProbeIndex = 0u;
		uint32_t m_probeCount = 0u;
		glm::uvec3 m_probeCounts{ 1u };
	};

	struct SAILOR_SHARED_API GIProbe final
	{
		glm::vec3 m_position{};
		glm::vec3 m_relocationOffset{};
		float m_validity = 1.0f;
		// Valid/relocated state plus six baked local-occluder bits for
		// +X, -X, +Y, -Y, +Z and -Z starting at bit 8.
		uint32_t m_flags = static_cast<uint32_t>(EGIProbeFlag::Valid);
		std::array<glm::vec3, GIProbeSphericalHarmonicsCoefficientCount> m_irradiance{};
		// Exact locally clamped clearance distance and its square for rays along
		// +X, -X, +Y, -Y, +Z and -Z. A matching flag bit means that exact ray hit
		// geometry inside this probe's owning-brick interpolation support. These
		// values preserve bake/layout diagnostics and compatibility, but runtime
		// diffuse sampling must not extend six ray hits into infinite blocker planes.
		std::array<glm::vec2, GIProbeVisibilityDirectionCount> m_visibility{};
		// Fraction of rays in each signed-axis lobe that reached the environment
		// without intersecting scene geometry. Unlike the locally clamped moments
		// above, this traces to maxRayDistance and occludes only environment
		// specular; it does not attenuate the baked diffuse SH a second time.
		std::array<float, GIProbeVisibilityDirectionCount>
			m_environmentVisibility{ 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f };
	};

	struct SAILOR_SHARED_API GIProbesDiagnostics final
	{
		uint32_t m_invalidProbeCount = 0u;
		uint32_t m_relocatedProbeCount = 0u;
		float m_averageValidity = 0.0f;
		float m_bakeDurationSeconds = 0.0f;
		std::string m_message{};
	};

	struct SAILOR_SHARED_API GIProbesData final
	{
		uint32_t m_formatVersion = GIProbesFormatVersion;
		uint32_t m_shOrder = GIProbeSphericalHarmonicsOrder;
		EGIProbesCompression m_compression = EGIProbesCompression::Float32;
		glm::vec3 m_volumeMin{};
		glm::vec3 m_volumeMax{};
		GIProbesBakeSettings m_bakeSettings{};
		uint64_t m_layoutHash = 0u;
		uint64_t m_representationHash = 0u;
		uint64_t m_transportHash = 0u;
		uint64_t m_lightingHash = 0u;
		uint64_t m_sourceWorldHash = 0u;
		std::string m_stateName{};
		std::string m_bakerVersion{};
		GIProbesDiagnostics m_diagnostics{};
		TVector<GIProbeBrick> m_bricks{};
		TVector<GIProbe> m_probes{};

		bool Validate(std::string& outDiagnostic) const;
		bool IsCompositionCompatibleWith(
			const GIProbesData& rhs,
			std::string& outDiagnostic) const;
	};

	using GIProbesDataPtr = TSharedPtr<GIProbesData>;

	SAILOR_SHARED_API uint64_t ComputeGIProbesLayoutHash(
		const GIProbesData& data) noexcept;
	SAILOR_SHARED_API uint64_t ComputeGIProbesRepresentationHash(
		uint32_t formatVersion,
		uint32_t shOrder,
		EGIProbesCompression compression) noexcept;
}
