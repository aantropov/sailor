#include "Sailor.h"
#include "ECS/LightingECS.h"
#include "Settings/GraphicsSettings.h"
#include "Workspace/WorkspaceContext.h"
#include "RHI/GpuFrameTimeQueryRing.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <stdexcept>
#include <string>

using namespace Sailor;
using namespace Sailor::Settings;

namespace
{
	class TempDirectory final
	{
	public:
		explicit TempDirectory(const char* label)
		{
			static uint64_t nextId = 0u;
			const auto timestamp = std::chrono::steady_clock::now().time_since_epoch().count();
			m_path = std::filesystem::temp_directory_path() /
				("sailor-graphics-settings-" + std::string(label) + "-" +
					std::to_string(timestamp) + "-" + std::to_string(nextId++));
			std::filesystem::create_directories(m_path);
		}

		~TempDirectory() noexcept
		{
			std::error_code removeError;
			std::filesystem::remove_all(m_path, removeError);
		}

		const std::filesystem::path& Get() const noexcept { return m_path; }
		std::filesystem::path Path(const std::filesystem::path& relative) const
		{
			return m_path / relative;
		}

	private:
		std::filesystem::path m_path;
	};

	void Require(bool condition, const std::string& message)
	{
		if (!condition)
		{
			throw std::runtime_error(message);
		}
	}

	bool IsNear(float lhs, float rhs, float tolerance = 0.0001f)
	{
		return std::abs(lhs - rhs) <= tolerance;
	}

	void WriteText(const std::filesystem::path& path, const std::string& text)
	{
		std::ofstream output(path, std::ios::binary);
		output << text;
		Require(output.good(), "test settings should be writable: " + path.generic_string());
	}

	std::string ReadRepositoryFile(const std::filesystem::path& relativePath)
	{
		std::ifstream input(
			std::filesystem::path(SAILOR_TEST_SOURCE_DIR) / relativePath,
			std::ios::binary);
		Require(input.good(), "repository source should be readable: " + relativePath.generic_string());
		std::string text = std::string(
			std::istreambuf_iterator<char>(input),
			std::istreambuf_iterator<char>());
		text.erase(std::remove(text.begin(), text.end(), '\r'), text.end());
		return text;
	}

	std::string ReplaceFirst(
		std::string source,
		const std::string& expected,
		const std::string& replacement)
	{
		const size_t position = source.find(expected);
		Require(position != std::string::npos, "test replacement source should exist: " + expected);
		source.replace(position, expected.size(), replacement);
		return source;
	}

	const std::string& ValidProjectSettings()
	{
		static const std::string settings = R"YAML(settingsVersion: 1
unknownRoot: retained
graphics:
  defaultQuality: Medium
  unknownGraphics: retained
  presets:
    Ultra:
      resolutionFactor: 1.0
      fpsCap: 120
      msaaSamples: 8
      shadowQuality: High
      shadowBias: 1.25
      shadowCascadeCount: 4
      shadowCascadeResolutions: [4096, 2048, 2048, 1024]
      supportSoftShadows: true
      cloudsResolutionMultiplier: 1.0
      skyResolution: 512
      lodBias: -1
      futureField: retained
    High:
      resolutionFactor: 1.0
      fpsCap: 120
      msaaSamples: 4
      shadowQuality: High
      shadowBias: 0
      shadowCascadeCount: 4
      shadowCascadeResolutions: [2048, 2048, 1024, 1024]
      supportSoftShadows: true
      cloudsResolutionMultiplier: 0.75
      skyResolution: 256
      lodBias: 0
    Medium:
      resolutionFactor: 0.85
      fpsCap: 120
      msaaSamples: 2
      shadowQuality: Medium
      shadowBias: 0
      shadowCascadeCount: 3
      shadowCascadeResolutions: [2048, 1024, 512]
      supportSoftShadows: true
      cloudsResolutionMultiplier: 0.5
      skyResolution: 256
      lodBias: 0
    Low:
      resolutionFactor: 0.7
      fpsCap: 120
      msaaSamples: 1
      shadowQuality: Low
      shadowBias: 0
      shadowCascadeCount: 2
      shadowCascadeResolutions: [1024, 512]
      supportSoftShadows: false
      cloudsResolutionMultiplier: 0.25
      skyResolution: 128
      lodBias: +1
    VeryLow:
      resolutionFactor: 0.5
      fpsCap: 120
      msaaSamples: 1
      shadowQuality: VeryLow
      shadowBias: 0
      shadowCascadeCount: 1
      shadowCascadeResolutions: [512]
      supportSoftShadows: false
      cloudsResolutionMultiplier: 0.125
      skyResolution: 64
      lodBias: +2
)YAML";
		return settings;
	}

	std::string EditorSettings(
		const std::string& selectedQuality,
		const std::string& statsMode)
	{
		return "settingsVersion: 1\n"
			"unknownRoot: retained\n"
			"graphics:\n"
			"  selectedQuality: " + selectedQuality + "\n"
			"  statsMode: " + statsMode + "\n"
			"  futureField: retained\n";
	}

	void RequireInvalidProjectField(
		const std::string& payload,
		const std::string& fieldPath)
	{
		const ProjectGraphicsSettingsLoadResult result = ParseProjectGraphicsSettings(
			payload,
			"validation fixture");
		Require(result.m_status == EGraphicsSettingsLoadStatus::Invalid,
			"invalid project field should be rejected: " + result.m_diagnostic);
		Require(result.m_diagnostic.find(fieldPath) != std::string::npos,
			"invalid project diagnostic should identify " + fieldPath + ": " + result.m_diagnostic);
		Require(result.m_settings.m_defaultQuality == EGraphicsQuality::High,
			"invalid project settings should retain the safe built-in default");
		Require(result.m_settings.GetProfile(EGraphicsQuality::Ultra).m_msaaSamples == 8u,
			"invalid project settings should not publish partially parsed profile values");
	}

	void TestBuiltInDefaultsAndPolicies()
	{
		const GraphicsSettings defaults;
		Require(defaults.m_version == GraphicsSettingsVersion,
			"built-in settings should use the current version");
		Require(defaults.m_defaultQuality == EGraphicsQuality::High,
			"built-in project default should be High");

		const GraphicsQualityProfile& ultra = defaults.GetProfile(EGraphicsQuality::Ultra);
		Require(IsNear(ultra.m_resolutionFactor, 1.0f) && ultra.m_fpsCap == 120u &&
			ultra.m_msaaSamples == 8u,
			"Ultra should use native resolution, a 120 FPS cap, and 8x MSAA");
		Require(ultra.m_shadowQuality == ELightShadowQuality::High &&
			IsNear(ultra.m_shadowBias, 0.0f) &&
			ultra.m_shadowCascadeCount == 4u &&
			ultra.GetShadowCascadeResolution(0u) == 4096u &&
			ultra.GetShadowCascadeResolution(3u) == 1024u,
			"Ultra should expose all four planned shadow cascades");
		Require(ultra.GetShadowCascadeResolution(4u) == 0u,
			"inactive cascade access should return a safe zero extent");
		Require(ultra.m_bSupportSoftShadows &&
			IsNear(ultra.m_cloudsResolutionMultiplier, 1.0f) &&
			ultra.m_skyResolution == 512u && ultra.m_lodBias == -1,
			"Ultra should match the remaining planned defaults");

		const GraphicsQualityProfile& high = defaults.GetProfile(EGraphicsQuality::High);
		Require(IsNear(high.m_resolutionFactor, 1.0f) && high.m_fpsCap == 120u &&
			high.m_msaaSamples == 4u &&
			high.m_shadowQuality == ELightShadowQuality::High &&
			IsNear(high.m_shadowBias, 0.0f) && high.m_shadowCascadeCount == 4u &&
			high.m_shadowCascadeResolutions ==
				std::array<uint32_t, MaxShadowCascades>{ 2048u, 2048u, 1024u, 1024u } &&
			high.m_bSupportSoftShadows && IsNear(high.m_cloudsResolutionMultiplier, 0.75f) &&
			high.m_skyResolution == 256u && high.m_lodBias == 0,
			"High should match the complete planned defaults");

		const GraphicsQualityProfile& medium = defaults.GetProfile(EGraphicsQuality::Medium);
		Require(IsNear(medium.m_resolutionFactor, 0.85f) && medium.m_fpsCap == 120u &&
			medium.m_msaaSamples == 2u &&
			medium.m_shadowQuality == ELightShadowQuality::Medium &&
			IsNear(medium.m_shadowBias, 0.0f) && medium.m_shadowCascadeCount == 3u &&
			medium.m_shadowCascadeResolutions ==
				std::array<uint32_t, MaxShadowCascades>{ 2048u, 1024u, 512u, 0u } &&
			medium.m_bSupportSoftShadows && IsNear(medium.m_cloudsResolutionMultiplier, 0.5f) &&
			medium.m_skyResolution == 256u && medium.m_lodBias == 0,
			"Medium should match the complete planned defaults");

		const GraphicsQualityProfile& low = defaults.GetProfile(EGraphicsQuality::Low);
		Require(IsNear(low.m_resolutionFactor, 0.7f) && low.m_fpsCap == 120u &&
			low.m_msaaSamples == 1u &&
			low.m_shadowQuality == ELightShadowQuality::Low &&
			IsNear(low.m_shadowBias, 0.0f) && low.m_shadowCascadeCount == 2u &&
			low.m_shadowCascadeResolutions ==
				std::array<uint32_t, MaxShadowCascades>{ 1024u, 512u, 0u, 0u } &&
			!low.m_bSupportSoftShadows && IsNear(low.m_cloudsResolutionMultiplier, 0.25f) &&
			low.m_skyResolution == 128u && low.m_lodBias == 1,
			"Low should match the complete planned defaults");

		const GraphicsQualityProfile& veryLow = defaults.GetProfile(EGraphicsQuality::VeryLow);
		Require(IsNear(veryLow.m_resolutionFactor, 0.5f) &&
			veryLow.m_fpsCap == 120u &&
			veryLow.m_msaaSamples == 1u &&
			veryLow.m_shadowQuality == ELightShadowQuality::VeryLow &&
			IsNear(veryLow.m_shadowBias, 0.0f) &&
			veryLow.m_shadowCascadeCount == 1u &&
			!veryLow.m_bSupportSoftShadows &&
			IsNear(veryLow.m_cloudsResolutionMultiplier, 0.125f) &&
			veryLow.m_skyResolution == 64u && veryLow.m_lodBias == 2,
			"VeryLow should match the planned fallback values");

		const GraphicsExtent renderExtent = ResolveRenderDimensions(1919u, 1079u, 0.5f);
		Require(renderExtent.m_width == 960u && renderExtent.m_height == 540u,
			"render dimensions should scale and round to the nearest pixel");
		const GraphicsExtent minimumRenderExtent = ResolveRenderDimensions(0u, 1u, 0.25f);
		Require(minimumRenderExtent.m_width == 1u && minimumRenderExtent.m_height == 1u,
			"render dimensions should clamp to at least one pixel");
		const GraphicsExtent skyExtent = ResolveSkyExtent(veryLow);
		Require(skyExtent.m_width == 64u && skyExtent.m_height == 64u,
			"sky extent should be square and profile-controlled");
		const GraphicsExtent cloudsExtent = ResolveCloudsExtent(1920u, 1080u, ultra, 0.5f);
		Require(cloudsExtent.m_width == 540u && cloudsExtent.m_height == 540u,
			"platform cloud adjustment should apply after the profile multiplier");

		Require(ToMsaaSamples(8u) == RHI::EMsaaSamples::Samples_8 &&
			ToMsaaSamples(3u) == RHI::EMsaaSamples::Samples_1,
			"MSAA conversion should preserve supported values and safely default invalid input");
		Require(ApplyShadowQualityCap(ELightShadowQuality::High, ELightShadowQuality::Low) ==
			ELightShadowQuality::Low,
			"shadow cap should lower authored quality");
		Require(ApplyShadowQualityCap(ELightShadowQuality::Low, ELightShadowQuality::High) ==
			ELightShadowQuality::Low,
			"shadow cap should not raise authored quality");
		Require(ApplyLodBias(1u, 4u, 0u, 3u, 2) == 3u &&
			ApplyLodBias(2u, 4u, 0u, 3u, -8) == 0u &&
			ApplyLodBias(0u, 4u, 1u, 2u, -3) == 1u,
			"signed LOD bias should choose coarser/finer levels and honor authored bounds");

		Require(IsNear(LightingECS::GetShadowCascadeLevel(0u, 1u), 1.0f),
			"one active cascade should cover the full configured shadow distance");
		Require(IsNear(LightingECS::GetShadowCascadeLevel(0u, 2u), 0.2f) &&
			IsNear(LightingECS::GetShadowCascadeLevel(1u, 2u), 1.0f),
			"two active cascades should use the two coarsest stable split levels");
		Require(IsNear(LightingECS::GetShadowCascadeLevel(0u, 3u), 0.075f) &&
			IsNear(LightingECS::GetShadowCascadeLevel(1u, 3u), 0.2f) &&
			IsNear(LightingECS::GetShadowCascadeLevel(2u, 3u), 1.0f),
			"three active cascades should end at the full-distance split");
		Require(IsNear(LightingECS::GetShadowCascadeLevel(0u, 4u), 0.025f) &&
			IsNear(LightingECS::GetShadowCascadeLevel(1u, 4u), 0.075f) &&
			IsNear(LightingECS::GetShadowCascadeLevel(2u, 4u), 0.2f) &&
			IsNear(LightingECS::GetShadowCascadeLevel(3u, 4u), 1.0f),
			"four active cascades should retain the complete split schedule");
	}

	void TestProjectParsingAndValidation()
	{
		const ProjectGraphicsSettingsLoadResult parsed = ParseProjectGraphicsSettings(
			ValidProjectSettings(),
			"valid project fixture");
		Require(parsed.IsLoaded(), "valid project settings should load: " + parsed.m_diagnostic);
		Require(parsed.m_settings.m_defaultQuality == EGraphicsQuality::Medium,
			"project default quality should deserialize");
		Require(parsed.m_settings.GetProfile(EGraphicsQuality::Low).m_lodBias == 1,
			"explicit positive signed LOD bias should deserialize");
		Require(parsed.m_settings.GetProfile(EGraphicsQuality::Medium).m_fpsCap == 120u,
			"FPS cap should deserialize with the active quality profile");
		Require(IsNear(parsed.m_settings.GetProfile(EGraphicsQuality::Ultra).m_shadowBias, 1.25f),
			"shadow bias should deserialize with the quality profile");
		Require(parsed.m_settings.GetProfile(EGraphicsQuality::Medium).m_shadowCascadeResolutions[3] == 0u,
			"inactive cascade storage should be cleared");

		const ProjectGraphicsSettingsLoadResult unsupported = ParseProjectGraphicsSettings(
			ReplaceFirst(ValidProjectSettings(), "settingsVersion: 1", "settingsVersion: 2"),
			"future project fixture");
		Require(unsupported.m_status == EGraphicsSettingsLoadStatus::UnsupportedVersion,
			"future project version should be distinguished from corrupt data");
		Require(unsupported.m_settings.m_defaultQuality == EGraphicsQuality::High,
			"unsupported project version should use built-in defaults");
		const ProjectGraphicsSettingsLoadResult multipleDocuments = ParseProjectGraphicsSettings(
			ValidProjectSettings() + "---\nsettingsVersion: 1\n",
			"multiple project documents");
		Require(multipleDocuments.m_status == EGraphicsSettingsLoadStatus::Invalid &&
			multipleDocuments.m_diagnostic.find("exactly one YAML document") != std::string::npos,
			"multiple project documents should be rejected atomically");

		RequireInvalidProjectField(
			ReplaceFirst(ValidProjectSettings(), "defaultQuality: Medium", "defaultQuality: Custom"),
			"graphics.defaultQuality");
		RequireInvalidProjectField(
			ReplaceFirst(ValidProjectSettings(), "resolutionFactor: 1.0", "resolutionFactor: 0.1"),
			"graphics.presets.Ultra.resolutionFactor");
		RequireInvalidProjectField(
			ReplaceFirst(ValidProjectSettings(), "fpsCap: 120", "fpsCap: 0"),
			"graphics.presets.Ultra.fpsCap");
		RequireInvalidProjectField(
			ReplaceFirst(ValidProjectSettings(), "msaaSamples: 8", "msaaSamples: 3"),
			"graphics.presets.Ultra.msaaSamples");
		RequireInvalidProjectField(
			ReplaceFirst(ValidProjectSettings(), "shadowQuality: High", "shadowQuality: Cinematic"),
			"graphics.presets.Ultra.shadowQuality");
		RequireInvalidProjectField(
			ReplaceFirst(ValidProjectSettings(), "shadowBias: 1.25", "shadowBias: 17"),
			"graphics.presets.Ultra.shadowBias");
		RequireInvalidProjectField(
			ReplaceFirst(ValidProjectSettings(), "shadowCascadeCount: 4", "shadowCascadeCount: 5"),
			"graphics.presets.Ultra.shadowCascadeCount");
		RequireInvalidProjectField(
			ReplaceFirst(ValidProjectSettings(), "4096, 2048", "1000, 2048"),
			"graphics.presets.Ultra.shadowCascadeResolutions[0]");
		RequireInvalidProjectField(
			ReplaceFirst(ValidProjectSettings(), "supportSoftShadows: true", "supportSoftShadows: maybe"),
			"graphics.presets.Ultra.supportSoftShadows");
		RequireInvalidProjectField(
			ReplaceFirst(ValidProjectSettings(), "cloudsResolutionMultiplier: 1.0", "cloudsResolutionMultiplier: 0.01"),
			"graphics.presets.Ultra.cloudsResolutionMultiplier");
		RequireInvalidProjectField(
			ReplaceFirst(ValidProjectSettings(), "skyResolution: 512", "skyResolution: 100"),
			"graphics.presets.Ultra.skyResolution");
		RequireInvalidProjectField(
			ReplaceFirst(ValidProjectSettings(), "lodBias: -1", "lodBias: -9"),
			"graphics.presets.Ultra.lodBias");
		RequireInvalidProjectField(
			ReplaceFirst(
				ValidProjectSettings(),
				"shadowCascadeResolutions: [4096, 2048, 2048, 1024]",
				"shadowCascadeResolutions: [4096, 2048, 1024]"),
			"graphics.presets.Ultra.shadowCascadeResolutions");
	}

	void TestEditorParsingAndAppStatsMode()
	{
		const EditorGraphicsSettingsLoadResult parsed = ParseEditorGraphicsSettings(
			EditorSettings("Low", "RenderStatsAndQueries"),
			"valid editor fixture");
		Require(parsed.IsLoaded(), "valid editor settings should load: " + parsed.m_diagnostic);
		Require(parsed.m_settings.m_selectedQuality == EGraphicsQualitySelection::Low &&
			parsed.m_settings.m_statsMode == ERenderStatsMode::RenderStatsAndQueries,
			"editor quality and stats selection should deserialize");
		Require(ResolveQualitySelection(
			EGraphicsQualitySelection::ProjectDefault,
			EGraphicsQuality::Medium) == EGraphicsQuality::Medium,
			"ProjectDefault should resolve through the project document");

		const EditorGraphicsSettingsLoadResult invalid = ParseEditorGraphicsSettings(
			EditorSettings("Low", "GpuEverything"),
			"invalid editor fixture");
		Require(invalid.m_status == EGraphicsSettingsLoadStatus::Invalid &&
			invalid.m_diagnostic.find("graphics.statsMode") != std::string::npos,
			"invalid Stats mode should produce a field-qualified diagnostic");
		Require(invalid.m_settings.m_selectedQuality == EGraphicsQualitySelection::ProjectDefault &&
			invalid.m_settings.m_statsMode == ERenderStatsMode::None,
			"invalid editor settings should fall back as one atomic document");

		Require(App::SetRenderStatsMode(ERenderStatsMode::RenderStats),
			"valid live Stats mode should be accepted");
		Require(App::GetRenderStatsMode() == ERenderStatsMode::RenderStats,
			"live Stats mode should be observable through App");
		Require(!App::SetRenderStatsMode(static_cast<ERenderStatsMode>(255u)),
			"invalid live Stats mode should be rejected");
		Require(App::GetRenderStatsMode() == ERenderStatsMode::RenderStats,
			"rejected Stats mode should not mutate the active value");
		Require(App::SetRenderStatsMode(ERenderStatsMode::None),
			"test should restore the default Stats mode");
	}

	void TestWorkspaceSelectionAndEditorIsolation()
	{
		TempDirectory workspace("selection");
		const Workspace::WorkspaceContextResolveResult contextResult =
			Workspace::ResolveWorkspaceContext(workspace.Get());
		Require(contextResult.IsSuccess(),
			"legacy workspace should resolve for settings tests: " + contextResult.m_message);
		const Workspace::WorkspaceContext& context = contextResult.m_context;
		Require(context.GetProjectSettingsPath() == context.GetRoot() / "ProjectSettings.yaml",
			"project settings path should be rooted in the active workspace");
		Require(context.GetEditorSettingsPath() == context.GetCache() / "EditorSettings.yaml",
			"editor settings path should use the resolved cache directory");

		WriteText(context.GetProjectSettingsPath(), ValidProjectSettings());
		WriteText(context.GetEditorSettingsPath(), EditorSettings("Ultra", "RenderStats"));

		const GraphicsSettingsState editorState = LoadGraphicsSettings(context, true);
		Require(editorState.m_projectLoadStatus == EGraphicsSettingsLoadStatus::Loaded &&
			editorState.m_editorLoadStatus == EGraphicsSettingsLoadStatus::Loaded,
			"editor startup should load both settings documents");
		Require(editorState.m_activeQuality == EGraphicsQuality::Ultra &&
			editorState.GetActiveProfile().m_msaaSamples == 8u &&
			editorState.m_editorSettings.m_statsMode == ERenderStatsMode::RenderStats,
			"editor startup should apply the cache-side explicit quality and Stats mode");

		const GraphicsSettingsState standaloneState = LoadGraphicsSettings(context, false);
		Require(standaloneState.m_projectLoadStatus == EGraphicsSettingsLoadStatus::Loaded &&
			standaloneState.m_editorLoadStatus == EGraphicsSettingsLoadStatus::NotLoaded,
			"standalone startup should not load the cache-side editor document");
		Require(standaloneState.m_activeQuality == EGraphicsQuality::Medium &&
			standaloneState.GetActiveProfile().m_msaaSamples == 2u &&
			standaloneState.m_editorSettings.m_statsMode == ERenderStatsMode::None,
			"standalone startup should use the project default and ignore editor overrides");

		WriteText(context.GetEditorSettingsPath(), EditorSettings("Unknown", "RenderStats"));
		const GraphicsSettingsState invalidEditorState = LoadGraphicsSettings(context, true);
		Require(invalidEditorState.m_editorLoadStatus == EGraphicsSettingsLoadStatus::Invalid &&
			invalidEditorState.m_activeQuality == EGraphicsQuality::Medium &&
			invalidEditorState.m_editorSettings.m_statsMode == ERenderStatsMode::None,
			"invalid editor settings should atomically fall back to ProjectDefault and None");

		std::filesystem::remove(context.GetProjectSettingsPath());
		const GraphicsSettingsState missingProjectState = LoadGraphicsSettings(context, false);
		Require(missingProjectState.m_projectLoadStatus == EGraphicsSettingsLoadStatus::Missing &&
			missingProjectState.m_activeQuality == EGraphicsQuality::High &&
			missingProjectState.GetActiveProfile().m_msaaSamples == 4u,
			"missing project settings should produce the safe built-in High profile");
	}

	void TestGpuFrameTimeQueryRingDoesNotReuseDelayedSlots()
	{
		float milliseconds = 0.0f;
		Require(
			RHI::TryResolveGpuFrameTimeMilliseconds(
				100u, 150u, 64u, 1000000.0f, milliseconds) &&
			milliseconds == 50.0f,
			"ordered timestamp results should resolve to milliseconds");
		Require(
			!RHI::TryResolveGpuFrameTimeMilliseconds(
				200u, 100u, 64u, 1.0f, milliseconds),
			"a stale full-width end timestamp must not become an enormous unsigned duration");
		Require(
			RHI::TryResolveGpuFrameTimeMilliseconds(
				0xfffffff0u, 0x10u, 32u, 1.0f, milliseconds),
			"limited-width timestamp counters should still support a valid wrap");

		RHI::TGpuFrameTimeQueryRing<2u> ring;
		const uint32_t first = ring.Acquire();
		Require(first == 0u && ring.MarkIssued(first),
			"first query slot should transition from recording to issued");
		const uint32_t second = ring.Acquire();
		Require(second == 1u && ring.MarkIssued(second),
			"second query slot should transition from recording to issued");
		Require(
			ring.Acquire() == RHI::TGpuFrameTimeQueryRing<2u>::InvalidSlot,
			"a delayed query ring must not reset or reissue slots that are still pending");
		Require(
			ring.GetState(first) == RHI::EGpuFrameTimeQuerySlotState::Issued &&
			ring.GetState(second) == RHI::EGpuFrameTimeQuerySlotState::Issued,
			"pending slots should retain completion ownership");
		Require(ring.MarkCompleted(second),
			"a ready query result should release exactly its owning slot");
		const uint32_t reused = ring.Acquire();
		Require(reused == second,
			"only the explicitly completed slot should become reusable");
		Require(ring.CancelRecording(reused),
			"a boundary command that failed before submission should be cancellable");
		Require(
			ring.GetState(first) == RHI::EGpuFrameTimeQuerySlotState::Issued,
			"cancelling another recording must not release a delayed issued query");
	}

	void TestGraphicsSettingsRuntimeIntegrationContracts()
	{
		for (const char* rendererPath :
			{ "Content/DefaultRenderer.renderer", "Content/EditorRenderer.renderer" })
		{
			const std::string renderer = ReadRepositoryFile(rendererPath);
			Require(renderer.find(
				"- name: DepthBuffer\n  format: D32_SFLOAT_S8_UINT\n  width: RenderWidth\n  height: RenderHeight") !=
				std::string::npos,
				"scene depth must be allocated at the scaled render extent");
			Require(renderer.find(
				"- name: OverlayDepthBuffer\n  format: D32_SFLOAT_S8_UINT\n  width: ViewportWidth\n  height: ViewportHeight") !=
				std::string::npos,
				"overlay depth must remain at the native viewport extent");
			Require(renderer.find("- color: EditorOutput\n  - depthStencil: OverlayDepthBuffer") !=
				std::string::npos,
				"native editor overlays must bind the native depth target");
		}

		const std::string blitNode = ReadRepositoryFile("Runtime/FrameGraph/BlitNode.cpp");
		Require(blitNode.find("bUseFullscreenColorBlit") != std::string::npos &&
			blitNode.find("src->GetExtent() != dst->GetExtent()") != std::string::npos &&
			blitNode.find("BlitRaw(commandList, frameGraph, sceneView, src, dst)") != std::string::npos,
			"scaled color output must use a fullscreen pass that owns the native viewport");

		const std::string lighting = ReadRepositoryFile("Runtime/ECS/LightingECS.cpp");
		Require(lighting.find("lightCascadesMatrices.Num()") !=
			std::string::npos &&
			lighting.find("graphicsProfile.GetShadowCascadeResolution(k)") !=
			std::string::npos,
			"flight-local CSM targets should be allocated only for active matrices at profile resolutions");
		Require(lighting.find(
			"m_shadowMapTextures[i] = i < m_csmShadowMaps.Num() && m_csmShadowMaps[i] ?") !=
			std::string::npos,
			"inactive fixed CSM bindings must use the default shadow texture");
		Require(lighting.find("for (uint32_t candidate = NumCascades;") !=
			std::string::npos,
			"local shadow slots must remain after the fixed CSM binding range");

		const std::string shadowPrepass = ReadRepositoryFile(
			"Runtime/FrameGraph/ShadowPrepassNode.cpp");
		Require(shadowPrepass.find("for (uint32_t i = 0; i < activeCascadeCount; ++i)") !=
			std::string::npos &&
			shadowPrepass.find("GetShadowCascadeLevel(i, activeCascadeCount)") !=
			std::string::npos,
			"CSM matrices and splits must be generated only for active cascades");
		Require(shadowPrepass.find("App::GetActiveGraphicsSettings().m_shadowBias") !=
			std::string::npos &&
			shadowPrepass.find("sourceState.GetDepthBias() + shadowBias") !=
			std::string::npos,
			"profile shadow bias must affect standard casters and add to custom material bias");

		const std::string sky = ReadRepositoryFile("Runtime/FrameGraph/SkyNode.cpp");
		Require(sky.find("Settings::ResolveSkyExtent(graphicsProfile)") !=
			std::string::npos &&
			sky.find("Settings::ResolveCloudsExtent(") != std::string::npos &&
			sky.find("driver->CreateRenderTarget(\n\t\t\tcommandList") != std::string::npos,
			"resolved sky and cloud extents must feed real render-target allocation paths");
		const size_t composeBegin = sky.find(
			"BeginDebugRegion(commandList, \"Compose\"");
		const size_t overlaysBegin = sky.find(
			"BeginDebugRegion(commandList, \"Stars & Clouds\"");
		const size_t environmentBegin = sky.find(
			"BeginDebugRegion(commandList, \"Generate Environment Map\"");
		Require(composeBegin != std::string::npos &&
			overlaysBegin > composeBegin &&
			environmentBegin > overlaysBegin,
			"sky compose and overlay passes must be independently bounded");
		const std::string composeBody = sky.substr(
			composeBegin,
			overlaysBegin - composeBegin);
		const std::string overlaysBody = sky.substr(
			overlaysBegin,
			environmentBegin - overlaysBegin);
		Require(composeBody.find("-(float)target->GetExtent().y") ==
			std::string::npos &&
			composeBody.find(
				"(float)target->GetExtent().x, (float)target->GetExtent().y") !=
				std::string::npos,
			"front-culled sky compose must retain its positive target viewport");
		Require(overlaysBody.find("SetDefaultViewport(commandList)") ==
			std::string::npos &&
			overlaysBody.find("-(float)target->GetExtent().y") !=
			std::string::npos &&
			overlaysBody.find(
				"glm::vec2(target->GetExtent().x, target->GetExtent().y)") !=
				std::string::npos,
			"back-culled sky overlays must use the scaled target viewport with Vulkan orientation");

		const std::string linearDepth = ReadRepositoryFile(
			"Runtime/FrameGraph/LinearizeDepthNode.cpp");
		Require(linearDepth.find("SetDefaultViewport(commandList)") ==
			std::string::npos &&
			linearDepth.find("-(float)target->GetExtent().y") !=
			std::string::npos &&
			linearDepth.find(
				"glm::vec2(target->GetExtent().x, target->GetExtent().y)") !=
			std::string::npos,
			"linear depth must cover the scaled target used by sky and cloud depth masking");

		const std::string meshLods = ReadRepositoryFile(
			"Runtime/ECS/StaticMeshRendererECS.cpp");
		const std::string instanceLods = ReadRepositoryFile("Runtime/RHI/SceneView.cpp");
		Require(meshLods.find("Settings::ApplyLodBias(") != std::string::npos &&
			instanceLods.find("Settings::ApplyLodBias(") != std::string::npos,
			"signed LOD bias must affect both mesh and per-instance selection paths");

		const std::string renderer = ReadRepositoryFile("Runtime/RHI/Renderer.cpp");
		const size_t submissionContextsRelease = renderer.find(
			"m_submissionContexts.Clear();");
		const size_t driverRelease = renderer.find("m_driverInstance.Clear();");
		Require(submissionContextsRelease != std::string::npos &&
			driverRelease != std::string::npos &&
			submissionContextsRelease < driverRelease,
			"renderer shutdown must release flight-local GPU resources before the Vulkan driver and instance");
		const size_t beginBoundary = renderer.find("BeginGpuFrameTimeQuery(");
		const size_t updateSubmission = renderer.find(
			"bFrameSubmitsSucceeded = updateFrameRHI(chainSemaphore)");
		const size_t endBoundary = renderer.find("EndGpuFrameTimeQuery(");
		const size_t finalSubmission = renderer.find(
			"m_driverInstance->PresentFrame(frame, primaryCommandLists");
		Require(beginBoundary < updateSubmission &&
			updateSubmission < endBoundary &&
			endBoundary < finalSubmission,
			"GPU timestamps must bracket update, compute, transfer, and final graphics submissions");
		const std::string frameGraph = ReadRepositoryFile(
			"Runtime/FrameGraph/RHIFrameGraph.cpp");
		Require(frameGraph.find("GpuFrameTimeQuery") == std::string::npos,
			"frame timing must not be narrowed to a graphics framegraph snapshot");
		Require(frameGraph.find("GetSceneRenderExtent()") != std::string::npos &&
			frameGraph.find("debugDraw->GetResolvedAttachment(\"color\")") !=
				std::string::npos &&
			frameGraph.find("frameData.m_viewportSize = GetSceneRenderExtent();") !=
				std::string::npos,
			"frame shader data must use the actual resolved scene target extent");
		const std::string vulkanDriver = ReadRepositoryFile(
			"Runtime/GraphicsDriver/Vulkan/VulkanGraphicsDriver.cpp");
		Require(
			vulkanDriver.find("vkResetQueryPool(") != std::string::npos &&
			vulkanDriver.find("vkCmdResetQueryPool(") == std::string::npos &&
			vulkanDriver.find("VK_QUERY_RESULT_WITH_AVAILABILITY_BIT") !=
				std::string::npos,
			"timestamp slots must clear stale availability before reuse and poll both availability values");
		const std::string debugContext = ReadRepositoryFile(
			"Runtime/RHI/DebugContext.cpp");
		Require(debugContext.find("Settings::ResolveRenderDimensions(") ==
			std::string::npos &&
			debugContext.find("renderExtent.x") != std::string::npos &&
			debugContext.find("renderExtent.y") != std::string::npos &&
			debugContext.find("commands->SetDefaultViewport(secondaryDrawCmdList)") ==
			std::string::npos &&
			debugContext.find("-renderHeight") != std::string::npos &&
			debugContext.find("glm::vec2(renderWidth, renderHeight)") !=
			std::string::npos,
			"debug grid secondary commands must use the exact resolved target viewport and scissor");
		const std::string engineLoop = ReadRepositoryFile("Runtime/Engine/EngineLoop.cpp");
		Require(engineLoop.find("GPU queries unavailable") != std::string::npos,
			"unsupported timestamp devices must report an explicit Stats overlay state");
		Require(engineLoop.find("std::this_thread::sleep_until(cpuFrameDeadline)") !=
			std::string::npos,
			"EngineLoop must own the CPU FPS limiter");
		const std::string app = ReadRepositoryFile("Runtime/Sailor.cpp");
		Require(app.find("GetActiveGraphicsSettings().m_fpsCap") != std::string::npos &&
			app.find("EditorFrameIntervalMicro") == std::string::npos,
			"the active profile FPS cap must be passed into EngineLoop without an editor-only duplicate limiter");
	}
}

int main()
{
	try
	{
		TestBuiltInDefaultsAndPolicies();
		TestProjectParsingAndValidation();
		TestEditorParsingAndAppStatsMode();
		TestWorkspaceSelectionAndEditorIsolation();
		TestGpuFrameTimeQueryRingDoesNotReuseDelayedSlots();
		TestGraphicsSettingsRuntimeIntegrationContracts();
	}
	catch (const std::exception& exception)
	{
		std::cerr << "GraphicsSettingsTests failed: " << exception.what() << std::endl;
		return 1;
	}

	std::cout << "GraphicsSettingsTests passed." << std::endl;
	return 0;
}
