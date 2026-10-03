#include "Settings/GraphicsSettings.h"

#include "Core/YamlUtils.h"
#include "Workspace/WorkspaceContext.h"
#include "Workspace/WorkspacePathEncoding.h"

#include <charconv>
#include <cmath>
#include <fstream>
#include <sstream>
#include <type_traits>
#include <utility>

namespace
{
	using namespace Sailor;
	using namespace Sailor::Settings;

	constexpr std::array<EGraphicsQuality, NumGraphicsQualityPresets> GraphicsQualities
	{
		EGraphicsQuality::Ultra,
		EGraphicsQuality::High,
		EGraphicsQuality::Medium,
		EGraphicsQuality::Low,
		EGraphicsQuality::VeryLow
	};

	std::string Quote(const std::string& value)
	{
		return "'" + value + "'";
	}

	std::string SourceLabel(const std::string& sourceName, const char* fallback)
	{
		return sourceName.empty() ? fallback : sourceName;
	}

	std::string InvalidField(
		const std::string& source,
		const std::string& fieldPath,
		const std::string& requirement)
	{
		return source + " is invalid: field " + Quote(fieldPath) + " " + requirement + ".";
	}

	bool ValidateMap(
		const YAML::Node& document,
		const std::string& source,
		const std::string& fieldPath,
		std::string& outDiagnostic)
	{
		const Utils::YamlMapValidationResult validation =
			Utils::ValidateYamlMap(document);
		if (validation.m_error == Utils::EYamlMapValidationError::ExpectedMap)
		{
			outDiagnostic = fieldPath.empty()
				? source + " is invalid: the document root must be a map."
				: InvalidField(source, fieldPath, "must be a map");
			return false;
		}

		if (validation.m_error == Utils::EYamlMapValidationError::NonScalarKey ||
			validation.m_error == Utils::EYamlMapValidationError::EmptyKey)
		{
			outDiagnostic = fieldPath.empty()
				? source + " is invalid: the document contains an empty or non-scalar field name."
				: InvalidField(source, fieldPath, "contains an empty or non-scalar field name");
			return false;
		}
		if (validation.m_error == Utils::EYamlMapValidationError::DuplicateKey)
		{
			const std::string duplicatePath = fieldPath.empty()
				? validation.m_fieldName
				: fieldPath + "." + validation.m_fieldName;
			outDiagnostic = InvalidField(source, duplicatePath, "is duplicated");
			return false;
		}

		return true;
	}

	bool ReadMap(
		const YAML::Node& parent,
		const char* fieldName,
		const std::string& source,
		const std::string& fieldPath,
		YAML::Node& outValue,
		std::string& outDiagnostic)
	{
		outValue = Utils::FindYamlMapField(parent, fieldName);
		if (!outValue.IsDefined())
		{
			outDiagnostic = InvalidField(source, fieldPath, "is required and must be a map");
			return false;
		}
		return ValidateMap(outValue, source, fieldPath, outDiagnostic);
	}

	bool ReadScalar(
		const YAML::Node& parent,
		const char* fieldName,
		const std::string& source,
		const std::string& fieldPath,
		YAML::Node& outValue,
		std::string& outDiagnostic)
	{
		outValue = Utils::FindYamlMapField(parent, fieldName);
		if (!outValue.IsDefined() || !outValue.IsScalar())
		{
			outDiagnostic = InvalidField(source, fieldPath, "is required and must be a scalar");
			return false;
		}
		return true;
	}

	template<typename T>
	bool TryReadDecimalInteger(const YAML::Node& field, T& outValue)
	{
		if (!field.IsScalar()) return false;
		const std::string& scalar = field.Scalar();
		const char* begin = scalar.data();
		const char* end = begin + scalar.size();
		if constexpr (std::is_signed_v<T>)
		{
			if (begin != end && *begin == '+') ++begin;
		}
		const auto parsed = std::from_chars(begin, end, outValue);
		return begin != end && parsed.ec == std::errc() && parsed.ptr == end;
	}

	bool ReadUint32(
		const YAML::Node& parent,
		const char* fieldName,
		const std::string& source,
		const std::string& fieldPath,
		uint32_t& outValue,
		std::string& outDiagnostic)
	{
		YAML::Node field;
		if (!ReadScalar(parent, fieldName, source, fieldPath, field, outDiagnostic)) return false;
		if (!TryReadDecimalInteger(field, outValue))
		{
			outDiagnostic = InvalidField(source, fieldPath, "must be an unsigned 32-bit integer");
			return false;
		}
		return true;
	}

	bool ReadOptionalUint32(
		const YAML::Node& parent,
		const char* fieldName,
		const std::string& source,
		const std::string& fieldPath,
		uint32_t& outValue,
		std::string& outDiagnostic)
	{
		const YAML::Node field = Utils::FindYamlMapField(parent, fieldName);
		if (!field.IsDefined()) return true;
		if (!TryReadDecimalInteger(field, outValue))
		{
			outDiagnostic = InvalidField(source, fieldPath, "must be an unsigned 32-bit integer");
			return false;
		}
		return true;
	}

	bool ReadInt32(
		const YAML::Node& parent,
		const char* fieldName,
		const std::string& source,
		const std::string& fieldPath,
		int32_t& outValue,
		std::string& outDiagnostic)
	{
		YAML::Node field;
		if (!ReadScalar(parent, fieldName, source, fieldPath, field, outDiagnostic)) return false;
		if (!TryReadDecimalInteger(field, outValue))
		{
			outDiagnostic = InvalidField(source, fieldPath, "must be a signed 32-bit integer");
			return false;
		}
		return true;
	}

	template<typename TValue>
	bool ReadConvertedScalar(
		const YAML::Node& parent,
		const char* fieldName,
		const std::string& source,
		const std::string& fieldPath,
		const char* expectedType,
		TValue& outValue,
		std::string& outDiagnostic)
	{
		YAML::Node field;
		if (!ReadScalar(parent, fieldName, source, fieldPath, field, outDiagnostic))
		{
			return false;
		}

		std::string yamlDiagnostic;
		if (!Utils::TryDecodeYamlScalar(field, outValue, yamlDiagnostic))
		{
			outDiagnostic = InvalidField(source, fieldPath, std::string("must be ") + expectedType);
			if (!yamlDiagnostic.empty())
			{
				outDiagnostic += " YAML detail: " + yamlDiagnostic;
			}
			return false;
		}
		return true;
	}

	bool ReadString(
		const YAML::Node& parent,
		const char* fieldName,
		const std::string& source,
		const std::string& fieldPath,
		std::string& outValue,
		std::string& outDiagnostic)
	{
		YAML::Node field;
		if (!ReadScalar(parent, fieldName, source, fieldPath, field, outDiagnostic))
		{
			return false;
		}

		outValue = field.Scalar();
		if (outValue.empty())
		{
			outDiagnostic = InvalidField(source, fieldPath, "cannot be empty");
			return false;
		}
		return true;
	}

	bool IsPowerOfTwo(uint32_t value) noexcept
	{
		return value != 0u && (value & (value - 1u)) == 0u;
	}

	bool ReadRuntimeGIProfile(
		const YAML::Node& profile,
		const std::string& source,
		const std::string& profilePath,
		RuntimeGIProbesQualitySettings& quality,
		std::string& outDiagnostic)
	{
		const std::string path = profilePath + ".runtimeGIProbes";
		YAML::Node runtime;
		if (!ReadMap(profile, "runtimeGIProbes", source, path, runtime, outDiagnostic)) return false;

		const auto readUint = [&](const char* name, uint32_t& value)
		{
			return ReadUint32(runtime, name, source, path + "." + name, value, outDiagnostic);
		};
		const auto readFloat = [&](const char* name, float& value)
		{
			return ReadConvertedScalar(runtime, name, source, path + "." + name, "a finite number", value, outDiagnostic);
		};
		if (!readUint("version", quality.m_version) ||
			!ReadConvertedScalar(runtime, "enabled", source, path + ".enabled", "a boolean", quality.m_bEnabled, outDiagnostic) ||
			!readUint("maxActiveProbes", quality.m_maxActiveProbes) ||
			!readUint("initialSamplesPerProbe", quality.m_initialSamplesPerProbe) ||
			!readFloat("spacingMultiplier", quality.m_spacingMultiplier) ||
			!readUint("targetSamplesPerProbe", quality.m_targetSamplesPerProbe) ||
			!readUint("workerCount", quality.m_workerCount) ||
			!readFloat("cpuDutyFraction", quality.m_cpuDutyFraction) ||
			!readFloat("cpuBudgetMilliseconds", quality.m_cpuBudgetMilliseconds) ||
			!readFloat("maxPublicationsPerSecond", quality.m_maxPublicationsPerSecond) ||
			!readFloat("initialPublicationCoverage", quality.m_initialPublicationCoverage) ||
			!readUint("maxDirtyUploadBytesPerFrame", quality.m_maxDirtyUploadBytesPerFrame))
		{
			return false;
		}
		std::string diagnostic;
		if (!quality.Validate(diagnostic))
		{
			outDiagnostic = InvalidField(source, path, diagnostic);
			return false;
		}
		return true;
	}

	bool ReadProfile(
		const YAML::Node& presets,
		EGraphicsQuality quality,
		const std::string& source,
		GraphicsQualityProfile& outProfile,
		std::string& outDiagnostic)
	{
		const std::string qualityName(magic_enum::enum_name(quality));
		const std::string path = "graphics.presets." + qualityName;
		YAML::Node profile;
		if (!ReadMap(presets, qualityName.c_str(), source, path, profile, outDiagnostic)) return false;

		const auto invalid = [&](const char* name, const char* requirement)
		{
			if (outDiagnostic.empty()) outDiagnostic = InvalidField(source, path + "." + name, requirement);
			return false;
		};
		const auto readUint = [&](const char* name, uint32_t& value)
		{
			return ReadUint32(profile, name, source, path + "." + name, value, outDiagnostic);
		};
		const auto readFloat = [&](const char* name, float& value)
		{
			return ReadConvertedScalar(profile, name, source, path + "." + name, "a finite number", value, outDiagnostic);
		};
		const auto readBool = [&](const char* name, bool& value)
		{
			return ReadConvertedScalar(profile, name, source, path + "." + name, "a boolean", value, outDiagnostic);
		};

		if (!readFloat("resolutionFactor", outProfile.m_resolutionFactor) ||
			!std::isfinite(outProfile.m_resolutionFactor) ||
			outProfile.m_resolutionFactor < 0.25f || outProfile.m_resolutionFactor > 2.0f)
		{
			return invalid("resolutionFactor", "must be a finite number in the range [0.25, 2.0]");
		}
		if (!readUint("fpsCap", outProfile.m_fpsCap) || outProfile.m_fpsCap < 1u || outProfile.m_fpsCap > 1000u)
		{
			return invalid("fpsCap", "must be in the range [1, 1000]");
		}
		if (!readUint("msaaSamples", outProfile.m_msaaSamples) ||
			(outProfile.m_msaaSamples != 1u && outProfile.m_msaaSamples != 2u &&
				outProfile.m_msaaSamples != 4u && outProfile.m_msaaSamples != 8u))
		{
			return invalid("msaaSamples", "must be one of 1, 2, 4, or 8");
		}

		std::string shadowQuality;
		if (!ReadString(profile, "shadowQuality", source, path + ".shadowQuality", shadowQuality, outDiagnostic)) return false;
		const auto parsedShadowQuality = magic_enum::enum_cast<ELightShadowQuality>(shadowQuality);
		if (!parsedShadowQuality) return invalid("shadowQuality", "must be one of High, Medium, Low, or VeryLow");
		outProfile.m_shadowQuality = *parsedShadowQuality;

		if (!readFloat("shadowBias", outProfile.m_shadowBias) || !std::isfinite(outProfile.m_shadowBias) ||
			outProfile.m_shadowBias < -16.0f || outProfile.m_shadowBias > 16.0f)
		{
			return invalid("shadowBias", "must be a finite number in the range [-16, 16]");
		}
		if (!readFloat("shadowDistance", outProfile.m_shadowDistance) || !std::isfinite(outProfile.m_shadowDistance) ||
			outProfile.m_shadowDistance < 1.0f || outProfile.m_shadowDistance > 10000.0f)
		{
			return invalid("shadowDistance", "must be a finite number in the range [1, 10000]");
		}
		if (!readUint("shadowCascadeCount", outProfile.m_shadowCascadeCount) ||
			outProfile.m_shadowCascadeCount < 1u || outProfile.m_shadowCascadeCount > MaxShadowCascades)
		{
			return invalid("shadowCascadeCount", "must be in the range [1, 4]");
		}

		const std::string cascadePath = path + ".shadowCascadeResolutions";
		const YAML::Node cascades = Utils::FindYamlMapField(profile, "shadowCascadeResolutions");
		if (!cascades.IsDefined() || !cascades.IsSequence() || cascades.size() != outProfile.m_shadowCascadeCount)
		{
			return invalid("shadowCascadeResolutions", "must be a sequence whose length matches shadowCascadeCount");
		}
		outProfile.m_shadowCascadeResolutions.fill(0u);
		for (uint32_t index = 0; index < outProfile.m_shadowCascadeCount; ++index)
		{
			const std::string resolutionPath = cascadePath + "[" + std::to_string(index) + "]";
			if (!cascades[index].IsScalar())
			{
				outDiagnostic = InvalidField(source, resolutionPath, "must be an unsigned integer");
				return false;
			}
			uint32_t resolution = 0;
			if (!TryReadDecimalInteger(cascades[index], resolution) ||
				resolution < 32u || resolution > 8192u || !IsPowerOfTwo(resolution))
			{
				outDiagnostic = InvalidField(source, resolutionPath, "must be a power of two in the range [32, 8192]");
				return false;
			}
			outProfile.m_shadowCascadeResolutions[index] = resolution;
		}

		if (!readBool("supportSoftShadows", outProfile.m_bSupportSoftShadows)) return false;
		if (!readFloat("cloudsResolutionMultiplier", outProfile.m_cloudsResolutionMultiplier) ||
			!std::isfinite(outProfile.m_cloudsResolutionMultiplier) ||
			outProfile.m_cloudsResolutionMultiplier < 0.0625f || outProfile.m_cloudsResolutionMultiplier > 2.0f)
		{
			return invalid("cloudsResolutionMultiplier", "must be a finite number in the range [0.0625, 2.0]");
		}
		if (!readBool("cloudsDithering", outProfile.m_bCloudsDithering)) return false;
		if (!readUint("skyResolution", outProfile.m_skyResolution) ||
			outProfile.m_skyResolution < 32u || outProfile.m_skyResolution > 8192u || !IsPowerOfTwo(outProfile.m_skyResolution))
		{
			return invalid("skyResolution", "must be a power of two in the range [32, 8192]");
		}
		if (!ReadInt32(profile, "lodBias", source, path + ".lodBias", outProfile.m_lodBias, outDiagnostic) ||
			outProfile.m_lodBias < -8 || outProfile.m_lodBias > 8)
		{
			return invalid("lodBias", "must be in the range [-8, 8]");
		}
		if (!ReadOptionalUint32(profile, "vegetationInstanceBudget", source, path + ".vegetationInstanceBudget",
			outProfile.m_vegetationInstanceBudget, outDiagnostic) || outProfile.m_vegetationInstanceBudget > 1048576u)
		{
			return invalid("vegetationInstanceBudget", "must be in the range [0, 1048576]");
		}
		if (!readBool("enableGlobalIllumination", outProfile.m_bEnableGlobalIllumination)) return false;
		if (!readUint("maxGiProbeStatesPerSnapshot", outProfile.m_maxGiProbeStatesPerSnapshot) ||
			outProfile.m_maxGiProbeStatesPerSnapshot > 16u)
		{
			return invalid("maxGiProbeStatesPerSnapshot", "must be in the range [0, 16]");
		}
		return ReadRuntimeGIProfile(profile, source, path, outProfile.m_runtimeGIProbes, outDiagnostic);
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
}

Sailor::Settings::ProjectGraphicsSettingsLoadResult Sailor::Settings::ParseProjectGraphicsSettings(
	const std::string& payload,
	const std::string& sourceName) noexcept
{
	ProjectGraphicsSettingsLoadResult result;
	const std::string source = SourceLabel(sourceName, "ProjectSettings.yaml");
	YAML::Node document;
	std::string yamlDiagnostic;
	if (!Utils::TryLoadSingleYamlDocument(payload, document, yamlDiagnostic))
	{
		result.m_status = EGraphicsSettingsLoadStatus::Invalid;
		result.m_diagnostic = source + " is invalid YAML: " + yamlDiagnostic;
		return result;
	}

	if (!ValidateMap(document, source, {}, result.m_diagnostic))
	{
		result.m_status = EGraphicsSettingsLoadStatus::Invalid;
		return result;
	}

	GraphicsSettings parsedSettings;
	if (!ReadUint32(
			document,
			"settingsVersion",
			source,
			"settingsVersion",
			parsedSettings.m_version,
			result.m_diagnostic))
	{
		result.m_status = EGraphicsSettingsLoadStatus::Invalid;
		return result;
	}
	if (parsedSettings.m_version != ProjectGraphicsSettingsVersion)
	{
		result.m_status = EGraphicsSettingsLoadStatus::UnsupportedVersion;
		result.m_diagnostic = source + " has unsupported settingsVersion (expected " +
			Quote(std::to_string(ProjectGraphicsSettingsVersion)) + ", actual " +
			Quote(std::to_string(parsedSettings.m_version)) + "); using built-in defaults.";
		return result;
	}

	YAML::Node graphics;
	if (!ReadMap(document, "graphics", source, "graphics", graphics, result.m_diagnostic))
	{
		result.m_status = EGraphicsSettingsLoadStatus::Invalid;
		return result;
	}

	std::string defaultQuality;
	if (!ReadString(
			graphics,
			"defaultQuality",
			source,
			"graphics.defaultQuality",
			defaultQuality,
			result.m_diagnostic))
	{
		result.m_status = EGraphicsSettingsLoadStatus::Invalid;
		return result;
	}
	const auto parsedDefaultQuality =
		magic_enum::enum_cast<EGraphicsQuality>(defaultQuality);
	if (!parsedDefaultQuality)
	{
		result.m_status = EGraphicsSettingsLoadStatus::Invalid;
		result.m_diagnostic = InvalidField(
			source,
			"graphics.defaultQuality",
			"must be one of Ultra, High, Medium, Low, or VeryLow");
		return result;
	}
	parsedSettings.m_defaultQuality = *parsedDefaultQuality;

	YAML::Node presets;
	if (!ReadMap(graphics, "presets", source, "graphics.presets", presets, result.m_diagnostic))
	{
		result.m_status = EGraphicsSettingsLoadStatus::Invalid;
		return result;
	}

	for (EGraphicsQuality quality : GraphicsQualities)
	{
		if (!ReadProfile(
				presets,
				quality,
				source,
				parsedSettings.m_presets[static_cast<size_t>(quality)],
				result.m_diagnostic))
		{
			result.m_status = EGraphicsSettingsLoadStatus::Invalid;
			return result;
		}
	}
	result.m_settings = std::move(parsedSettings);
	result.m_status = EGraphicsSettingsLoadStatus::Loaded;
	result.m_diagnostic = "Loaded " + source + ".";
	return result;
}

Sailor::Settings::EditorGraphicsSettingsLoadResult Sailor::Settings::ParseEditorGraphicsSettings(
	const std::string& payload,
	const std::string& sourceName) noexcept
{
	EditorGraphicsSettingsLoadResult result;
	const std::string source = SourceLabel(sourceName, "EditorSettings.yaml");
	YAML::Node document;
	std::string yamlDiagnostic;
	if (!Utils::TryLoadSingleYamlDocument(payload, document, yamlDiagnostic))
	{
		result.m_status = EGraphicsSettingsLoadStatus::Invalid;
		result.m_diagnostic = source + " is invalid YAML: " + yamlDiagnostic;
		return result;
	}

	if (!ValidateMap(document, source, {}, result.m_diagnostic))
	{
		result.m_status = EGraphicsSettingsLoadStatus::Invalid;
		return result;
	}

	EditorGraphicsSettings parsedSettings;
	if (!ReadUint32(
			document,
			"settingsVersion",
			source,
			"settingsVersion",
			parsedSettings.m_version,
			result.m_diagnostic))
	{
		result.m_status = EGraphicsSettingsLoadStatus::Invalid;
		return result;
	}
	if (parsedSettings.m_version != EditorGraphicsSettingsVersion)
	{
		result.m_status = EGraphicsSettingsLoadStatus::UnsupportedVersion;
		result.m_diagnostic = source + " has unsupported settingsVersion (expected " +
			Quote(std::to_string(EditorGraphicsSettingsVersion)) + ", actual " +
			Quote(std::to_string(parsedSettings.m_version)) + "); using editor defaults.";
		return result;
	}

	YAML::Node graphics;
	if (!ReadMap(document, "graphics", source, "graphics", graphics, result.m_diagnostic))
	{
		result.m_status = EGraphicsSettingsLoadStatus::Invalid;
		return result;
	}

	std::string selectedQuality;
	if (!ReadString(
			graphics,
			"selectedQuality",
			source,
			"graphics.selectedQuality",
			selectedQuality,
			result.m_diagnostic))
	{
		result.m_status = EGraphicsSettingsLoadStatus::Invalid;
		return result;
	}
	const auto parsedSelectedQuality =
		magic_enum::enum_cast<EGraphicsQualitySelection>(selectedQuality);
	if (!parsedSelectedQuality)
	{
		result.m_status = EGraphicsSettingsLoadStatus::Invalid;
		result.m_diagnostic = InvalidField(
			source,
			"graphics.selectedQuality",
			"must be one of ProjectDefault, Ultra, High, Medium, Low, or VeryLow");
		return result;
	}
	parsedSettings.m_selectedQuality = *parsedSelectedQuality;

	std::string statsMode;
	if (!ReadString(
			graphics,
			"statsMode",
			source,
			"graphics.statsMode",
			statsMode,
			result.m_diagnostic))
	{
		result.m_status = EGraphicsSettingsLoadStatus::Invalid;
		return result;
	}
	const auto parsedStatsMode =
		magic_enum::enum_cast<ERenderStatsMode>(statsMode);
	if (!parsedStatsMode)
	{
		result.m_status = EGraphicsSettingsLoadStatus::Invalid;
		result.m_diagnostic = InvalidField(
			source,
			"graphics.statsMode",
			"must be one of None, RenderStats, or RenderStatsAndQueries");
		return result;
	}
	parsedSettings.m_statsMode = *parsedStatsMode;
	if (!ReadConvertedScalar(
			graphics,
			"runtimeGIProbesPreviewEnabled",
			source,
			"graphics.runtimeGIProbesPreviewEnabled",
			"a boolean",
			parsedSettings.m_bRuntimeGIProbesPreviewEnabled,
			result.m_diagnostic))
	{
		result.m_status = EGraphicsSettingsLoadStatus::Invalid;
		return result;
	}
	std::string runtimeGIBudget;
	if (!ReadString(
			graphics,
			"runtimeGIProbesBudget",
			source,
			"graphics.runtimeGIProbesBudget",
			runtimeGIBudget,
			result.m_diagnostic))
	{
		result.m_status = EGraphicsSettingsLoadStatus::Invalid;
		return result;
	}
	const auto parsedRuntimeGIBudget =
		magic_enum::enum_cast<ERuntimeGIProbesEditorBudget>(runtimeGIBudget);
	if (!parsedRuntimeGIBudget)
	{
		result.m_status = EGraphicsSettingsLoadStatus::Invalid;
		result.m_diagnostic = InvalidField(
			source,
			"graphics.runtimeGIProbesBudget",
			"must be Eco or Balanced");
		return result;
	}
	parsedSettings.m_runtimeGIProbesBudget = *parsedRuntimeGIBudget;

	std::string runtimeGIDebugView;
	if (!ReadString(
			graphics,
			"runtimeGIProbesDebugView",
			source,
			"graphics.runtimeGIProbesDebugView",
			runtimeGIDebugView,
			result.m_diagnostic))
	{
		result.m_status = EGraphicsSettingsLoadStatus::Invalid;
		return result;
	}
	const auto parsedRuntimeGIDebugView =
		magic_enum::enum_cast<ERuntimeGIProbesEditorDebugView>(
			runtimeGIDebugView);
	if (!parsedRuntimeGIDebugView)
	{
		result.m_status = EGraphicsSettingsLoadStatus::Invalid;
		result.m_diagnostic = InvalidField(
			source,
			"graphics.runtimeGIProbesDebugView",
			"must be a supported Runtime GI debug view");
		return result;
	}
	parsedSettings.m_runtimeGIProbesDebugView = *parsedRuntimeGIDebugView;

	result.m_settings = parsedSettings;
	result.m_status = EGraphicsSettingsLoadStatus::Loaded;
	result.m_diagnostic = "Loaded " + source + ".";
	return result;
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
