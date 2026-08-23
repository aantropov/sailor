#include "Sailor.h"
#include "ECS/LightingECS.h"
#include "Settings/GraphicsSettings.h"
#include "Workspace/WorkspaceContext.h"
#include "RHI/GpuFrameTimeQueryRing.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <utility>

using namespace Sailor;
using namespace Sailor::Settings;

namespace
{
	const char* g_currentTestName = "startup";

	[[noreturn]] void ReportTermination() noexcept
	{
		std::cerr << "GraphicsSettingsTests terminated while running: " <<
			g_currentTestName << std::endl;

		try
		{
			if (const std::exception_ptr exception = std::current_exception())
			{
				std::rethrow_exception(exception);
			}
		}
		catch (const std::exception& exception)
		{
			std::cerr << "Termination exception: " << exception.what() << std::endl;
		}
		catch (...)
		{
			std::cerr << "Termination exception: unknown" << std::endl;
		}

		std::_Exit(2);
	}

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

	template<typename TTest>
	void RunTest(const char* name, TTest&& test)
	{
		const char* previousTestName = g_currentTestName;
		g_currentTestName = name;
		std::cout << "[ RUN      ] " << name << std::endl;
		test();
		std::cout << "[       OK ] " << name << std::endl;
		g_currentTestName = previousTestName;
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

	std::string EraseLinesContaining(
		std::string source,
		const std::string& marker)
	{
		size_t markerPosition = source.find(marker);
		while (markerPosition != std::string::npos)
		{
			const size_t lineStart = source.rfind('\n', markerPosition);
			const size_t eraseStart = lineStart == std::string::npos
				? 0u
				: lineStart + 1u;
			const size_t lineEnd = source.find('\n', markerPosition);
			const size_t eraseEnd = lineEnd == std::string::npos
				? source.size()
				: lineEnd + 1u;
			source.erase(eraseStart, eraseEnd - eraseStart);
			markerPosition = source.find(marker, eraseStart);
		}
		return source;
	}

	const std::string& ValidProjectSettings()
	{
		static const std::string settings = R"YAML(settingsVersion: 2
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
      maxGiProbeStatesPerSnapshot: 4
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
      maxGiProbeStatesPerSnapshot: 3
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
      vegetationInstanceBudget: 12345
      lodBias: 0
      maxGiProbeStatesPerSnapshot: 2
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
      maxGiProbeStatesPerSnapshot: 2
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
      maxGiProbeStatesPerSnapshot: 1
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
		const std::string testName = "ProjectParsing.Invalid." + fieldPath;
		RunTest(testName.c_str(), [&]()
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
			});
	}

	void TestBuiltInDefaultsAndPolicies()
	{
		const GraphicsSettings defaults;
		Require(defaults.m_version == ProjectGraphicsSettingsVersion,
			"built-in settings should use the current version");
		Require(EditorGraphicsSettings{}.m_version ==
			EditorGraphicsSettingsVersion,
			"cache-side Editor settings should retain their independent version");
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
			ultra.m_skyResolution == 512u &&
			ultra.m_vegetationInstanceBudget == 65536u && ultra.m_lodBias == -1 &&
			ultra.m_maxGiProbeStatesPerSnapshot == 4u,
			"Ultra should match the remaining planned defaults");

		const GraphicsQualityProfile& high = defaults.GetProfile(EGraphicsQuality::High);
		Require(IsNear(high.m_resolutionFactor, 1.0f) && high.m_fpsCap == 120u &&
			high.m_msaaSamples == 4u &&
			high.m_shadowQuality == ELightShadowQuality::High &&
			IsNear(high.m_shadowBias, 0.0f) && high.m_shadowCascadeCount == 4u &&
			high.m_shadowCascadeResolutions ==
				std::array<uint32_t, MaxShadowCascades>{ 2048u, 2048u, 1024u, 1024u } &&
			high.m_bSupportSoftShadows && IsNear(high.m_cloudsResolutionMultiplier, 0.75f) &&
			high.m_skyResolution == 256u &&
			high.m_vegetationInstanceBudget == 32768u && high.m_lodBias == 0 &&
			high.m_maxGiProbeStatesPerSnapshot == 3u,
			"High should match the complete planned defaults");

		const GraphicsQualityProfile& medium = defaults.GetProfile(EGraphicsQuality::Medium);
		Require(IsNear(medium.m_resolutionFactor, 0.85f) && medium.m_fpsCap == 120u &&
			medium.m_msaaSamples == 2u &&
			medium.m_shadowQuality == ELightShadowQuality::Medium &&
			IsNear(medium.m_shadowBias, 0.0f) && medium.m_shadowCascadeCount == 3u &&
			medium.m_shadowCascadeResolutions ==
				std::array<uint32_t, MaxShadowCascades>{ 2048u, 1024u, 512u, 0u } &&
			medium.m_bSupportSoftShadows && IsNear(medium.m_cloudsResolutionMultiplier, 0.5f) &&
			medium.m_skyResolution == 256u &&
			medium.m_vegetationInstanceBudget == 16384u && medium.m_lodBias == 0 &&
			medium.m_maxGiProbeStatesPerSnapshot == 2u,
			"Medium should match the complete planned defaults");

		const GraphicsQualityProfile& low = defaults.GetProfile(EGraphicsQuality::Low);
		Require(IsNear(low.m_resolutionFactor, 0.7f) && low.m_fpsCap == 120u &&
			low.m_msaaSamples == 1u &&
			low.m_shadowQuality == ELightShadowQuality::Low &&
			IsNear(low.m_shadowBias, 0.0f) && low.m_shadowCascadeCount == 2u &&
			low.m_shadowCascadeResolutions ==
				std::array<uint32_t, MaxShadowCascades>{ 1024u, 512u, 0u, 0u } &&
			!low.m_bSupportSoftShadows && IsNear(low.m_cloudsResolutionMultiplier, 0.25f) &&
			low.m_skyResolution == 128u &&
			low.m_vegetationInstanceBudget == 8192u && low.m_lodBias == 1 &&
			low.m_maxGiProbeStatesPerSnapshot == 2u,
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
			veryLow.m_skyResolution == 64u &&
			veryLow.m_vegetationInstanceBudget == 2048u && veryLow.m_lodBias == 2 &&
			veryLow.m_maxGiProbeStatesPerSnapshot == 1u,
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
		RunTest("ProjectParsing.ValidDocument", []()
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
				Require(parsed.m_settings.GetProfile(EGraphicsQuality::Medium).m_vegetationInstanceBudget == 12345u &&
					parsed.m_settings.GetProfile(EGraphicsQuality::Ultra).m_vegetationInstanceBudget == 65536u,
					"vegetation budgets should deserialize when present and use quality defaults when omitted");
				Require(parsed.m_settings.GetProfile(EGraphicsQuality::Ultra).m_maxGiProbeStatesPerSnapshot == 4u,
					"GI probe-state snapshot budget should deserialize per quality profile");
				Require(IsNear(parsed.m_settings.GetProfile(EGraphicsQuality::Ultra).m_shadowBias, 1.25f),
					"shadow bias should deserialize with the quality profile");
				Require(parsed.m_settings.GetProfile(EGraphicsQuality::Medium).m_shadowCascadeResolutions[3] == 0u,
					"inactive cascade storage should be cleared");
			});

		RunTest("ProjectParsing.LegacyGiBudgetMigration", []()
			{
				std::string legacy = ReplaceFirst(
					ValidProjectSettings(),
					"settingsVersion: 2",
					"settingsVersion: 1");
				legacy = EraseLinesContaining(
					std::move(legacy),
					"maxGiProbeStatesPerSnapshot:");
				const ProjectGraphicsSettingsLoadResult parsed =
					ParseProjectGraphicsSettings(
						legacy,
						"legacy project fixture");
				Require(parsed.IsLoaded() &&
					parsed.m_settings.m_version ==
						ProjectGraphicsSettingsVersion &&
					parsed.m_settings.GetProfile(
						EGraphicsQuality::Ultra).m_maxGiProbeStatesPerSnapshot == 4u &&
					parsed.m_settings.GetProfile(
						EGraphicsQuality::VeryLow).m_maxGiProbeStatesPerSnapshot == 1u,
					"version-1 project settings must migrate with per-quality GI defaults");
			});

		RunTest("ProjectParsing.UnsupportedVersion", []()
			{
				const ProjectGraphicsSettingsLoadResult unsupported = ParseProjectGraphicsSettings(
					ReplaceFirst(ValidProjectSettings(), "settingsVersion: 2", "settingsVersion: 3"),
					"future project fixture");
				Require(unsupported.m_status == EGraphicsSettingsLoadStatus::UnsupportedVersion,
					"future project version should be distinguished from corrupt data");
				Require(unsupported.m_settings.m_defaultQuality == EGraphicsQuality::High,
					"unsupported project version should use built-in defaults");
			});

		RunTest("ProjectParsing.MultipleDocuments", []()
			{
				const ProjectGraphicsSettingsLoadResult multipleDocuments = ParseProjectGraphicsSettings(
					ValidProjectSettings() + "---\nsettingsVersion: 2\n",
					"multiple project documents");
				Require(multipleDocuments.m_status == EGraphicsSettingsLoadStatus::Invalid &&
					multipleDocuments.m_diagnostic.find("exactly one YAML document") != std::string::npos,
					"multiple project documents should be rejected atomically");
			});

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
			ReplaceFirst(ValidProjectSettings(), "vegetationInstanceBudget: 12345", "vegetationInstanceBudget: 1048577"),
			"graphics.presets.Medium.vegetationInstanceBudget");
		RequireInvalidProjectField(
			ReplaceFirst(ValidProjectSettings(), "lodBias: -1", "lodBias: -9"),
			"graphics.presets.Ultra.lodBias");
		RequireInvalidProjectField(
			ReplaceFirst(
				ValidProjectSettings(),
				"maxGiProbeStatesPerSnapshot: 4",
				"maxGiProbeStatesPerSnapshot: 17"),
			"graphics.presets.Ultra.maxGiProbeStatesPerSnapshot");
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
}

int main()
{
	std::set_terminate(ReportTermination);

	try
	{
		RunTest("BuiltInDefaultsAndPolicies", TestBuiltInDefaultsAndPolicies);
		RunTest("ProjectParsingAndValidation", TestProjectParsingAndValidation);
		RunTest("EditorParsingAndAppStatsMode", TestEditorParsingAndAppStatsMode);
		RunTest("WorkspaceSelectionAndEditorIsolation", TestWorkspaceSelectionAndEditorIsolation);
		RunTest("GpuFrameTimeQueryRingDoesNotReuseDelayedSlots", TestGpuFrameTimeQueryRingDoesNotReuseDelayedSlots);
	}
	catch (const std::exception& exception)
	{
		std::cerr << "GraphicsSettingsTests failed: " << exception.what() << std::endl;
		return 1;
	}

	std::cout << "GraphicsSettingsTests passed." << std::endl;
	return 0;
}
