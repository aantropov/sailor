#include "AssetRegistry/Shader/ShaderCache.h"

#include "AssetRegistry/Shader/ShaderCompiler.h"
#include "YamlExceptionBoundary.h"

#include <string>

using namespace Sailor;

ShaderCache::ShaderCache() = default;

ShaderCache::ShaderCache(const IShaderSourceStateProvider* sourceStateProvider)
	: m_sourceStateProvider(sourceStateProvider)
{
}

ShaderCache::~ShaderCache() = default;

std::string ShaderCache::GetCacheProducerIdentity()
{
	return "shader-compiler-v" + std::to_string(ShaderCompiler::CacheProducerVersion);
}

Workspace::WorkspaceCacheIdentity ShaderCache::MakeExpectedIdentity()
{
	return Workspace::MakeWorkspaceCacheIdentity(
		CacheKind, GetCacheProducerIdentity(), PayloadVersion, App::GetWorkspaceContext());
}

ShaderCache::ArtifactMetadata::ArtifactMetadata() = default;

ShaderCache::ArtifactMetadata::~ArtifactMetadata() = default;

YAML::Node ShaderCache::ArtifactMetadata::Serialize() const
{
	YAML::Node result(YAML::NodeType::Map);
	SERIALIZE_PROPERTY(result, m_byteLength);
	SERIALIZE_PROPERTY(result, m_checksum);
	return result;
}

void ShaderCache::ArtifactMetadata::Deserialize(const YAML::Node& inData)
{
	*this = {};
	std::string yamlDiagnostic;
	if (!Sailor::External::GuardYamlExceptions(
			[&]()
			{
				DESERIALIZE_PROPERTY(inData, m_byteLength);
				DESERIALIZE_PROPERTY(inData, m_checksum);
			},
			yamlDiagnostic))
	{
		*this = {};
	}
}

bool ShaderCache::ArtifactMetadata::Validate(std::string_view context, std::string& outDiagnostic) const
{
	if (!IsPresent() && m_checksum != 0)
	{
		outDiagnostic = std::string(context) + " has a checksum for an absent artifact.";
		return false;
	}
	if (IsPresent() && m_byteLength % sizeof(uint32_t) != 0)
	{
		outDiagnostic = std::string(context) + " byteLength is not aligned to uint32 SPIR-V words.";
		return false;
	}
	return true;
}

YAML::Node ShaderCache::ArtifactSet::Serialize() const
{
	YAML::Node result(YAML::NodeType::Map);
	SERIALIZE_PROPERTY(result, m_vertex);
	SERIALIZE_PROPERTY(result, m_fragment);
	SERIALIZE_PROPERTY(result, m_compute);
	return result;
}

void ShaderCache::ArtifactSet::Deserialize(const YAML::Node& inData)
{
	*this = {};
	std::string yamlDiagnostic;
	if (!Sailor::External::GuardYamlExceptions(
			[&]()
			{
				DESERIALIZE_PROPERTY(inData, m_vertex);
				DESERIALIZE_PROPERTY(inData, m_fragment);
				DESERIALIZE_PROPERTY(inData, m_compute);
			},
			yamlDiagnostic))
	{
		*this = {};
	}
}

YAML::Node ShaderCache::ShaderCacheData::Entry::Serialize() const
{
	YAML::Node result(YAML::NodeType::Map);
	SERIALIZE_PROPERTY(result, m_fileId);
	SERIALIZE_PROPERTY(result, m_timestamp);
	SERIALIZE_PROPERTY(result, m_sourceFingerprint);
	SERIALIZE_PROPERTY(result, m_permutation);
	SERIALIZE_PROPERTY(result, m_generation);
	SERIALIZE_PROPERTY(result, m_regular);
	SERIALIZE_PROPERTY(result, m_debug);
	return result;
}

void ShaderCache::ShaderCacheData::Entry::Deserialize(const YAML::Node& inData)
{
	*this = {};
	std::string yamlDiagnostic;
	if (!Sailor::External::GuardYamlExceptions(
			[&]()
			{
				DESERIALIZE_PROPERTY(inData, m_fileId);
				DESERIALIZE_PROPERTY(inData, m_timestamp);
				DESERIALIZE_PROPERTY(inData, m_sourceFingerprint);
				DESERIALIZE_PROPERTY(inData, m_permutation);
				DESERIALIZE_PROPERTY(inData, m_generation);
				DESERIALIZE_PROPERTY(inData, m_regular);
				DESERIALIZE_PROPERTY(inData, m_debug);
			},
			yamlDiagnostic))
	{
		*this = {};
	}
}

bool ShaderCache::ShaderCacheData::Entry::Validate(const FileId& key, std::string& outDiagnostic) const
{
	const auto fail = [&](std::string_view reason)
	{
		std::string message = "Shader cache entry '" + key.ToString() + "' ";
		message += reason;
		outDiagnostic = std::move(message);
		return false;
	};
	if (!m_fileId || m_fileId != key)
	{
		return fail("has a mismatched fileId field.");
	}
	if (!IsValidGeneration(m_generation))
	{
		return fail("has an invalid immutable artifact generation.");
	}
	if (!m_regular.m_vertex.Validate("regular vertex artifact", outDiagnostic) ||
		!m_regular.m_fragment.Validate("regular fragment artifact", outDiagnostic) ||
		!m_regular.m_compute.Validate("regular compute artifact", outDiagnostic) ||
		!m_debug.m_vertex.Validate("debug vertex artifact", outDiagnostic) ||
		!m_debug.m_fragment.Validate("debug fragment artifact", outDiagnostic) ||
		!m_debug.m_compute.Validate("debug compute artifact", outDiagnostic))
	{
		return fail(outDiagnostic);
	}
	if (!IsValidArtifactSet(m_regular, false))
	{
		return fail("regular artifacts must contain a vertex/fragment pair or compute artifact.");
	}
	if (!IsValidArtifactSet(m_debug, false))
	{
		return fail("debug artifacts must contain a vertex/fragment pair or compute artifact.");
	}
	if (!HasMatchingArtifactTopology(m_regular, m_debug))
	{
		return fail("regular and debug artifacts must contain identical shader stages.");
	}
	return true;
}

YAML::Node ShaderCache::ShaderCacheData::Serialize() const
{
	YAML::Node result(YAML::NodeType::Map);
	SERIALIZE_PROPERTY(result, m_entries);
	return result;
}

void ShaderCache::ShaderCacheData::Deserialize(const YAML::Node& inData)
{
	ShaderCacheData candidate;
	std::string diagnostic;
	if (TryDeserialize(inData, candidate, diagnostic))
	{
		m_entries = std::move(candidate.m_entries);
	}
	else
	{
		m_entries.Clear();
	}
}

bool ShaderCache::ShaderCacheData::DeserializeProperties(const YAML::Node& inData)
{
	return DESERIALIZE_PROPERTY(inData, m_entries);
}

bool ShaderCache::ShaderCacheData::Validate(std::string& outDiagnostic) const
{
	for (const auto& fileEntries : m_entries)
	{
		const FileId& fileId = fileEntries.m_first;
		const TVector<Entry>& entries = *fileEntries.m_second;
		if (!fileId || entries.IsEmpty())
		{
			outDiagnostic = "Shader cache entries require a valid file id and at least one permutation.";
			return false;
		}

		TSet<uint32_t> permutations;
		for (const Entry& entry : entries)
		{
			if (!permutations.Insert(entry.m_permutation))
			{
				outDiagnostic = "Shader cache entry '" + fileId.ToString() + "' duplicates permutation " +
								std::to_string(entry.m_permutation) + ".";
				return false;
			}
			if (!entry.Validate(fileId, outDiagnostic))
			{
				return false;
			}
		}
	}
	return true;
}

bool ShaderCache::ShaderCacheData::TryDeserialize(const YAML::Node& inData,
	ShaderCacheData& outData,
	std::string& outDiagnostic) noexcept
{
	auto deserialize = [&]() -> bool
	{
		ShaderCacheData candidate;
		if (!candidate.DeserializeProperties(inData) || !candidate.Validate(outDiagnostic))
		{
			if (outDiagnostic.empty())
			{
				outDiagnostic = "Shader cache payload is missing required data.";
			}
			return false;
		}

		outData.m_entries = std::move(candidate.m_entries);
		outDiagnostic.clear();
		return true;
	};

	bool bResult = false;
	std::string yamlDiagnostic;
	if (!Sailor::External::TryInvokeYaml(deserialize, bResult, yamlDiagnostic))
	{
		outDiagnostic = "Shader cache data contains invalid YAML values: " + yamlDiagnostic;
		return false;
	}
	return bResult;
}

std::string ShaderCache::SerializeShaderCachePayload(const ShaderCacheData& cache)
{
	YAML::Node payload(YAML::NodeType::Map);
	payload["shaderCache"] = cache.Serialize();
	return YAML::Dump(payload);
}

bool ShaderCache::TryDeserializeShaderCachePayload(const std::string& payload,
	ShaderCacheData& outData,
	std::string& outDiagnostic) noexcept
{
	auto deserialize = [&]() -> bool
	{
		const YAML::Node root = YAML::Load(payload);
		const YAML::Node shaderCache = root["shaderCache"];
		if (!shaderCache)
		{
			outDiagnostic = "Shader cache payload is missing 'shaderCache'.";
			return false;
		}
		return ShaderCacheData::TryDeserialize(shaderCache, outData, outDiagnostic);
	};

	bool bResult = false;
	std::string yamlDiagnostic;
	if (!Sailor::External::TryInvokeYaml(deserialize, bResult, yamlDiagnostic))
	{
		outDiagnostic = "Shader cache payload contains invalid YAML: " + yamlDiagnostic;
		return false;
	}
	return bResult;
}
