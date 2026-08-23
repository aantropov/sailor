#include "Settings/GraphicsSettings.h"

#include "RHI/Types.h"
#include "Workspace/WorkspaceContext.h"
#include "Workspace/WorkspacePathEncoding.h"
#include "YamlExceptionBoundary.h"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <fstream>
#include <limits>
#include <sstream>
#include <system_error>
#include <utility>
#include <yaml-cpp/eventhandler.h>
#include <yaml-cpp/yaml.h>

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

	size_t QualityIndex(EGraphicsQuality quality) noexcept
	{
		switch (quality)
		{
		case EGraphicsQuality::Ultra: return 0u;
		case EGraphicsQuality::High: return 1u;
		case EGraphicsQuality::Medium: return 2u;
		case EGraphicsQuality::Low: return 3u;
		case EGraphicsQuality::VeryLow: return 4u;
		default: return 1u;
		}
	}

	GraphicsQualityProfile MakeProfile(
		float resolutionFactor,
		uint32_t fpsCap,
		uint32_t msaaSamples,
		ELightShadowQuality shadowQuality,
		float shadowBias,
		uint32_t shadowCascadeCount,
		const std::array<uint32_t, MaxShadowCascades>& shadowCascadeResolutions,
		bool bSupportSoftShadows,
		float cloudsResolutionMultiplier,
		uint32_t skyResolution,
		uint32_t vegetationInstanceBudget,
		int32_t lodBias,
		uint32_t maxGiProbeStatesPerSnapshot)
	{
		GraphicsQualityProfile profile;
		profile.m_resolutionFactor = resolutionFactor;
		profile.m_fpsCap = fpsCap;
		profile.m_msaaSamples = msaaSamples;
		profile.m_shadowQuality = shadowQuality;
		profile.m_shadowBias = shadowBias;
		profile.m_shadowCascadeCount = shadowCascadeCount;
		profile.m_shadowCascadeResolutions = shadowCascadeResolutions;
		profile.m_bSupportSoftShadows = bSupportSoftShadows;
		profile.m_cloudsResolutionMultiplier = cloudsResolutionMultiplier;
		profile.m_skyResolution = skyResolution;
		profile.m_vegetationInstanceBudget = vegetationInstanceBudget;
		profile.m_lodBias = lodBias;
		profile.m_maxGiProbeStatesPerSnapshot = maxGiProbeStatesPerSnapshot;
		return profile;
	}

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

	class YamlDocumentCounter final : public YAML::EventHandler
	{
	public:
		void OnDocumentStart(const YAML::Mark&) override { ++m_numDocuments; }
		void OnDocumentEnd() override {}
		void OnNull(const YAML::Mark&, YAML::anchor_t) override {}
		void OnAlias(const YAML::Mark&, YAML::anchor_t) override {}
		void OnScalar(
			const YAML::Mark&,
			const std::string&,
			YAML::anchor_t,
			const std::string&) override {}
		void OnSequenceStart(
			const YAML::Mark&,
			const std::string&,
			YAML::anchor_t,
			YAML::EmitterStyle::value) override {}
		void OnSequenceEnd() override {}
		void OnMapStart(
			const YAML::Mark&,
			const std::string&,
			YAML::anchor_t,
			YAML::EmitterStyle::value) override {}
		void OnMapEnd() override {}

		size_t GetNumDocuments() const noexcept { return m_numDocuments; }

	private:
		size_t m_numDocuments = 0u;
	};

	bool TryLoadSingleDocument(
		const std::string& payload,
		YAML::Node& outDocument,
		std::string& outDiagnostic) noexcept
	{
		size_t documentCount = 0u;
		if (!External::GuardYamlExceptions(
				[&]()
				{
					std::istringstream input(payload);
					YAML::Parser parser(input);
					YamlDocumentCounter counter;
					while (parser.HandleNextDocument(counter)) {}
					documentCount = counter.GetNumDocuments();
					if (documentCount == 1u)
					{
						outDocument = YAML::Load(payload);
					}
				},
				outDiagnostic))
		{
			return false;
		}
		if (documentCount != 1u)
		{
			outDiagnostic = "expected exactly one YAML document, found " +
				std::to_string(documentCount);
			return false;
		}
		return true;
	}

	YAML::Node FindField(const YAML::Node& document, const char* fieldName)
	{
		if (!document.IsMap())
		{
			return YAML::Node(YAML::NodeType::Undefined);
		}

		for (const auto& field : document)
		{
			if (field.first.IsScalar() && field.first.Scalar() == fieldName)
			{
				return field.second;
			}
		}

		return YAML::Node(YAML::NodeType::Undefined);
	}

	bool ValidateMap(
		const YAML::Node& document,
		const std::string& source,
		const std::string& fieldPath,
		std::string& outDiagnostic)
	{
		if (!document.IsMap())
		{
			outDiagnostic = fieldPath.empty()
				? source + " is invalid: the document root must be a map."
				: InvalidField(source, fieldPath, "must be a map");
			return false;
		}

		size_t fieldIndex = 0u;
		for (const auto& field : document)
		{
			if (!field.first.IsScalar() || field.first.Scalar().empty())
			{
				outDiagnostic = fieldPath.empty()
					? source + " is invalid: the document contains an empty or non-scalar field name."
					: InvalidField(source, fieldPath, "contains an empty or non-scalar field name");
				return false;
			}

			const std::string key = field.first.Scalar();
			size_t previousIndex = 0u;
			for (const auto& previousField : document)
			{
				if (previousIndex++ >= fieldIndex)
				{
					break;
				}
				if (previousField.first.IsScalar() && previousField.first.Scalar() == key)
				{
					const std::string duplicatePath = fieldPath.empty() ? key : fieldPath + "." + key;
					outDiagnostic = InvalidField(source, duplicatePath, "is duplicated");
					return false;
				}
			}
			++fieldIndex;
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
		outValue = FindField(parent, fieldName);
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
		outValue = FindField(parent, fieldName);
		if (!outValue.IsDefined() || !outValue.IsScalar())
		{
			outDiagnostic = InvalidField(source, fieldPath, "is required and must be a scalar");
			return false;
		}
		return true;
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
		if (!ReadScalar(parent, fieldName, source, fieldPath, field, outDiagnostic))
		{
			return false;
		}

		const std::string scalar = field.Scalar();
		const char* begin = scalar.data();
		const char* end = begin + scalar.size();
		const auto parsed = std::from_chars(begin, end, outValue);
		if (scalar.empty() || parsed.ec != std::errc() || parsed.ptr != end)
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
		const YAML::Node field = FindField(parent, fieldName);
		if (!field.IsDefined())
		{
			return true;
		}
		if (!field.IsScalar())
		{
			outDiagnostic = InvalidField(source, fieldPath, "must be an unsigned 32-bit integer");
			return false;
		}

		const std::string scalar = field.Scalar();
		const char* begin = scalar.data();
		const char* end = begin + scalar.size();
		const auto parsed = std::from_chars(begin, end, outValue);
		if (scalar.empty() || parsed.ec != std::errc() || parsed.ptr != end)
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
		if (!ReadScalar(parent, fieldName, source, fieldPath, field, outDiagnostic))
		{
			return false;
		}

		const std::string scalar = field.Scalar();
		const char* begin = scalar.data();
		const char* end = begin + scalar.size();
		if (begin != end && *begin == '+')
		{
			++begin;
		}
		const auto parsed = std::from_chars(begin, end, outValue);
		if (begin == end || parsed.ec != std::errc() || parsed.ptr != end)
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
		if (!External::TryConvertYaml(field, outValue, yamlDiagnostic))
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

	bool ParseQuality(const std::string& value, EGraphicsQuality& outQuality) noexcept
	{
		if (value == "Ultra") outQuality = EGraphicsQuality::Ultra;
		else if (value == "High") outQuality = EGraphicsQuality::High;
		else if (value == "Medium") outQuality = EGraphicsQuality::Medium;
		else if (value == "Low") outQuality = EGraphicsQuality::Low;
		else if (value == "VeryLow") outQuality = EGraphicsQuality::VeryLow;
		else return false;
		return true;
	}

	bool ParseQualitySelection(
		const std::string& value,
		EGraphicsQualitySelection& outSelection) noexcept
	{
		if (value == "ProjectDefault") outSelection = EGraphicsQualitySelection::ProjectDefault;
		else if (value == "Ultra") outSelection = EGraphicsQualitySelection::Ultra;
		else if (value == "High") outSelection = EGraphicsQualitySelection::High;
		else if (value == "Medium") outSelection = EGraphicsQualitySelection::Medium;
		else if (value == "Low") outSelection = EGraphicsQualitySelection::Low;
		else if (value == "VeryLow") outSelection = EGraphicsQualitySelection::VeryLow;
		else return false;
		return true;
	}

	bool ParseShadowQuality(
		const std::string& value,
		ELightShadowQuality& outQuality) noexcept
	{
		if (value == "High") outQuality = ELightShadowQuality::High;
		else if (value == "Medium") outQuality = ELightShadowQuality::Medium;
		else if (value == "Low") outQuality = ELightShadowQuality::Low;
		else if (value == "VeryLow") outQuality = ELightShadowQuality::VeryLow;
		else return false;
		return true;
	}

	bool ParseStatsMode(const std::string& value, ERenderStatsMode& outMode) noexcept
	{
		if (value == "None") outMode = ERenderStatsMode::None;
		else if (value == "RenderStats") outMode = ERenderStatsMode::RenderStats;
		else if (value == "RenderStatsAndQueries") outMode = ERenderStatsMode::RenderStatsAndQueries;
		else return false;
		return true;
	}

	bool IsPowerOfTwo(uint32_t value) noexcept
	{
		return value != 0u && (value & (value - 1u)) == 0u;
	}

	bool ReadProfile(
		const YAML::Node& presets,
		EGraphicsQuality quality,
		const std::string& source,
		bool bAllowMissingGiProbeStateBudget,
		GraphicsQualityProfile& outProfile,
		std::string& outDiagnostic)
	{
		const char* qualityName = ToString(quality);
		const std::string profilePath = "graphics.presets." + std::string(qualityName);
		YAML::Node profile;
		if (!ReadMap(presets, qualityName, source, profilePath, profile, outDiagnostic))
		{
			return false;
		}

		if (!ReadConvertedScalar(
				profile,
				"resolutionFactor",
				source,
				profilePath + ".resolutionFactor",
				"a finite number",
				outProfile.m_resolutionFactor,
				outDiagnostic) ||
			!std::isfinite(outProfile.m_resolutionFactor) ||
			outProfile.m_resolutionFactor < 0.25f ||
			outProfile.m_resolutionFactor > 2.0f)
		{
			if (outDiagnostic.empty())
			{
				outDiagnostic = InvalidField(
					source,
					profilePath + ".resolutionFactor",
					"must be a finite number in the range [0.25, 2.0]");
			}
			return false;
		}

		if (!ReadUint32(
				profile,
				"fpsCap",
				source,
				profilePath + ".fpsCap",
				outProfile.m_fpsCap,
				outDiagnostic) ||
			outProfile.m_fpsCap < 1u ||
			outProfile.m_fpsCap > 1000u)
		{
			if (outDiagnostic.empty())
			{
				outDiagnostic = InvalidField(
					source,
					profilePath + ".fpsCap",
					"must be in the range [1, 1000]");
			}
			return false;
		}

		if (!ReadUint32(
				profile,
				"msaaSamples",
				source,
				profilePath + ".msaaSamples",
				outProfile.m_msaaSamples,
				outDiagnostic) ||
			(outProfile.m_msaaSamples != 1u &&
				outProfile.m_msaaSamples != 2u &&
				outProfile.m_msaaSamples != 4u &&
				outProfile.m_msaaSamples != 8u))
		{
			if (outDiagnostic.empty())
			{
				outDiagnostic = InvalidField(
					source,
					profilePath + ".msaaSamples",
					"must be one of 1, 2, 4, or 8");
			}
			return false;
		}

		std::string shadowQuality;
		if (!ReadString(
				profile,
				"shadowQuality",
				source,
				profilePath + ".shadowQuality",
				shadowQuality,
				outDiagnostic) ||
			!ParseShadowQuality(shadowQuality, outProfile.m_shadowQuality))
		{
			if (outDiagnostic.empty())
			{
				outDiagnostic = InvalidField(
					source,
					profilePath + ".shadowQuality",
					"must be one of High, Medium, Low, or VeryLow");
			}
			return false;
		}

		if (!ReadConvertedScalar(
				profile,
				"shadowBias",
				source,
				profilePath + ".shadowBias",
				"a finite number",
				outProfile.m_shadowBias,
				outDiagnostic) ||
			!std::isfinite(outProfile.m_shadowBias) ||
			outProfile.m_shadowBias < -16.0f ||
			outProfile.m_shadowBias > 16.0f)
		{
			if (outDiagnostic.empty())
			{
				outDiagnostic = InvalidField(
					source,
					profilePath + ".shadowBias",
					"must be a finite number in the range [-16, 16]");
			}
			return false;
		}

		if (!ReadUint32(
				profile,
				"shadowCascadeCount",
				source,
				profilePath + ".shadowCascadeCount",
				outProfile.m_shadowCascadeCount,
				outDiagnostic) ||
			outProfile.m_shadowCascadeCount < 1u ||
			outProfile.m_shadowCascadeCount > MaxShadowCascades)
		{
			if (outDiagnostic.empty())
			{
				outDiagnostic = InvalidField(
					source,
					profilePath + ".shadowCascadeCount",
					"must be in the range [1, 4]");
			}
			return false;
		}

		const std::string cascadePath = profilePath + ".shadowCascadeResolutions";
		const YAML::Node cascadeResolutions = FindField(profile, "shadowCascadeResolutions");
		if (!cascadeResolutions.IsDefined() || !cascadeResolutions.IsSequence() ||
			cascadeResolutions.size() != outProfile.m_shadowCascadeCount)
		{
			outDiagnostic = InvalidField(
				source,
				cascadePath,
				"must be a sequence whose length matches shadowCascadeCount");
			return false;
		}

		outProfile.m_shadowCascadeResolutions.fill(0u);
		for (uint32_t index = 0u; index < outProfile.m_shadowCascadeCount; ++index)
		{
			const YAML::Node resolution = cascadeResolutions[index];
			const std::string resolutionPath = cascadePath + "[" + std::to_string(index) + "]";
			if (!resolution.IsScalar())
			{
				outDiagnostic = InvalidField(source, resolutionPath, "must be an unsigned integer");
				return false;
			}

			const std::string scalar = resolution.Scalar();
			uint32_t parsedResolution = 0u;
			const char* begin = scalar.data();
			const char* end = begin + scalar.size();
			const auto parsed = std::from_chars(begin, end, parsedResolution);
			if (scalar.empty() || parsed.ec != std::errc() || parsed.ptr != end ||
				parsedResolution < 32u || parsedResolution > 8192u ||
				!IsPowerOfTwo(parsedResolution))
			{
				outDiagnostic = InvalidField(
					source,
					resolutionPath,
					"must be a power of two in the range [32, 8192]");
				return false;
			}
			outProfile.m_shadowCascadeResolutions[index] = parsedResolution;
		}

		if (!ReadConvertedScalar(
				profile,
				"supportSoftShadows",
				source,
				profilePath + ".supportSoftShadows",
				"a boolean",
				outProfile.m_bSupportSoftShadows,
				outDiagnostic))
		{
			return false;
		}

		if (!ReadConvertedScalar(
				profile,
				"cloudsResolutionMultiplier",
				source,
				profilePath + ".cloudsResolutionMultiplier",
				"a finite number",
				outProfile.m_cloudsResolutionMultiplier,
				outDiagnostic) ||
			!std::isfinite(outProfile.m_cloudsResolutionMultiplier) ||
			outProfile.m_cloudsResolutionMultiplier < 0.0625f ||
			outProfile.m_cloudsResolutionMultiplier > 2.0f)
		{
			if (outDiagnostic.empty())
			{
				outDiagnostic = InvalidField(
					source,
					profilePath + ".cloudsResolutionMultiplier",
					"must be a finite number in the range [0.0625, 2.0]");
			}
			return false;
		}

		if (!ReadUint32(
				profile,
				"skyResolution",
				source,
				profilePath + ".skyResolution",
				outProfile.m_skyResolution,
				outDiagnostic) ||
			outProfile.m_skyResolution < 32u ||
			outProfile.m_skyResolution > 8192u ||
			!IsPowerOfTwo(outProfile.m_skyResolution))
		{
			if (outDiagnostic.empty())
			{
				outDiagnostic = InvalidField(
					source,
					profilePath + ".skyResolution",
					"must be a power of two in the range [32, 8192]");
			}
			return false;
		}

		if (!ReadInt32(
				profile,
				"lodBias",
				source,
				profilePath + ".lodBias",
				outProfile.m_lodBias,
				outDiagnostic) ||
			outProfile.m_lodBias < -8 || outProfile.m_lodBias > 8)
		{
			if (outDiagnostic.empty())
			{
				outDiagnostic = InvalidField(
					source,
					profilePath + ".lodBias",
					"must be in the range [-8, 8]");
			}
			return false;
		}

		if (!ReadOptionalUint32(
				profile,
				"vegetationInstanceBudget",
				source,
				profilePath + ".vegetationInstanceBudget",
				outProfile.m_vegetationInstanceBudget,
				outDiagnostic) ||
			outProfile.m_vegetationInstanceBudget > 1048576u)
		{
			if (outDiagnostic.empty())
			{
				outDiagnostic = InvalidField(
					source,
					profilePath + ".vegetationInstanceBudget",
					"must be in the range [0, 1048576]");
			}
			return false;
		}

		const YAML::Node giBudget = FindField(
			profile,
			"maxGiProbeStatesPerSnapshot");
		if (bAllowMissingGiProbeStateBudget && !giBudget.IsDefined())
		{
			return true;
		}
		if (!ReadUint32(
				profile,
				"maxGiProbeStatesPerSnapshot",
				source,
				profilePath + ".maxGiProbeStatesPerSnapshot",
				outProfile.m_maxGiProbeStatesPerSnapshot,
				outDiagnostic) ||
			outProfile.m_maxGiProbeStatesPerSnapshot > 16u)
		{
			if (outDiagnostic.empty())
			{
				outDiagnostic = InvalidField(
					source,
					profilePath + ".maxGiProbeStatesPerSnapshot",
					"must be in the range [0, 16]");
			}
			return false;
		}

		return true;
	}

	uint32_t ScaleDimension(uint32_t dimension, float scale) noexcept
	{
		if (dimension == 0u || !std::isfinite(scale) || scale <= 0.0f)
		{
			return 1u;
		}

		const double scaled = static_cast<double>(dimension) * static_cast<double>(scale);
		if (!std::isfinite(scaled) || scaled >= static_cast<double>((std::numeric_limits<uint32_t>::max)()))
		{
			return (std::numeric_limits<uint32_t>::max)();
		}
		return (std::max)(1u, static_cast<uint32_t>(std::floor(scaled + 0.5)));
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

Sailor::Settings::GraphicsSettings::GraphicsSettings()
{
	m_presets[QualityIndex(EGraphicsQuality::Ultra)] = MakeProfile(
		1.0f,
		120u,
		8u,
		ELightShadowQuality::High,
		0.0f,
		4u,
		{ 4096u, 2048u, 2048u, 1024u },
		true,
		1.0f,
		512u,
		65536u,
		-1,
		4u);
	m_presets[QualityIndex(EGraphicsQuality::High)] = MakeProfile(
		1.0f,
		120u,
		4u,
		ELightShadowQuality::High,
		0.0f,
		4u,
		{ 2048u, 2048u, 1024u, 1024u },
		true,
		0.75f,
		256u,
		32768u,
		0,
		3u);
	m_presets[QualityIndex(EGraphicsQuality::Medium)] = MakeProfile(
		0.85f,
		120u,
		2u,
		ELightShadowQuality::Medium,
		0.0f,
		3u,
		{ 2048u, 1024u, 512u, 0u },
		true,
		0.5f,
		256u,
		16384u,
		0,
		2u);
	m_presets[QualityIndex(EGraphicsQuality::Low)] = MakeProfile(
		0.7f,
		120u,
		1u,
		ELightShadowQuality::Low,
		0.0f,
		2u,
		{ 1024u, 512u, 0u, 0u },
		false,
		0.25f,
		128u,
		8192u,
		1,
		2u);
	m_presets[QualityIndex(EGraphicsQuality::VeryLow)] = MakeProfile(
		0.5f,
		120u,
		1u,
		ELightShadowQuality::VeryLow,
		0.0f,
		1u,
		{ 512u, 0u, 0u, 0u },
		false,
		0.125f,
		64u,
		2048u,
		2,
		1u);
}

bool Sailor::Settings::GraphicsQualityProfile::IsShadowCascadeActive(
	uint32_t cascadeIndex) const noexcept
{
	return cascadeIndex < m_shadowCascadeCount && cascadeIndex < MaxShadowCascades;
}

uint32_t Sailor::Settings::GraphicsQualityProfile::GetShadowCascadeResolution(
	uint32_t cascadeIndex) const noexcept
{
	return IsShadowCascadeActive(cascadeIndex) ? m_shadowCascadeResolutions[cascadeIndex] : 0u;
}

const Sailor::Settings::GraphicsQualityProfile& Sailor::Settings::GraphicsSettings::GetProfile(
	EGraphicsQuality quality) const noexcept
{
	return m_presets[QualityIndex(quality)];
}

const Sailor::Settings::GraphicsQualityProfile& Sailor::Settings::GraphicsSettingsState::GetActiveProfile() const noexcept
{
	return m_projectSettings.GetProfile(m_activeQuality);
}

const char* Sailor::Settings::ToString(EGraphicsQuality quality) noexcept
{
	switch (quality)
	{
	case EGraphicsQuality::Ultra: return "Ultra";
	case EGraphicsQuality::High: return "High";
	case EGraphicsQuality::Medium: return "Medium";
	case EGraphicsQuality::Low: return "Low";
	case EGraphicsQuality::VeryLow: return "VeryLow";
	default: return "High";
	}
}

const char* Sailor::Settings::ToString(EGraphicsQualitySelection selection) noexcept
{
	switch (selection)
	{
	case EGraphicsQualitySelection::ProjectDefault: return "ProjectDefault";
	case EGraphicsQualitySelection::Ultra: return "Ultra";
	case EGraphicsQualitySelection::High: return "High";
	case EGraphicsQualitySelection::Medium: return "Medium";
	case EGraphicsQualitySelection::Low: return "Low";
	case EGraphicsQualitySelection::VeryLow: return "VeryLow";
	default: return "ProjectDefault";
	}
}

const char* Sailor::Settings::ToString(ERenderStatsMode mode) noexcept
{
	switch (mode)
	{
	case ERenderStatsMode::None: return "None";
	case ERenderStatsMode::RenderStats: return "RenderStats";
	case ERenderStatsMode::RenderStatsAndQueries: return "RenderStatsAndQueries";
	default: return "None";
	}
}

bool Sailor::Settings::IsValidRenderStatsMode(ERenderStatsMode mode) noexcept
{
	return mode == ERenderStatsMode::None ||
		mode == ERenderStatsMode::RenderStats ||
		mode == ERenderStatsMode::RenderStatsAndQueries;
}

Sailor::Settings::EGraphicsQuality Sailor::Settings::ResolveQualitySelection(
	EGraphicsQualitySelection selection,
	EGraphicsQuality projectDefault) noexcept
{
	switch (selection)
	{
	case EGraphicsQualitySelection::Ultra: return EGraphicsQuality::Ultra;
	case EGraphicsQualitySelection::High: return EGraphicsQuality::High;
	case EGraphicsQualitySelection::Medium: return EGraphicsQuality::Medium;
	case EGraphicsQualitySelection::Low: return EGraphicsQuality::Low;
	case EGraphicsQualitySelection::VeryLow: return EGraphicsQuality::VeryLow;
	case EGraphicsQualitySelection::ProjectDefault:
	default:
		return projectDefault;
	}
}

Sailor::RHI::EMsaaSamples Sailor::Settings::ToMsaaSamples(uint32_t samples) noexcept
{
	switch (samples)
	{
	case 2u: return RHI::EMsaaSamples::Samples_2;
	case 4u: return RHI::EMsaaSamples::Samples_4;
	case 8u: return RHI::EMsaaSamples::Samples_8;
	case 1u:
	default:
		return RHI::EMsaaSamples::Samples_1;
	}
}

Sailor::Settings::GraphicsExtent Sailor::Settings::ResolveRenderDimensions(
	uint32_t outputWidth,
	uint32_t outputHeight,
	float resolutionFactor) noexcept
{
	return
	{
		ScaleDimension(outputWidth, resolutionFactor),
		ScaleDimension(outputHeight, resolutionFactor)
	};
}

Sailor::Settings::GraphicsExtent Sailor::Settings::ResolveSkyExtent(
	const GraphicsQualityProfile& profile) noexcept
{
	const uint32_t resolution = (std::max)(1u, profile.m_skyResolution);
	return { resolution, resolution };
}

Sailor::Settings::GraphicsExtent Sailor::Settings::ResolveCloudsExtent(
	uint32_t renderWidth,
	uint32_t renderHeight,
	const GraphicsQualityProfile& profile,
	float platformMultiplier) noexcept
{
	const uint32_t shortestDimension = (std::min)(renderWidth, renderHeight);
	const float scale = profile.m_cloudsResolutionMultiplier * platformMultiplier;
	const uint32_t resolution = ScaleDimension(shortestDimension, scale);
	return { resolution, resolution };
}

Sailor::ELightShadowQuality Sailor::Settings::ApplyShadowQualityCap(
	ELightShadowQuality authoredQuality,
	ELightShadowQuality qualityCap) noexcept
{
	const uint8_t highestQuality = static_cast<uint8_t>(ELightShadowQuality::High);
	const uint8_t authored = (std::min)(static_cast<uint8_t>(authoredQuality), highestQuality);
	const uint8_t cap = (std::min)(static_cast<uint8_t>(qualityCap), highestQuality);
	return static_cast<ELightShadowQuality>((std::min)(authored, cap));
}

uint32_t Sailor::Settings::ApplyLodBias(
	uint32_t selectedLod,
	uint32_t numAvailableLods,
	uint32_t authoredMinLod,
	uint32_t authoredMaxLod,
	int32_t lodBias) noexcept
{
	if (numAvailableLods == 0u)
	{
		return 0u;
	}

	const uint32_t highestAvailableLod = numAvailableLods - 1u;
	const uint32_t minLod = (std::min)(authoredMinLod, highestAvailableLod);
	const uint32_t maxLod = (std::max)(minLod, (std::min)(authoredMaxLod, highestAvailableLod));
	const int64_t selected = static_cast<int64_t>((std::clamp)(selectedLod, minLod, maxLod));
	const int64_t biased = selected + static_cast<int64_t>(lodBias);
	return static_cast<uint32_t>((std::clamp)(
		biased,
		static_cast<int64_t>(minLod),
		static_cast<int64_t>(maxLod)));
}

Sailor::Settings::ProjectGraphicsSettingsLoadResult Sailor::Settings::ParseProjectGraphicsSettings(
	const std::string& payload,
	const std::string& sourceName) noexcept
{
	ProjectGraphicsSettingsLoadResult result;
	const std::string source = SourceLabel(sourceName, "ProjectSettings.yaml");
	YAML::Node document;
	std::string yamlDiagnostic;
	if (!TryLoadSingleDocument(payload, document, yamlDiagnostic))
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
	const bool bLegacyProjectSettings = parsedSettings.m_version ==
		LegacyProjectGraphicsSettingsVersion;
	if (!bLegacyProjectSettings &&
		parsedSettings.m_version != ProjectGraphicsSettingsVersion)
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
			result.m_diagnostic) ||
		!ParseQuality(defaultQuality, parsedSettings.m_defaultQuality))
	{
		result.m_status = EGraphicsSettingsLoadStatus::Invalid;
		if (result.m_diagnostic.empty())
		{
			result.m_diagnostic = InvalidField(
				source,
				"graphics.defaultQuality",
				"must be one of Ultra, High, Medium, Low, or VeryLow");
		}
		return result;
	}

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
				bLegacyProjectSettings,
				parsedSettings.m_presets[QualityIndex(quality)],
				result.m_diagnostic))
		{
			result.m_status = EGraphicsSettingsLoadStatus::Invalid;
			return result;
		}
	}
	parsedSettings.m_version = ProjectGraphicsSettingsVersion;

	result.m_settings = std::move(parsedSettings);
	result.m_status = EGraphicsSettingsLoadStatus::Loaded;
	result.m_diagnostic = bLegacyProjectSettings
		? "Loaded " + source +
			" and supplied default GI probe-state budgets while migrating settingsVersion 1 to 2."
		: "Loaded " + source + ".";
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
	if (!TryLoadSingleDocument(payload, document, yamlDiagnostic))
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
			result.m_diagnostic) ||
		!ParseQualitySelection(selectedQuality, parsedSettings.m_selectedQuality))
	{
		result.m_status = EGraphicsSettingsLoadStatus::Invalid;
		if (result.m_diagnostic.empty())
		{
			result.m_diagnostic = InvalidField(
				source,
				"graphics.selectedQuality",
				"must be one of ProjectDefault, Ultra, High, Medium, Low, or VeryLow");
		}
		return result;
	}

	std::string statsMode;
	if (!ReadString(
			graphics,
			"statsMode",
			source,
			"graphics.statsMode",
			statsMode,
			result.m_diagnostic) ||
		!ParseStatsMode(statsMode, parsedSettings.m_statsMode))
	{
		result.m_status = EGraphicsSettingsLoadStatus::Invalid;
		if (result.m_diagnostic.empty())
		{
			result.m_diagnostic = InvalidField(
				source,
				"graphics.statsMode",
				"must be one of None, RenderStats, or RenderStatsAndQueries");
		}
		return result;
	}

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
