#include "Workspace/WorkspaceCacheContract.h"
#include "Containers/Containers.h"

#include "Core/YamlUtils.h"
#include "Workspace/WorkspaceContext.h"
#include "Workspace/WorkspaceModuleApi.h"
#include "YamlExceptionBoundary.h"

#include <algorithm>
#include <cctype>
#include <charconv>
#include <fstream>
#include <sstream>
#include <yaml-cpp/yaml.h>

#if !defined(SAILOR_ENGINE_VERSION)
#define SAILOR_ENGINE_VERSION "unknown"
#endif

#if !defined(SAILOR_BUILD_CONFIG)
#define SAILOR_BUILD_CONFIG "unknown"
#endif

namespace
{
	using Sailor::TSet;
	using namespace Sailor::Workspace;

	constexpr const char* CacheVersionField = "cacheVersion";
	constexpr const char* PayloadVersionField = "payloadVersion";
	constexpr const char* CacheKindField = "cacheKind";
	constexpr const char* ProducerIdentityField = "producerIdentity";
	constexpr const char* WorkspaceIdField = "workspaceId";
	constexpr const char* EngineVersionField = "engineVersion";
	constexpr const char* BuildIdentityField = "buildIdentity";
	constexpr const char* PayloadField = "payload";

	const TSet<std::string> EnvelopeFields =
	{
		CacheVersionField,
		PayloadVersionField,
		CacheKindField,
		ProducerIdentityField,
		WorkspaceIdField,
		EngineVersionField,
		BuildIdentityField,
		PayloadField
	};

	WorkspaceCacheLoadResult Fail(
		EWorkspaceCacheLoadStatus status,
		std::string diagnostic) noexcept
	{
		WorkspaceCacheLoadResult result;
		result.m_status = status;
		result.m_diagnostic = std::move(diagnostic);
		return result;
	}

	std::string Quote(const std::string& value)
	{
		return "'" + value + "'";
	}

	std::string SourceLabel(const std::string& sourceName)
	{
		return sourceName.empty() ? "workspace cache" : sourceName;
	}

	bool ValidateMapKeys(
		const YAML::Node& document,
		const std::string& sourceName,
		std::string& outDiagnostic)
	{
		const Sailor::Utils::YamlMapValidationResult validation =
			Sailor::Utils::ValidateYamlMap(document);
		if (validation.m_error ==
			Sailor::Utils::EYamlMapValidationError::NonScalarKey)
		{
			outDiagnostic = sourceName +
				" is corrupt: its envelope contains a non-scalar field name.";
			return false;
		}
		if (validation.m_error ==
				Sailor::Utils::EYamlMapValidationError::EmptyKey ||
			validation.m_error ==
				Sailor::Utils::EYamlMapValidationError::DuplicateKey)
		{
			outDiagnostic = sourceName +
				" is corrupt: its envelope contains duplicate or empty field " +
				Quote(validation.m_fieldName) + ".";
			return false;
		}

		return true;
	}

	bool ValidateCurrentEnvelopeFields(
		const YAML::Node& document,
		const std::string& sourceName,
		std::string& outDiagnostic)
	{
		for (const auto& field : document)
		{
			const std::string key = field.first.Scalar();
			if (!EnvelopeFields.Contains(key))
			{
				outDiagnostic = sourceName + " is corrupt: its current envelope contains unknown field " +
					Quote(key) + ".";
				return false;
			}
		}

		for (const std::string& requiredField : EnvelopeFields)
		{
			if (!Sailor::Utils::FindYamlMapField(
					document,
					requiredField).IsDefined())
			{
				outDiagnostic = sourceName + " is corrupt: its current envelope is missing required field " +
					Quote(requiredField) + ".";
				return false;
			}
		}

		return true;
	}

	bool ReadUint32(
		const YAML::Node& field,
		const char* fieldName,
		const std::string& sourceName,
		uint32_t& outValue,
		std::string& outDiagnostic)
	{
		if (!field.IsDefined() || !field.IsScalar())
		{
			outDiagnostic = sourceName + " is corrupt: field " + Quote(fieldName) +
				" must be an unsigned integer scalar.";
			return false;
		}

		const std::string scalar = field.Scalar();
		if (scalar.empty() ||
			!std::all_of(scalar.begin(), scalar.end(), [](unsigned char character)
				{
					return std::isdigit(character) != 0;
				}))
		{
			outDiagnostic = sourceName + " is corrupt: field " + Quote(fieldName) +
				" must be an unsigned integer scalar.";
			return false;
		}

		const char* begin = scalar.data();
		const char* end = begin + scalar.size();
		const auto parsed = std::from_chars(begin, end, outValue);
		if (parsed.ec != std::errc() || parsed.ptr != end)
		{
			outDiagnostic = sourceName + " is corrupt: field " + Quote(fieldName) +
				" must be an unsigned 32-bit integer scalar.";
			return false;
		}
		return true;
	}

	bool ReadRequiredString(
		const YAML::Node& document,
		const char* fieldName,
		const std::string& sourceName,
		std::string& outValue,
		std::string& outDiagnostic,
		bool bAllowEmpty = false)
	{
		const YAML::Node field = Sailor::Utils::FindYamlMapField(
			document,
			fieldName);
		if (!field.IsDefined() || !field.IsScalar())
		{
			outDiagnostic = sourceName + " is corrupt: field " + Quote(fieldName) +
				" must be a scalar.";
			return false;
		}

		outValue = field.Scalar();
		if (!bAllowEmpty && outValue.empty())
		{
			outDiagnostic = sourceName + " is corrupt: field " + Quote(fieldName) +
				" cannot be empty.";
			return false;
		}
		return true;
	}

	WorkspaceCacheLoadResult VersionMismatch(
		const std::string& sourceName,
		const char* fieldName,
		uint32_t expected,
		const std::string& actual)
	{
		return Fail(
			EWorkspaceCacheLoadStatus::UnsupportedVersion,
			sourceName + " has unsupported " + fieldName +
			" (expected " + Quote(std::to_string(expected)) +
			", actual " + Quote(actual) + ").");
	}

	WorkspaceCacheLoadResult IdentityMismatch(
		const std::string& sourceName,
		const char* fieldName,
		const std::string& expected,
		const std::string& actual)
	{
		return Fail(
			EWorkspaceCacheLoadStatus::StaleIdentity,
			sourceName + " has stale identity field " + Quote(fieldName) +
			" (expected " + Quote(expected) + ", actual " + Quote(actual) + ").");
	}

	bool ValidateIdentityForSerialization(
		const WorkspaceCacheIdentity& identity,
		std::string& outDiagnostic)
	{
		if (identity.m_cacheVersion != WorkspaceCacheFormatVersion)
		{
			outDiagnostic = "Cannot serialize workspace cache envelope: cacheVersion must be " +
				Quote(std::to_string(WorkspaceCacheFormatVersion)) + ", actual " +
				Quote(std::to_string(identity.m_cacheVersion)) + ".";
			return false;
		}
		if (identity.m_payloadVersion == 0)
		{
			outDiagnostic = "Cannot serialize workspace cache envelope: payloadVersion must be greater than zero.";
			return false;
		}

		const std::pair<const char*, const std::string*> fields[] =
		{
			{ CacheKindField, &identity.m_cacheKind },
			{ ProducerIdentityField, &identity.m_producerIdentity },
			{ WorkspaceIdField, &identity.m_workspaceId },
			{ EngineVersionField, &identity.m_engineVersion },
			{ BuildIdentityField, &identity.m_buildIdentity }
		};
		for (const auto& [fieldName, value] : fields)
		{
			if (value->empty())
			{
				outDiagnostic = "Cannot serialize workspace cache envelope: field " +
					Quote(fieldName) + " cannot be empty.";
				return false;
			}
		}

		return true;
	}

	std::filesystem::path NormalizeLegacyRoot(const std::filesystem::path& root)
	{
		std::error_code error;
		std::filesystem::path normalized = std::filesystem::weakly_canonical(root, error);
		if (error)
		{
			error.clear();
			normalized = std::filesystem::absolute(root, error);
			if (error)
			{
				normalized = root;
			}
		}
		return normalized.lexically_normal();
	}

	std::string GenericUtf8String(const std::filesystem::path& path)
	{
		const std::u8string utf8 = path.generic_u8string();
		return std::string(
			reinterpret_cast<const char*>(utf8.data()),
			utf8.size());
	}

#if defined(_WIN32)
	void FoldAsciiCaseForWindows(std::string& value) noexcept
	{
		for (char& character : value)
		{
			if (character >= 'A' && character <= 'Z')
			{
				character = static_cast<char>(character + ('a' - 'A'));
			}
		}
	}
#endif


}

using namespace Sailor::Workspace;

std::string Sailor::Workspace::ResolveWorkspaceCacheIdentity(
	const std::string& workspaceId,
	const std::filesystem::path& canonicalWorkspaceRoot)
{
	if (!workspaceId.empty())
	{
		return workspaceId;
	}

	std::string normalizedRoot = GenericUtf8String(NormalizeLegacyRoot(canonicalWorkspaceRoot));
#if defined(_WIN32)
	FoldAsciiCaseForWindows(normalizedRoot);
#endif
	return "legacy-root:" + normalizedRoot;
}

const std::string& Sailor::Workspace::GetWorkspaceCacheEngineVersion()
{
	static const std::string EngineVersion = SAILOR_ENGINE_VERSION;
	return EngineVersion;
}

const std::string& Sailor::Workspace::GetWorkspaceCacheBuildIdentity()
{
	static const std::string BuildIdentity =
		std::string("config=") + SAILOR_BUILD_CONFIG + ";" + GetWorkspaceModuleAbiTagV1();
	return BuildIdentity;
}

WorkspaceCacheIdentity Sailor::Workspace::MakeWorkspaceCacheIdentity(
	const std::string& cacheKind,
	const std::string& producerIdentity,
	uint32_t payloadVersion,
	const WorkspaceContext& workspaceContext)
{
	return MakeWorkspaceCacheIdentity(
		cacheKind,
		producerIdentity,
		payloadVersion,
		workspaceContext.GetWorkspaceId(),
		workspaceContext.GetRoot());
}

WorkspaceCacheIdentity Sailor::Workspace::MakeWorkspaceCacheIdentity(
	const std::string& cacheKind,
	const std::string& producerIdentity,
	uint32_t payloadVersion,
	const std::string& workspaceId,
	const std::filesystem::path& canonicalWorkspaceRoot)
{
	WorkspaceCacheIdentity identity;
	identity.m_cacheVersion = WorkspaceCacheFormatVersion;
	identity.m_payloadVersion = payloadVersion;
	identity.m_cacheKind = cacheKind;
	identity.m_producerIdentity = producerIdentity;
	identity.m_workspaceId = ResolveWorkspaceCacheIdentity(workspaceId, canonicalWorkspaceRoot);
	identity.m_engineVersion = GetWorkspaceCacheEngineVersion();
	identity.m_buildIdentity = GetWorkspaceCacheBuildIdentity();
	return identity;
}

bool Sailor::Workspace::SerializeWorkspaceCacheEnvelope(
	const WorkspaceCacheIdentity& identity,
	const std::string& payload,
	std::string& outEnvelope,
	std::string& outDiagnostic) noexcept
{
	outEnvelope.clear();
	outDiagnostic.clear();
	if (!ValidateIdentityForSerialization(identity, outDiagnostic))
	{
		return false;
	}

	YAML::Node document;
	document[CacheVersionField] = identity.m_cacheVersion;
	document[PayloadVersionField] = identity.m_payloadVersion;
	document[CacheKindField] = identity.m_cacheKind;
	document[ProducerIdentityField] = identity.m_producerIdentity;
	document[WorkspaceIdField] = identity.m_workspaceId;
	document[EngineVersionField] = identity.m_engineVersion;
	document[BuildIdentityField] = identity.m_buildIdentity;
	document[PayloadField] = payload;
	std::string yamlDiagnostic;
	if (!Sailor::External::TryDumpYaml(document, outEnvelope, yamlDiagnostic))
	{
		outEnvelope.clear();
		outDiagnostic = "Cannot serialize workspace cache envelope: " + yamlDiagnostic;
		return false;
	}
	return true;
}

WorkspaceCacheLoadResult Sailor::Workspace::ParseWorkspaceCacheEnvelope(
	const std::string& envelope,
	const WorkspaceCacheIdentity& expectedIdentity,
	const std::string& sourceName) noexcept
{
	const std::string source = SourceLabel(sourceName);
	YAML::Node document;
	std::string yamlDiagnostic;
	if (!Sailor::External::TryLoadYaml(envelope, document, yamlDiagnostic))
	{
		return Fail(
			EWorkspaceCacheLoadStatus::Corrupt,
			source + " is corrupt: invalid YAML: " + yamlDiagnostic);
	}
	if (!document.IsMap())
	{
		return Fail(
			EWorkspaceCacheLoadStatus::Corrupt,
			source + " is corrupt: its envelope must be a YAML map.");
	}

	std::string diagnostic;
	if (!ValidateMapKeys(document, source, diagnostic))
	{
		return Fail(EWorkspaceCacheLoadStatus::Corrupt, std::move(diagnostic));
	}

	const YAML::Node cacheVersionField = Sailor::Utils::FindYamlMapField(
		document,
		CacheVersionField);
	if (!cacheVersionField.IsDefined())
	{
		return VersionMismatch(
			source,
			CacheVersionField,
			WorkspaceCacheFormatVersion,
			"missing");
	}

	uint32_t cacheVersion = 0;
	if (!ReadUint32(
			cacheVersionField,
			CacheVersionField,
			source,
			cacheVersion,
			diagnostic))
	{
		return Fail(EWorkspaceCacheLoadStatus::Corrupt, std::move(diagnostic));
	}
	if (cacheVersion != WorkspaceCacheFormatVersion)
	{
		return VersionMismatch(
			source,
			CacheVersionField,
			WorkspaceCacheFormatVersion,
			std::to_string(cacheVersion));
	}

	const YAML::Node payloadVersionField = Sailor::Utils::FindYamlMapField(
		document,
		PayloadVersionField);
	if (!payloadVersionField.IsDefined())
	{
		return VersionMismatch(
			source,
			PayloadVersionField,
			expectedIdentity.m_payloadVersion,
			"missing");
	}

	uint32_t payloadVersion = 0;
	if (!ReadUint32(
			payloadVersionField,
			PayloadVersionField,
			source,
			payloadVersion,
			diagnostic))
	{
		return Fail(EWorkspaceCacheLoadStatus::Corrupt, std::move(diagnostic));
	}
	if (payloadVersion == 0 || payloadVersion != expectedIdentity.m_payloadVersion)
	{
		return VersionMismatch(
			source,
			PayloadVersionField,
			expectedIdentity.m_payloadVersion,
			std::to_string(payloadVersion));
	}

	if (!ValidateCurrentEnvelopeFields(document, source, diagnostic))
	{
		return Fail(EWorkspaceCacheLoadStatus::Corrupt, std::move(diagnostic));
	}

	std::string cacheKind;
	std::string producerIdentity;
	std::string workspaceId;
	std::string engineVersion;
	std::string buildIdentity;
	if (!ReadRequiredString(document, CacheKindField, source, cacheKind, diagnostic) ||
		!ReadRequiredString(document, ProducerIdentityField, source, producerIdentity, diagnostic) ||
		!ReadRequiredString(document, WorkspaceIdField, source, workspaceId, diagnostic) ||
		!ReadRequiredString(document, EngineVersionField, source, engineVersion, diagnostic) ||
		!ReadRequiredString(document, BuildIdentityField, source, buildIdentity, diagnostic))
	{
		return Fail(EWorkspaceCacheLoadStatus::Corrupt, std::move(diagnostic));
	}

	if (cacheKind != expectedIdentity.m_cacheKind)
	{
		return IdentityMismatch(source, CacheKindField, expectedIdentity.m_cacheKind, cacheKind);
	}
	if (producerIdentity != expectedIdentity.m_producerIdentity)
	{
		return IdentityMismatch(
			source,
			ProducerIdentityField,
			expectedIdentity.m_producerIdentity,
			producerIdentity);
	}
	if (workspaceId != expectedIdentity.m_workspaceId)
	{
		return IdentityMismatch(source, WorkspaceIdField, expectedIdentity.m_workspaceId, workspaceId);
	}
	if (engineVersion != expectedIdentity.m_engineVersion)
	{
		return IdentityMismatch(
			source,
			EngineVersionField,
			expectedIdentity.m_engineVersion,
			engineVersion);
	}
	if (buildIdentity != expectedIdentity.m_buildIdentity)
	{
		return IdentityMismatch(
			source,
			BuildIdentityField,
			expectedIdentity.m_buildIdentity,
			buildIdentity);
	}

	std::string payload;
	if (!ReadRequiredString(document, PayloadField, source, payload, diagnostic, true))
	{
		return Fail(EWorkspaceCacheLoadStatus::Corrupt, std::move(diagnostic));
	}

	WorkspaceCacheLoadResult result;
	result.m_status = EWorkspaceCacheLoadStatus::Loaded;
	result.m_diagnostic = source + " loaded with matching workspace and producer identity.";
	result.m_payload = std::move(payload);
	return result;
}

WorkspaceCacheLoadResult Sailor::Workspace::LoadWorkspaceCacheEnvelope(
	const std::filesystem::path& path,
	const WorkspaceCacheIdentity& expectedIdentity) noexcept
{
	std::error_code error;
	const bool exists = std::filesystem::exists(path, error);
	if (error)
	{
		return Fail(
			EWorkspaceCacheLoadStatus::IoFailure,
			"Cannot inspect workspace cache " + Quote(path.generic_string()) + ": " +
				error.message() + ".");
	}
	if (!exists)
	{
		return Fail(
			EWorkspaceCacheLoadStatus::Missing,
			"Workspace cache " + Quote(path.generic_string()) + " is missing.");
	}

	if (!std::filesystem::is_regular_file(path, error) || error)
	{
		return Fail(
			EWorkspaceCacheLoadStatus::IoFailure,
			"Workspace cache " + Quote(path.generic_string()) + " is not a readable regular file" +
				(error ? ": " + error.message() : std::string()) + ".");
	}

	std::ifstream stream(path, std::ios::binary);
	if (!stream.is_open())
	{
		return Fail(
			EWorkspaceCacheLoadStatus::IoFailure,
			"Cannot open workspace cache " + Quote(path.generic_string()) + ".");
	}

	std::ostringstream payload;
	payload << stream.rdbuf();
	if (stream.bad())
	{
		return Fail(
			EWorkspaceCacheLoadStatus::IoFailure,
			"Cannot read workspace cache " + Quote(path.generic_string()) + ".");
	}

	return ParseWorkspaceCacheEnvelope(payload.str(), expectedIdentity, path.generic_string());
}
