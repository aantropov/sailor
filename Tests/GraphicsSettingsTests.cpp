#include "Settings/GraphicsSettings.h"
#include "RHI/Types.h"
#include "Support/TempDirectory.h"
#include "Workspace/WorkspaceContext.h"

#include <cmath>
#include <fstream>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <tuple>
#include <yaml-cpp/yaml.h>

using namespace Sailor;
using namespace Sailor::Settings;

namespace
{
	void Require(bool condition, const std::string& message)
	{
		if (!condition) throw std::runtime_error(message);
	}

	void CheckProfile(const GraphicsQualityProfile& actual, const GraphicsQualityProfile& expected)
	{
		const auto values = [](const GraphicsQualityProfile& p)
		{
			return std::tie(p.m_resolutionFactor, p.m_fpsCap, p.m_msaaSamples, p.m_shadowQuality,
				p.m_shadowBias, p.m_shadowDistance, p.m_shadowCascadeCount, p.m_shadowCascadeResolutions,
				p.m_bSupportSoftShadows, p.m_cloudsResolutionMultiplier, p.m_bCloudsDithering,
				p.m_skyResolution, p.m_vegetationInstanceBudget, p.m_lodBias, p.m_bEnableGlobalIllumination,
				p.m_maxGiProbeStatesPerSnapshot, p.m_runtimeGIProbes);
		};
		Require(values(actual) == values(expected), "every profile value, including RuntimeGI, must be preserved");
	}

	YAML::Node ProjectDocument()
	{
		// Independent, explicit values for the existing five presets. No repository settings file is read.
		const auto high = YAML::Load(R"(
resolutionFactor: 1.0
fpsCap: 120
msaaSamples: 4
shadowQuality: High
shadowBias: 1.25
shadowDistance: 200
shadowCascadeCount: 4
shadowCascadeResolutions: [2048, 2048, 1024, 1024]
supportSoftShadows: true
cloudsResolutionMultiplier: 0.75
cloudsDithering: false
skyResolution: 256
vegetationInstanceBudget: 8192
lodBias: 0
enableGlobalIllumination: true
maxGiProbeStatesPerSnapshot: 3
runtimeGIProbes:
  version: 1
  enabled: false
  maxActiveProbes: 8192
  initialSamplesPerProbe: 16
  targetSamplesPerProbe: 64
  workerCount: 2
  maxDirtyUploadBytesPerFrame: 2097152
  spacingMultiplier: 1.0
  cpuDutyFraction: 0.25
  cpuBudgetMilliseconds: 4.0
  maxPublicationsPerSecond: 8.0
  initialPublicationCoverage: 0.125
)");
		const auto overrides = YAML::Load(R"(
Ultra:
  msaaSamples: 8
  shadowCascadeResolutions: [4096, 2048, 2048, 1024]
  cloudsResolutionMultiplier: 1.0
  skyResolution: 512
  vegetationInstanceBudget: 16384
  lodBias: -1
  maxGiProbeStatesPerSnapshot: 4
  runtimeGIProbes: {maxActiveProbes: 16384, cpuDutyFraction: 0.5}
High: {}
Medium:
  resolutionFactor: 0.85
  msaaSamples: 2
  shadowQuality: Medium
  shadowCascadeCount: 3
  shadowCascadeResolutions: [2048, 1024, 512]
  cloudsResolutionMultiplier: 0.5
  vegetationInstanceBudget: 4096
  maxGiProbeStatesPerSnapshot: 2
  runtimeGIProbes: {maxActiveProbes: 4096, cpuDutyFraction: 0.2, maxPublicationsPerSecond: 6}
Low:
  resolutionFactor: 0.7
  msaaSamples: 1
  shadowQuality: Low
  shadowCascadeCount: 2
  shadowCascadeResolutions: [1024, 512]
  supportSoftShadows: false
  cloudsResolutionMultiplier: 0.25
  skyResolution: 128
  vegetationInstanceBudget: 2048
  lodBias: 1
  maxGiProbeStatesPerSnapshot: 2
  runtimeGIProbes: {maxActiveProbes: 2048, targetSamplesPerProbe: 32, cpuDutyFraction: 0.15, maxPublicationsPerSecond: 4}
VeryLow:
  resolutionFactor: 0.5
  msaaSamples: 1
  shadowQuality: VeryLow
  shadowCascadeCount: 1
  shadowCascadeResolutions: [512]
  supportSoftShadows: false
  cloudsResolutionMultiplier: 0.125
  skyResolution: 64
  vegetationInstanceBudget: 512
  lodBias: 2
  maxGiProbeStatesPerSnapshot: 1
  runtimeGIProbes: {maxActiveProbes: 2048, targetSamplesPerProbe: 16, cpuDutyFraction: 0.1, maxPublicationsPerSecond: 2}
)");
		YAML::Node document;
		document["settingsVersion"] = 1;
		document["graphics"]["defaultQuality"] = "High";
		for (const auto& preset : overrides)
		{
			auto profile = YAML::Clone(high);
			for (const auto& field : preset.second)
			{
				const auto key = field.first.as<std::string>();
				if (key == "runtimeGIProbes")
				{
					for (const auto& budget : field.second)
						profile[key][budget.first.as<std::string>()] = budget.second;
				}
				else profile[key] = field.second;
			}
			document["graphics"]["presets"][preset.first.as<std::string>()] = profile;
		}
		return document;
	}

	YAML::Node EditorDocument()
	{
		return YAML::Load(R"(
settingsVersion: 1
graphics:
  selectedQuality: ProjectDefault
  statsMode: None
  runtimeGIProbesPreviewEnabled: false
  runtimeGIProbesBudget: Eco
  runtimeGIProbesDebugView: Lit
)");
	}

	void CheckDefaults(const GraphicsSettings& actual)
	{
		const GraphicsSettings defaults;
		Require(actual.m_version == 1 && actual.m_defaultQuality == EGraphicsQuality::High,
			"failed loading must publish built-in defaults, not partially parsed settings");
		for (uint32_t i = 0; i < NumGraphicsQualityPresets; ++i)
			CheckProfile(actual.m_presets[i], defaults.m_presets[i]);
	}

	void TestPresets()
	{
		const auto parsed = ParseProjectGraphicsSettings(YAML::Dump(ProjectDocument()));
		Require(parsed.IsLoaded(), parsed.m_diagnostic);
		CheckDefaults(parsed.m_settings);
		const GraphicsSettings defaults;
		for (uint32_t i = 0; i < NumGraphicsQualityPresets; ++i)
		{
			const auto& profile = defaults.GetProfile(static_cast<EGraphicsQuality>(i));
			Require(&profile == &defaults.m_presets[i], "profile selection must not copy or reorder presets");
			for (uint32_t cascade = 0; cascade <= MaxShadowCascades; ++cascade)
			{
				const bool active = cascade < profile.m_shadowCascadeCount;
				Require(profile.IsShadowCascadeActive(cascade) == active &&
					profile.GetShadowCascadeResolution(cascade) == (active ? profile.m_shadowCascadeResolutions[cascade] : 0),
					"inactive cascades must not expose a stale resolution");
			}
		}
		Require(&defaults.GetProfile(static_cast<EGraphicsQuality>(255)) == &defaults.GetProfile(EGraphicsQuality::High),
			"an invalid quality value retains the High fallback");
		std::cout << "GraphicsSettings five complete built-in presets passed\n";
	}

	void TestProfileDiagnostics()
	{
		struct Case { const char* field; const char* value; const char* requirement; };
		for (const auto& c : {
			Case{ "resolutionFactor", "0.2", "must be a finite number in the range [0.25, 2.0]" },
			Case{ "resolutionFactor", ".nan", "must be a finite number in the range [0.25, 2.0]" },
			Case{ "fpsCap", "1001", "must be in the range [1, 1000]" },
			Case{ "fpsCap", "-1", "must be an unsigned 32-bit integer" },
			Case{ "fpsCap", "'+120'", "must be an unsigned 32-bit integer" },
			Case{ "fpsCap", "'120x'", "must be an unsigned 32-bit integer" },
			Case{ "fpsCap", "'0x78'", "must be an unsigned 32-bit integer" },
			Case{ "fpsCap", "4294967296", "must be an unsigned 32-bit integer" },
			Case{ "fpsCap", "[]", "is required and must be a scalar" },
			Case{ "msaaSamples", "3", "must be one of 1, 2, 4, or 8" },
			Case{ "shadowQuality", "Ultra", "must be one of High, Medium, Low, or VeryLow" },
			Case{ "shadowBias", "17", "must be a finite number in the range [-16, 16]" },
			Case{ "shadowDistance", "0", "must be a finite number in the range [1, 10000]" },
			Case{ "shadowCascadeCount", "5", "must be in the range [1, 4]" },
			Case{ "shadowCascadeResolutions", "[1024]", "must be a sequence whose length matches shadowCascadeCount" },
			Case{ "cloudsResolutionMultiplier", "0.01", "must be a finite number in the range [0.0625, 2.0]" },
			Case{ "skyResolution", "127", "must be a power of two in the range [32, 8192]" },
			Case{ "lodBias", "9", "must be in the range [-8, 8]" },
			Case{ "lodBias", "2147483648", "must be a signed 32-bit integer" },
			Case{ "vegetationInstanceBudget", "1048577", "must be in the range [0, 1048576]" },
			Case{ "vegetationInstanceBudget", "{}", "must be an unsigned 32-bit integer" },
			Case{ "vegetationInstanceBudget", "'1x'", "must be an unsigned 32-bit integer" },
			Case{ "maxGiProbeStatesPerSnapshot", "17", "must be in the range [0, 16]" } })
		{
			auto document = ProjectDocument();
			document["graphics"]["defaultQuality"] = "Ultra";
			document["graphics"]["presets"]["Ultra"]["fpsCap"] = 75;
			document["graphics"]["presets"]["High"][c.field] = YAML::Load(c.value);
			const auto result = ParseProjectGraphicsSettings(YAML::Dump(document), "fixture");
			Require(result.m_status == EGraphicsSettingsLoadStatus::Invalid && result.m_diagnostic ==
				"fixture is invalid: field 'graphics.presets.High." + std::string(c.field) + "' " + c.requirement + ".",
				"field diagnostics must retain their path and requirement: " + result.m_diagnostic);
			CheckDefaults(result.m_settings);
		}
		for (const auto& field : ProjectDocument()["graphics"]["presets"]["High"])
		{
			const auto name = field.first.as<std::string>();
			auto document = ProjectDocument();
			document["graphics"]["presets"]["High"].remove(name);
			const auto result = ParseProjectGraphicsSettings(YAML::Dump(document), "fixture");
			if (name == "vegetationInstanceBudget")
			{
				Require(result.IsLoaded(), result.m_diagnostic);
				CheckDefaults(result.m_settings);
			}
			else
			{
				Require(result.m_status == EGraphicsSettingsLoadStatus::Invalid &&
					result.m_diagnostic.find("graphics.presets.High." + name) != std::string::npos,
					"every required field must identify its missing value: " + name);
				CheckDefaults(result.m_settings);
			}
		}
		auto signedValue = ProjectDocument();
		signedValue["graphics"]["presets"]["High"]["lodBias"] = "+2";
		signedValue["graphics"]["presets"]["High"]["vegetationInstanceBudget"] = 0;
		const auto accepted = ParseProjectGraphicsSettings(YAML::Dump(signedValue));
		Require(accepted.IsLoaded() && accepted.m_settings.GetProfile(EGraphicsQuality::High).m_lodBias == 2 &&
			accepted.m_settings.GetProfile(EGraphicsQuality::High).m_vegetationInstanceBudget == 0,
			"signed plus and a zero optional vegetation budget must remain legal");
		std::cout << "GraphicsSettings profile diagnostics, strict integers and atomic publication passed\n";
	}

	void TestDocumentValidation()
	{
		for (const char* payload : { "", "[]", "---\na: 1\n---\nb: 2", "graphics: [", "settingsVersion: 1\nsettingsVersion: 1" })
		{
			const auto result = ParseProjectGraphicsSettings(payload);
			Require(result.m_status == EGraphicsSettingsLoadStatus::Invalid && !result.m_diagnostic.empty(),
				"malformed/ambiguous documents must fail with diagnostics");
			CheckDefaults(result.m_settings);
		}
		auto document = ProjectDocument();
		document["settingsVersion"] = 2;
		const auto version = ParseProjectGraphicsSettings(YAML::Dump(document));
		Require(version.m_status == EGraphicsSettingsLoadStatus::UnsupportedVersion, "only version1 is accepted");
		CheckDefaults(version.m_settings);
		for (const char* field : { "version", "enabled", "maxActiveProbes", "initialSamplesPerProbe", "targetSamplesPerProbe",
			"workerCount", "maxDirtyUploadBytesPerFrame", "spacingMultiplier", "cpuDutyFraction", "cpuBudgetMilliseconds",
			"maxPublicationsPerSecond", "initialPublicationCoverage" })
		{
			document = ProjectDocument();
			document["graphics"]["presets"]["High"]["runtimeGIProbes"].remove(field);
			const auto result = ParseProjectGraphicsSettings(YAML::Dump(document));
			Require(result.m_status == EGraphicsSettingsLoadStatus::Invalid &&
				result.m_diagnostic.find(std::string("runtimeGIProbes.") + field) != std::string::npos,
				"RuntimeGI fields remain required even when disabled");
		}
		document = ProjectDocument();
		document["graphics"]["presets"]["High"]["runtimeGIProbes"]["initialSamplesPerProbe"] = 65;
		const auto invalidBudget = ParseProjectGraphicsSettings(YAML::Dump(document));
		Require(invalidBudget.m_status == EGraphicsSettingsLoadStatus::Invalid &&
			invalidBudget.m_diagnostic.find("16 <= initial <= target <= 65536") != std::string::npos,
			"RuntimeGI quality validation must retain relationships between fields");
		CheckDefaults(invalidBudget.m_settings);
		std::cout << "GraphicsSettings document and RuntimeGI validation passed\n";
	}

	void TestConversionDiagnostics()
	{
		for (const char* name : { "resolutionFactor", "shadowBias", "shadowDistance", "cloudsResolutionMultiplier",
			"supportSoftShadows", "cloudsDithering", "enableGlobalIllumination" })
		{
			auto document = ProjectDocument();
			document["graphics"]["presets"]["High"][name] = "not-a-value";
			const auto result = ParseProjectGraphicsSettings(YAML::Dump(document), "fixture");
			Require(result.m_status == EGraphicsSettingsLoadStatus::Invalid &&
				result.m_diagnostic.find("fixture is invalid: field 'graphics.presets.High." + std::string(name) + "' must be ") == 0 &&
				result.m_diagnostic.find("YAML detail:") != std::string::npos,
				"typed conversions must preserve source/path and YAML conversion detail");
			CheckDefaults(result.m_settings);
		}
		for (const char* value : { "{}", "31", "8193", "1023", "'1024x'", "'+1024'", "4294967296" })
		{
			auto document = ProjectDocument();
			document["graphics"]["presets"]["High"]["shadowCascadeResolutions"][2] = YAML::Load(value);
			const auto result = ParseProjectGraphicsSettings(YAML::Dump(document), "fixture");
			const std::string requirement = std::string(value) == "{}" ? "must be an unsigned integer" :
				"must be a power of two in the range [32, 8192]";
			Require(result.m_status == EGraphicsSettingsLoadStatus::Invalid && result.m_diagnostic ==
				"fixture is invalid: field 'graphics.presets.High.shadowCascadeResolutions[2]' " + requirement + ".",
				"cascade diagnostics must distinguish structure from invalid numeric values");
		}
		for (const auto& field : EditorDocument()["graphics"])
		{
			auto document = EditorDocument();
			const auto name = field.first.as<std::string>();
			document["graphics"]["selectedQuality"] = "Ultra";
			document["graphics"].remove(name);
			const auto result = ParseEditorGraphicsSettings(YAML::Dump(document), "editor");
			Require(result.m_status == EGraphicsSettingsLoadStatus::Invalid &&
				result.m_diagnostic.find("graphics." + name) != std::string::npos &&
				result.m_settings.m_selectedQuality == EGraphicsQualitySelection::ProjectDefault &&
				result.m_settings.m_statsMode == ERenderStatsMode::None && !result.m_settings.m_bRuntimeGIProbesPreviewEnabled,
				"a missing editor field must not publish earlier parsed changes");
		}
		std::cout << "GraphicsSettings conversion, cascade and editor diagnostics passed\n";
	}

	void WriteDocument(const std::filesystem::path& path, const YAML::Node& document)
	{
		std::ofstream stream(path);
		stream << YAML::Dump(document);
		Require(stream.good(), "fixture settings must be written");
	}

	void TestLoadingAndSelection()
	{
		Tests::TempDirectory temporary("graphics-settings");
		const auto resolved = Workspace::ResolveWorkspaceContext(temporary.Get());
		Require(resolved.IsSuccess(), resolved.m_message);
		const auto& workspace = resolved.m_context;
		auto state = LoadGraphicsSettings(workspace, true);
		Require(state.m_projectLoadStatus == EGraphicsSettingsLoadStatus::Missing &&
			state.m_editorLoadStatus == EGraphicsSettingsLoadStatus::Missing && state.m_activeQuality == EGraphicsQuality::High,
			"missing settings must use the built-in project/editor defaults");
		CheckDefaults(state.m_projectSettings);
		Require(LoadProjectGraphicsSettings(temporary.Get()).m_status == EGraphicsSettingsLoadStatus::IoFailure,
			"an existing directory is not a missing settings file");
		auto project = ProjectDocument();
		auto editor = EditorDocument();
		for (auto quality : magic_enum::enum_values<EGraphicsQuality>())
		{
			project["graphics"]["defaultQuality"] = std::string(magic_enum::enum_name(quality));
			WriteDocument(workspace.GetProjectSettingsPath(), project);
			for (auto selection : magic_enum::enum_values<EGraphicsQualitySelection>())
			{
				editor["graphics"]["selectedQuality"] = std::string(magic_enum::enum_name(selection));
				WriteDocument(workspace.GetEditorSettingsPath(), editor);
				state = LoadGraphicsSettings(workspace, true);
				const auto expected = selection == EGraphicsQualitySelection::ProjectDefault ? quality :
					*magic_enum::enum_cast<EGraphicsQuality>(magic_enum::enum_name(selection));
				Require(state.m_projectLoadStatus == EGraphicsSettingsLoadStatus::Loaded &&
					state.m_editorLoadStatus == EGraphicsSettingsLoadStatus::Loaded && state.m_activeQuality == expected,
					"editor selection must override only when explicitly selected");
				CheckProfile(state.GetActiveProfile(), state.m_projectSettings.GetProfile(expected));
				const auto game = LoadGraphicsSettings(workspace, false);
				Require(game.m_activeQuality == quality && game.m_editorLoadStatus == EGraphicsSettingsLoadStatus::NotLoaded,
					"game mode must ignore editor settings");
			}
		}
		editor["graphics"]["selectedQuality"] = "Unknown";
		WriteDocument(workspace.GetEditorSettingsPath(), editor);
		state = LoadGraphicsSettings(workspace, true);
		Require(state.m_editorLoadStatus == EGraphicsSettingsLoadStatus::Invalid && state.m_activeQuality == EGraphicsQuality::VeryLow,
			"an invalid editor selection must retain the valid project default");
		std::cout << "GraphicsSettings file status and project/editor precedence passed\n";
	}

	void TestValuePolicies()
	{
		const auto extent = ResolveRenderDimensions(101, 57, 0.5f);
		Require(extent.m_width == 51 && extent.m_height == 29, "render dimensions must round halves up");
		for (float invalid : { 0.0f, -1.0f, std::numeric_limits<float>::infinity(), std::numeric_limits<float>::quiet_NaN() })
			Require(ResolveRenderDimensions(1920, 1080, invalid).m_width == 1, "invalid scales use one pixel");
		Require(ResolveRenderDimensions(UINT32_MAX, 0, 2).m_width == UINT32_MAX &&
			ResolveRenderDimensions(UINT32_MAX, 0, 2).m_height == 1, "dimensions clamp at both integer bounds");
		GraphicsQualityProfile profile;
		profile.m_skyResolution = 512;
		profile.m_cloudsResolutionMultiplier = 0.5f;
		Require(ResolveSkyExtent(profile).m_height == 512 && ResolveCloudsExtent(1920, 1080, profile, 0.5f).m_width == 270,
			"sky/cloud dimensions must retain independent resolution policies");
		for (uint32_t samples : { 1u, 2u, 4u, 8u })
			Require(static_cast<uint32_t>(ToMsaaSamples(samples)) == samples, "legal MSAA samples must remain unchanged");
		Require(ToMsaaSamples(3) == RHI::EMsaaSamples::Samples_1, "unsupported MSAA keeps the existing 1x fallback");
		Require(ApplyLodBias(2, 5, 1, 3, -8) == 1 && ApplyLodBias(2, 5, 1, 3, 8) == 3 &&
			ApplyLodBias(2, 0, 0, 3, 1) == 0 && ApplyLodBias(99, 5, 3, 1, -1) == 3,
			"LOD bias must respect authored/available ranges, including empty and inverted inputs");
		Require(ApplyShadowQualityCap(ELightShadowQuality::High, ELightShadowQuality::Low) == ELightShadowQuality::Low &&
			ApplyShadowQualityCap(ELightShadowQuality::VeryLow, ELightShadowQuality::High) == ELightShadowQuality::VeryLow,
			"quality caps must not improve authored shadow quality");
		std::cout << "GraphicsSettings extent, MSAA, LOD and shadow policies passed\n";
	}

	void TestEditorWriterOutput(const std::filesystem::path& projectPath, const std::filesystem::path& editorPath)
	{
		const auto project = LoadProjectGraphicsSettings(projectPath);
		Require(project.IsLoaded(), project.m_diagnostic);
		CheckDefaults(project.m_settings);
		const auto editor = LoadEditorGraphicsSettings(editorPath);
		Require(editor.IsLoaded() && editor.m_settings.m_version == 1 &&
			editor.m_settings.m_selectedQuality == EGraphicsQualitySelection::Medium &&
			editor.m_settings.m_statsMode == ERenderStatsMode::RenderStatsAndQueries &&
			editor.m_settings.m_bRuntimeGIProbesPreviewEnabled &&
			editor.m_settings.m_runtimeGIProbesBudget == ERuntimeGIProbesEditorBudget::Balanced &&
			editor.m_settings.m_runtimeGIProbesDebugView == ERuntimeGIProbesEditorDebugView::Probes,
			"native loading must retain all values written by the existing editor codec");
		std::cout << "GraphicsSettings actual editor writer/native reader parity passed\n";
	}
}

int main(int argc, char** argv)
{
	try
	{
		TestPresets();
		TestProfileDiagnostics();
		TestDocumentValidation();
		TestConversionDiagnostics();
		TestLoadingAndSelection();
		TestValuePolicies();
		if (argc == 3) TestEditorWriterOutput(argv[1], argv[2]);
		else Require(argc == 1, "optional arguments are the editor-written project and editor YAML paths");
		return 0;
	}
	catch (const std::exception& error)
	{
		std::cerr << error.what() << '\n';
		return 1;
	}
}
