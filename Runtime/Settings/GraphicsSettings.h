#pragma once

#include "Core/Defines.h"
#include "Engine/Types.h"

#include <array>
#include <cstdint>
#include <filesystem>
#include <string>

namespace Sailor::Workspace
{
	class WorkspaceContext;
}

namespace Sailor::RHI
{
	enum class EMsaaSamples : uint8_t;
}

namespace Sailor::Settings
{
#if defined(_MSC_VER)
#pragma warning(push)
#pragma warning(disable: 4251)
#endif

	inline constexpr uint32_t ProjectGraphicsSettingsVersion = 1u;
	inline constexpr uint32_t EditorGraphicsSettingsVersion = 1u;
	inline constexpr uint32_t NumGraphicsQualityPresets = 5u;
	inline constexpr uint32_t MaxShadowCascades = 4u;

	enum class EGraphicsQuality : uint8_t
	{
		Ultra = 0,
		High,
		Medium,
		Low,
		VeryLow
	};

	enum class EGraphicsQualitySelection : uint8_t
	{
		ProjectDefault = 0,
		Ultra,
		High,
		Medium,
		Low,
		VeryLow
	};

	enum class ERenderStatsMode : uint8_t
	{
		None = 0,
		RenderStats,
		RenderStatsAndQueries
	};

	enum class EGraphicsSettingsLoadStatus : uint8_t
	{
		NotLoaded = 0,
		Missing,
		Loaded,
		Invalid,
		UnsupportedVersion,
		IoFailure
	};

	struct SAILOR_SHARED_API GraphicsExtent final
	{
		uint32_t m_width = 1u;
		uint32_t m_height = 1u;
	};

	struct SAILOR_SHARED_API GraphicsQualityProfile final
	{
		float m_resolutionFactor = 1.0f;
		uint32_t m_fpsCap = 120u;
		uint32_t m_msaaSamples = 1u;
		ELightShadowQuality m_shadowQuality = ELightShadowQuality::Medium;
		float m_shadowBias = 1.25f;
		uint32_t m_shadowCascadeCount = 1u;
		std::array<uint32_t, MaxShadowCascades> m_shadowCascadeResolutions
		{
			512u,
			0u,
			0u,
			0u
		};
		bool m_bSupportSoftShadows = false;
		float m_cloudsResolutionMultiplier = 0.5f;
		bool m_bCloudsDithering = false;
		uint32_t m_skyResolution = 256u;
		uint32_t m_vegetationInstanceBudget = 8192u;
		int32_t m_lodBias = 0;
		bool m_bEnableGlobalIllumination = true;
		uint32_t m_maxGiProbeStatesPerSnapshot = 3u;

		bool IsShadowCascadeActive(uint32_t cascadeIndex) const noexcept;
		uint32_t GetShadowCascadeResolution(uint32_t cascadeIndex) const noexcept;
	};

	struct SAILOR_SHARED_API GraphicsSettings final
	{
		GraphicsSettings();

		uint32_t m_version = ProjectGraphicsSettingsVersion;
		EGraphicsQuality m_defaultQuality = EGraphicsQuality::High;
		std::array<GraphicsQualityProfile, NumGraphicsQualityPresets> m_presets{};

		const GraphicsQualityProfile& GetProfile(EGraphicsQuality quality) const noexcept;
	};

	struct SAILOR_SHARED_API EditorGraphicsSettings final
	{
		uint32_t m_version = EditorGraphicsSettingsVersion;
		EGraphicsQualitySelection m_selectedQuality = EGraphicsQualitySelection::ProjectDefault;
		ERenderStatsMode m_statsMode = ERenderStatsMode::None;
	};

	struct SAILOR_SHARED_API ProjectGraphicsSettingsLoadResult final
	{
		EGraphicsSettingsLoadStatus m_status = EGraphicsSettingsLoadStatus::NotLoaded;
		std::string m_diagnostic;
		GraphicsSettings m_settings;

		bool IsLoaded() const noexcept { return m_status == EGraphicsSettingsLoadStatus::Loaded; }
	};

	struct SAILOR_SHARED_API EditorGraphicsSettingsLoadResult final
	{
		EGraphicsSettingsLoadStatus m_status = EGraphicsSettingsLoadStatus::NotLoaded;
		std::string m_diagnostic;
		EditorGraphicsSettings m_settings;

		bool IsLoaded() const noexcept { return m_status == EGraphicsSettingsLoadStatus::Loaded; }
	};

	struct SAILOR_SHARED_API GraphicsSettingsState final
	{
		GraphicsSettings m_projectSettings;
		EditorGraphicsSettings m_editorSettings;
		EGraphicsQuality m_activeQuality = EGraphicsQuality::High;
		EGraphicsSettingsLoadStatus m_projectLoadStatus = EGraphicsSettingsLoadStatus::NotLoaded;
		EGraphicsSettingsLoadStatus m_editorLoadStatus = EGraphicsSettingsLoadStatus::NotLoaded;
		std::string m_projectDiagnostic;
		std::string m_editorDiagnostic;
		bool m_bEditorMode = false;

		const GraphicsQualityProfile& GetActiveProfile() const noexcept;
	};

	SAILOR_SHARED_API EGraphicsQuality ResolveQualitySelection(
		EGraphicsQualitySelection selection,
		EGraphicsQuality projectDefault) noexcept;

	SAILOR_SHARED_API RHI::EMsaaSamples ToMsaaSamples(uint32_t samples) noexcept;
	SAILOR_SHARED_API GraphicsExtent ResolveRenderDimensions(
		uint32_t outputWidth,
		uint32_t outputHeight,
		float resolutionFactor) noexcept;
	SAILOR_SHARED_API GraphicsExtent ResolveSkyExtent(
		const GraphicsQualityProfile& profile) noexcept;
	SAILOR_SHARED_API GraphicsExtent ResolveCloudsExtent(
		uint32_t renderWidth,
		uint32_t renderHeight,
		const GraphicsQualityProfile& profile,
		float platformMultiplier = 1.0f) noexcept;
	SAILOR_SHARED_API ELightShadowQuality ApplyShadowQualityCap(
		ELightShadowQuality authoredQuality,
		ELightShadowQuality qualityCap) noexcept;
	SAILOR_SHARED_API uint32_t ApplyLodBias(
		uint32_t selectedLod,
		uint32_t numAvailableLods,
		uint32_t authoredMinLod,
		uint32_t authoredMaxLod,
		int32_t lodBias) noexcept;

	SAILOR_SHARED_API ProjectGraphicsSettingsLoadResult ParseProjectGraphicsSettings(
		const std::string& payload,
		const std::string& sourceName = "ProjectSettings.yaml") noexcept;
	SAILOR_SHARED_API EditorGraphicsSettingsLoadResult ParseEditorGraphicsSettings(
		const std::string& payload,
		const std::string& sourceName = "EditorSettings.yaml") noexcept;
	SAILOR_SHARED_API ProjectGraphicsSettingsLoadResult LoadProjectGraphicsSettings(
		const std::filesystem::path& path) noexcept;
	SAILOR_SHARED_API EditorGraphicsSettingsLoadResult LoadEditorGraphicsSettings(
		const std::filesystem::path& path) noexcept;
	SAILOR_SHARED_API GraphicsSettingsState LoadGraphicsSettings(
		const Workspace::WorkspaceContext& workspaceContext,
		bool bEditorMode) noexcept;

#if defined(_MSC_VER)
#pragma warning(pop)
#endif
}
