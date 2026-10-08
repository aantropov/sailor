#include "Settings/GraphicsSettingsReflection.h"

#include "Workspace/WorkspaceContext.h"
#include "Workspace/WorkspacePathEncoding.h"

#include <fstream>
#include <sstream>

using namespace Sailor;
using namespace Sailor::Settings;

namespace
{
	std::string Quote(const std::string& value)
	{
		return "'" + value + "'";
	}

	void RequireSetting(bool condition, std::string_view field, std::string_view requirement)
	{
		if (!condition)
		{
			throw YAML::RepresentationException(YAML::Mark::null_mark(), std::string(field).append(": ").append(requirement));
		}
	}

	void RequireMap(const YAML::Node& node)
	{
		const auto validation = Utils::ValidateYamlMap(node);
		RequireSetting(validation.IsValid(), validation.m_fieldName, "expected a map with unique field names");
	}

	bool IsTextureResolution(uint32_t value)
	{
		return value >= 32u && value <= 8192u && (value & (value - 1u)) == 0u;
	}

	void ValidateProfile(const GraphicsQualityProfile& profile)
	{
		RequireSetting(profile.m_resolutionFactor >= 0.25f && profile.m_resolutionFactor <= 2.0f,
			"resolutionFactor", "must be in the range [0.25, 2.0]");
		RequireSetting(profile.m_fpsCap >= 1u && profile.m_fpsCap <= 1000u, "fpsCap", "must be in the range [1, 1000]");
		RequireSetting(profile.m_msaaSamples == 1u || profile.m_msaaSamples == 2u ||
			profile.m_msaaSamples == 4u || profile.m_msaaSamples == 8u, "msaaSamples", "must be 1, 2, 4 or 8");
		RequireSetting(profile.m_shadowBias >= -16.0f && profile.m_shadowBias <= 16.0f,
			"shadowBias", "must be in the range [-16, 16]");
		RequireSetting(profile.m_shadowDistance >= 1.0f && profile.m_shadowDistance <= 10000.0f,
			"shadowDistance", "must be in the range [1, 10000]");
		RequireSetting(profile.m_cloudsResolutionMultiplier >= 0.0625f && profile.m_cloudsResolutionMultiplier <= 2.0f,
			"cloudsResolutionMultiplier", "must be in the range [0.0625, 2.0]");
		RequireSetting(IsTextureResolution(profile.m_skyResolution), "skyResolution", "must be a power of two in [32, 8192]");
		RequireSetting(profile.m_lodBias >= -8 && profile.m_lodBias <= 8, "lodBias", "must be in the range [-8, 8]");
		RequireSetting(profile.m_vegetationInstanceBudget <= 1048576u, "vegetationInstanceBudget", "must not exceed 1048576");
		RequireSetting(profile.m_maxGiProbeStatesPerSnapshot <= 16u, "maxGiProbeStatesPerSnapshot", "must not exceed 16");

		std::string diagnostic;
		if (!profile.m_runtimeGIProbes.Validate(diagnostic))
		{
			RequireSetting(false, "runtimeGIProbes", diagnostic);
		}
	}

	enum class EFileReadStatus : uint8_t
	{
		Loaded,
		Missing,
		IoFailure
	};

	EFileReadStatus ReadTextFile(
		const std::filesystem::path& path,
		std::string& outPayload,
		std::string& outDiagnostic)
	{
		outPayload.clear();
		outDiagnostic.clear();
		const std::string pathString = Workspace::PathToUtf8(path);
		std::error_code fileError;
		const bool bExists = std::filesystem::exists(path, fileError);
		if (fileError)
		{
			outDiagnostic = "Cannot inspect settings file " + Quote(pathString) + ": " + fileError.message() + ".";
			return EFileReadStatus::IoFailure;
		}
		if (!bExists)
		{
			outDiagnostic = "Settings file " + Quote(pathString) + " is missing; using built-in defaults.";
			return EFileReadStatus::Missing;
		}

		if (!std::filesystem::is_regular_file(path, fileError) || fileError)
		{
			outDiagnostic = "Settings path " + Quote(pathString) + " is not a readable regular file";
			if (fileError)
			{
				outDiagnostic += ": " + fileError.message();
			}
			outDiagnostic += ".";
			return EFileReadStatus::IoFailure;
		}

		std::ifstream input(path, std::ios::binary);
		if (!input.is_open())
		{
			outDiagnostic = "Cannot open settings file " + Quote(pathString) + ".";
			return EFileReadStatus::IoFailure;
		}

		std::ostringstream buffer;
		buffer << input.rdbuf();
		if (input.bad())
		{
			outDiagnostic = "Cannot read settings file " + Quote(pathString) + ".";
			return EFileReadStatus::IoFailure;
		}
		outPayload = buffer.str();
		return EFileReadStatus::Loaded;
	}

	template<typename TResult>
	TResult ParseSettings(const std::string& payload, std::string_view source)
	{
		TResult result;
		YAML::Node document;
		std::string diagnostic;
		if (!Utils::TryLoadSingleYamlDocument(payload, document, diagnostic))
		{
			result.m_status = EGraphicsSettingsLoadStatus::Invalid;
			result.m_diagnostic = std::string(source) + " is invalid YAML: " + diagnostic;
			return result;
		}

		std::string path;
		if (!External::GuardYamlExceptions([&]()
			{
				RequireMap(document);
				auto parsed = result.m_settings;
				path = "settingsVersion";
				parsed.m_version = document["settingsVersion"].template as<uint32_t>();
				constexpr uint32_t version = std::is_same_v<TResult, ProjectGraphicsSettingsLoadResult>
					? ProjectGraphicsSettingsVersion : EditorGraphicsSettingsVersion;
				if (parsed.m_version != version)
				{
					result.m_status = EGraphicsSettingsLoadStatus::UnsupportedVersion;
					result.m_diagnostic = std::string(source) + " has unsupported settingsVersion " +
						std::to_string(parsed.m_version) + "; expected " + std::to_string(version) + ".";
					return;
				}

				path = "graphics";
				const auto graphics = document["graphics"];
				RequireMap(graphics);
				if constexpr (std::is_same_v<TResult, ProjectGraphicsSettingsLoadResult>)
				{
					path = "graphics.defaultQuality";
					parsed.m_defaultQuality = graphics["defaultQuality"].template as<EGraphicsQuality>();
					path = "graphics.maxFramesInFlight";
					if (const auto frames = graphics["maxFramesInFlight"])
					{
						parsed.m_maxFramesInFlight = frames.template as<uint32_t>();
					}
					RequireSetting(parsed.m_maxFramesInFlight >= 1u && parsed.m_maxFramesInFlight <= 3u,
						"maxFramesInFlight", "must be 1, 2 or 3");
					path = "graphics.presets";
					const auto presets = graphics["presets"];
					RequireMap(presets);
					for (const auto quality : magic_enum::enum_values<EGraphicsQuality>())
					{
						const std::string_view name = magic_enum::enum_name(quality);
						path = "graphics.presets.";
						path += name;
						YAML::convert<GraphicsQualityProfile>::decode(presets[name],
							parsed.m_presets[static_cast<size_t>(quality)]);
					}
				}
				else
				{
					DeserializeReflected(graphics, parsed);
				}

				result.m_settings = std::move(parsed);
				result.m_status = EGraphicsSettingsLoadStatus::Loaded;
				result.m_diagnostic = std::string("Loaded ").append(source).append(".");
			}, diagnostic))
		{
			result.m_status = EGraphicsSettingsLoadStatus::Invalid;
			result.m_diagnostic = std::string(source) + " is invalid at " + path + ": " + diagnostic;
		}
		return result;
	}
}

YAML::Node YAML::convert<GraphicsQualityProfile>::encode(const GraphicsQualityProfile& profile)
{
	auto node = SerializeReflected(profile);
	YAML::Node cascades(YAML::NodeType::Sequence);
	for (uint32_t index = 0; profile.IsShadowCascadeActive(index); ++index)
	{
		cascades.push_back(profile.m_shadowCascadeResolutions[index]);
	}
	node["shadowCascadeResolutions"] = cascades;
	return node;
}

bool YAML::convert<GraphicsQualityProfile>::decode(const Node& node, GraphicsQualityProfile& profile)
{
	DeserializeReflected(node, profile);
	RequireSetting(profile.m_shadowCascadeCount >= 1u && profile.m_shadowCascadeCount <= MaxShadowCascades,
		"shadowCascadeCount", "must be in the range [1, 4]");
	const auto cascades = node["shadowCascadeResolutions"];
	RequireSetting(cascades.IsSequence() && cascades.size() == profile.m_shadowCascadeCount,
		"shadowCascadeResolutions", "length must match shadowCascadeCount");
	profile.m_shadowCascadeResolutions.fill(0u);
	for (uint32_t index = 0; index < profile.m_shadowCascadeCount; ++index)
	{
		const std::string field = "shadowCascadeResolutions[" + std::to_string(index) + "]";
		uint32_t resolution = 0;
		RequireSetting(Utils::TryDecodeYamlScalar(cascades[index], resolution), field, "expected an unsigned integer");
		RequireSetting(IsTextureResolution(resolution), field, "must be a power of two in [32, 8192]");
		profile.m_shadowCascadeResolutions[index] = resolution;
	}
	ValidateProfile(profile);
	return true;
}

ProjectGraphicsSettingsLoadResult Sailor::Settings::ParseProjectGraphicsSettings(
	const std::string& payload, std::string_view sourceName) noexcept
{
	return ParseSettings<ProjectGraphicsSettingsLoadResult>(payload,
		sourceName.empty() ? "ProjectSettings.yaml" : sourceName);
}

EditorGraphicsSettingsLoadResult Sailor::Settings::ParseEditorGraphicsSettings(
	const std::string& payload, std::string_view sourceName) noexcept
{
	return ParseSettings<EditorGraphicsSettingsLoadResult>(payload,
		sourceName.empty() ? "EditorSettings.yaml" : sourceName);
}

Sailor::Settings::ProjectGraphicsSettingsLoadResult Sailor::Settings::LoadProjectGraphicsSettings(
	const std::filesystem::path& path) noexcept
{
	ProjectGraphicsSettingsLoadResult result;
	std::string payload;
	const EFileReadStatus readStatus = ReadTextFile(path, payload, result.m_diagnostic);
	if (readStatus == EFileReadStatus::Missing)
	{
		result.m_status = EGraphicsSettingsLoadStatus::Missing;
		return result;
	}
	if (readStatus == EFileReadStatus::IoFailure)
	{
		result.m_status = EGraphicsSettingsLoadStatus::IoFailure;
		return result;
	}
	return ParseProjectGraphicsSettings(payload, "Project settings " + Quote(Workspace::PathToUtf8(path)));
}

Sailor::Settings::EditorGraphicsSettingsLoadResult Sailor::Settings::LoadEditorGraphicsSettings(
	const std::filesystem::path& path) noexcept
{
	EditorGraphicsSettingsLoadResult result;
	std::string payload;
	const EFileReadStatus readStatus = ReadTextFile(path, payload, result.m_diagnostic);
	if (readStatus == EFileReadStatus::Missing)
	{
		result.m_status = EGraphicsSettingsLoadStatus::Missing;
		return result;
	}
	if (readStatus == EFileReadStatus::IoFailure)
	{
		result.m_status = EGraphicsSettingsLoadStatus::IoFailure;
		return result;
	}
	return ParseEditorGraphicsSettings(payload, "Editor settings " + Quote(Workspace::PathToUtf8(path)));
}

Sailor::Settings::GraphicsSettingsState Sailor::Settings::LoadGraphicsSettings(
	const Workspace::WorkspaceContext& workspaceContext,
	bool bEditorMode) noexcept
{
	GraphicsSettingsState state;
	state.m_bEditorMode = bEditorMode;
	const ProjectGraphicsSettingsLoadResult projectResult = LoadProjectGraphicsSettings(
		workspaceContext.GetProjectSettingsPath());
	state.m_projectSettings = projectResult.m_settings;
	state.m_projectLoadStatus = projectResult.m_status;
	state.m_projectDiagnostic = projectResult.m_diagnostic;

	if (bEditorMode)
	{
		const EditorGraphicsSettingsLoadResult editorResult = LoadEditorGraphicsSettings(
			workspaceContext.GetEditorSettingsPath());
		state.m_editorSettings = editorResult.m_settings;
		state.m_editorLoadStatus = editorResult.m_status;
		state.m_editorDiagnostic = editorResult.m_diagnostic;
	}
	else
	{
		state.m_editorLoadStatus = EGraphicsSettingsLoadStatus::NotLoaded;
		state.m_editorDiagnostic = "EditorSettings.yaml is ignored outside editor mode.";
	}

	state.m_activeQuality = bEditorMode
		? ResolveQualitySelection(
			state.m_editorSettings.m_selectedQuality,
			state.m_projectSettings.m_defaultQuality)
		: state.m_projectSettings.m_defaultQuality;
	return state;
}
