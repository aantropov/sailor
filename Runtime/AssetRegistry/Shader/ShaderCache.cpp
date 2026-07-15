#include "ShaderCache.h"
#include "Containers/Containers.h"

#include "AssetRegistry/AssetRegistry.h"
#include "AssetRegistry/Shader/ShaderCompiler.h"
#include "Sailor.h"
#include "YamlExceptionBoundary.h"

#include <algorithm>
#include <cctype>
#include <cstring>
#include <fstream>
#include <iomanip>
#include <limits>
#include <random>
#include <sstream>
#include <unordered_set>

using namespace Sailor;

namespace
{
	constexpr const char* ShaderCacheKind = "shader-cache";
	constexpr uint32_t ShaderCachePayloadVersion = 2;
	constexpr uint64_t FnvOffsetBasis = 14695981039346656037ull;
	constexpr uint64_t FnvPrime = 1099511628211ull;

	std::string GetShaderCacheProducerIdentity()
	{
		return "shader-compiler-v" + std::to_string(ShaderCompiler::CacheProducerVersion);
	}

	Workspace::WorkspaceCacheIdentity MakeExpectedIdentity()
	{
		return Workspace::MakeWorkspaceCacheIdentity(
			ShaderCacheKind,
			GetShaderCacheProducerIdentity(),
			ShaderCachePayloadVersion,
			App::GetWorkspaceContext());
	}

	std::filesystem::path GetCacheChildPath(const char* child)
	{
		return std::filesystem::path(AssetRegistry::GetCacheFolder()) / child;
	}

	std::filesystem::path GetShaderFilepath(
		const std::filesystem::path& folder,
		const FileId& uid,
		int32_t permutation,
		const std::string& shaderKind,
		const char* extension,
		const std::string& generation = {})
	{
		const std::string filename = uid.ToString() + shaderKind +
			std::to_string(permutation) +
			(generation.empty() ? std::string() : "." + generation) +
			"." + extension;
		return folder / filename;
	}

	void AppendDiagnostic(std::string& diagnostic, const std::string& suffix)
	{
		if (suffix.empty())
		{
			return;
		}

		if (!diagnostic.empty())
		{
			diagnostic += " ";
		}
		diagnostic += suffix;
	}

	bool ValidateExactFields(
		const YAML::Node& node,
		std::initializer_list<const char*> expectedFields,
		const std::string& context,
		std::string& outDiagnostic)
	{
		if (!node.IsMap())
		{
			outDiagnostic = context + " must be a YAML map.";
			return false;
		}

		TSet<std::string> expected;
		for (const char* field : expectedFields)
		{
			expected.Insert(field);
		}

		TSet<std::string> actual;
		for (const auto& field : node)
		{
			if (!field.first.IsScalar())
			{
				outDiagnostic = context + " contains a non-scalar field name.";
				return false;
			}

			const std::string name = field.first.Scalar();
			if (!actual.Insert(name))
			{
				outDiagnostic = context + " contains duplicate field '" + name + "'.";
				return false;
			}
			if (!expected.Contains(name))
			{
				outDiagnostic = context + " contains unknown field '" + name + "'.";
				return false;
			}
		}

		for (const std::string& field : expected)
		{
			if (!actual.Contains(field))
			{
				outDiagnostic = context + " is missing required field '" + field + "'.";
				return false;
			}
		}

		return true;
	}

	YAML::Node FindField(const YAML::Node& node, const char* fieldName)
	{
		for (const auto& field : node)
		{
			if (field.first.IsScalar() && field.first.Scalar() == fieldName)
			{
				return field.second;
			}
		}
		return YAML::Node(YAML::NodeType::Undefined);
	}

	bool PathsEqual(const std::filesystem::path& lhs, const std::filesystem::path& rhs)
	{
		std::string left = lhs.generic_string();
		std::string right = rhs.generic_string();
#if defined(_WIN32)
		std::transform(left.begin(), left.end(), left.begin(), [](unsigned char character)
			{
				return static_cast<char>(std::tolower(character));
			});
		std::transform(right.begin(), right.end(), right.begin(), [](unsigned char character)
			{
				return static_cast<char>(std::tolower(character));
			});
#endif
		return left == right;
	}

	bool ResolveDirectCacheChild(
		const std::filesystem::path& cacheRoot,
		const std::filesystem::path& candidate,
		std::filesystem::path& outCanonical,
		std::string& outDiagnostic)
	{
		std::error_code error;
		const std::filesystem::path canonicalRoot = std::filesystem::weakly_canonical(cacheRoot, error);
		if (error)
		{
			outDiagnostic = "Cannot canonicalize shader cache root '" +
				cacheRoot.generic_string() + "': " + error.message();
			return false;
		}

		error.clear();
		outCanonical = std::filesystem::weakly_canonical(candidate, error);
		if (error)
		{
			outDiagnostic = "Cannot canonicalize shader cache path '" +
				candidate.generic_string() + "': " + error.message();
			return false;
		}

		if (!PathsEqual(outCanonical.parent_path(), canonicalRoot))
		{
			outDiagnostic = "Refusing shader cache access outside canonical cache root: '" +
				candidate.generic_string() + "'.";
			return false;
		}
		return true;
	}

	bool RemovePath(
		const std::filesystem::path& cacheRoot,
		const std::filesystem::path& path,
		bool bRecursive,
		std::string& outDiagnostic)
	{
		std::filesystem::path canonical;
		std::string diagnostic;
		if (!ResolveDirectCacheChild(cacheRoot, path, canonical, diagnostic))
		{
			AppendDiagnostic(outDiagnostic, diagnostic);
			return false;
		}

		std::error_code error;
		if (bRecursive)
		{
			std::filesystem::remove_all(path, error);
		}
		else
		{
			std::filesystem::remove(path, error);
		}
		if (error)
		{
			AppendDiagnostic(
				outDiagnostic,
				"Cannot remove shader cache path '" + path.generic_string() + "': " + error.message());
			return false;
		}
		return true;
	}

	bool ResolveOwnedArtifactPath(
		const std::filesystem::path& cacheRoot,
		const std::filesystem::path& ownedDirectory,
		const std::filesystem::path& artifact,
		std::filesystem::path& outCanonicalArtifact,
		std::string& outDiagnostic,
		bool& outIoFailure)
	{
		outIoFailure = false;
		std::error_code error;
		const auto status = std::filesystem::symlink_status(ownedDirectory, error);
		if (error)
		{
			outIoFailure = error != std::errc::no_such_file_or_directory;
			outDiagnostic = "Cannot inspect owned shader cache directory '" +
				ownedDirectory.generic_string() + "': " + error.message();
			return false;
		}
		if (std::filesystem::is_symlink(status))
		{
			outDiagnostic = "Refusing shader artifact access through symlinked cache directory '" +
				ownedDirectory.generic_string() + "'.";
			return false;
		}

		const std::filesystem::path canonicalRoot = std::filesystem::weakly_canonical(cacheRoot, error);
		if (error)
		{
			outIoFailure = true;
			outDiagnostic = "Cannot canonicalize shader cache root '" +
				cacheRoot.generic_string() + "': " + error.message();
			return false;
		}

		error.clear();
		const std::filesystem::path canonicalDirectory =
			std::filesystem::weakly_canonical(ownedDirectory, error);
		if (error)
		{
			outIoFailure = true;
			outDiagnostic = "Cannot canonicalize owned shader cache directory '" +
				ownedDirectory.generic_string() + "': " + error.message();
			return false;
		}
		if (!PathsEqual(canonicalDirectory.parent_path(), canonicalRoot))
		{
			outDiagnostic = "Refusing shader artifact access outside the canonical cache root: '" +
				artifact.generic_string() + "'.";
			return false;
		}

		error.clear();
		outCanonicalArtifact = std::filesystem::weakly_canonical(artifact, error);
		if (error)
		{
			outIoFailure = error != std::errc::no_such_file_or_directory;
			outDiagnostic = "Cannot canonicalize shader artifact '" +
				artifact.generic_string() + "': " + error.message();
			return false;
		}
		if (!PathsEqual(outCanonicalArtifact.parent_path(), canonicalDirectory))
		{
			outDiagnostic = "Refusing shader artifact access outside owned directory '" +
				canonicalDirectory.generic_string() + "': '" + artifact.generic_string() + "'.";
			return false;
		}
		return true;
	}

	bool RemoveOwnedArtifact(
		const std::filesystem::path& cacheRoot,
		const std::filesystem::path& ownedDirectory,
		const std::filesystem::path& artifact,
		std::string& outDiagnostic)
	{
		std::filesystem::path canonicalArtifact;
		bool ignoredIoFailure = false;
		if (!ResolveOwnedArtifactPath(
			cacheRoot,
			ownedDirectory,
			artifact,
			canonicalArtifact,
			outDiagnostic,
			ignoredIoFailure))
		{
			return false;
		}

		std::error_code error;
		std::filesystem::remove(artifact, error);
		if (error)
		{
			AppendDiagnostic(
				outDiagnostic,
				"Cannot remove shader cache artifact '" + artifact.generic_string() +
					"': " + error.message());
			return false;
		}
		return true;
	}

	bool IsMetadataStructurallyValid(const ShaderCache::ArtifactMetadata& metadata)
	{
		return metadata.m_byteLength != 0 || metadata.m_checksum == 0;
	}

	bool ShouldResetCache(Workspace::EWorkspaceCacheLoadStatus status) noexcept
	{
		return status == Workspace::EWorkspaceCacheLoadStatus::Missing ||
			status == Workspace::EWorkspaceCacheLoadStatus::StaleIdentity ||
			status == Workspace::EWorkspaceCacheLoadStatus::Corrupt ||
			status == Workspace::EWorkspaceCacheLoadStatus::UnsupportedVersion;
	}

	const char* CacheLoadStatusName(Workspace::EWorkspaceCacheLoadStatus status) noexcept
	{
		switch (status)
		{
		case Workspace::EWorkspaceCacheLoadStatus::Missing: return "Missing";
		case Workspace::EWorkspaceCacheLoadStatus::Loaded: return "Loaded";
		case Workspace::EWorkspaceCacheLoadStatus::StaleIdentity: return "StaleIdentity";
		case Workspace::EWorkspaceCacheLoadStatus::Corrupt: return "Corrupt";
		case Workspace::EWorkspaceCacheLoadStatus::UnsupportedVersion: return "UnsupportedVersion";
		case Workspace::EWorkspaceCacheLoadStatus::IoFailure: return "IoFailure";
		default: return "Unknown";
		}
	}
}

ShaderCache::ShaderCache() = default;

ShaderCache::~ShaderCache() = default;

ShaderCache::ArtifactMetadata::ArtifactMetadata() = default;

ShaderCache::ArtifactMetadata::~ArtifactMetadata() = default;

YAML::Node ShaderCache::ArtifactMetadata::Serialize() const
{
	YAML::Node result(YAML::NodeType::Map);
	result["byteLength"] = m_byteLength;
	result["checksum"] = m_checksum;
	return result;
}

void ShaderCache::ArtifactMetadata::Deserialize(const YAML::Node& inData)
{
	std::string yamlDiagnostic;
	if (!Sailor::External::GuardYamlExceptions(
		[&]()
		{
			m_byteLength = inData["byteLength"].as<uint64_t>();
			m_checksum = inData["checksum"].as<uint64_t>();
		},
		yamlDiagnostic))
	{
		m_byteLength = 0;
		m_checksum = 0;
	}
}

YAML::Node ShaderCache::ArtifactSet::Serialize() const
{
	YAML::Node result(YAML::NodeType::Map);
	result["vertex"] = m_vertex.Serialize();
	result["fragment"] = m_fragment.Serialize();
	result["compute"] = m_compute.Serialize();
	return result;
}

void ShaderCache::ArtifactSet::Deserialize(const YAML::Node& inData)
{
	std::string yamlDiagnostic;
	if (!Sailor::External::GuardYamlExceptions(
		[&]()
		{
			m_vertex.Deserialize(inData["vertex"]);
			m_fragment.Deserialize(inData["fragment"]);
			m_compute.Deserialize(inData["compute"]);
		},
		yamlDiagnostic))
	{
		m_vertex = {};
		m_fragment = {};
		m_compute = {};
	}
}

YAML::Node ShaderCache::ShaderCacheData::Entry::Serialize() const
{
	YAML::Node result(YAML::NodeType::Map);
	result["fileId"] = m_fileId;
	result["timestamp"] = m_timestamp;
	result["permutation"] = m_permutation;
	result["generation"] = m_generation;
	result["regular"] = m_regular.Serialize();
	result["debug"] = m_debug.Serialize();
	return result;
}

void ShaderCache::ShaderCacheData::Entry::Deserialize(const YAML::Node& inData)
{
	std::string yamlDiagnostic;
	if (!Sailor::External::GuardYamlExceptions(
		[&]()
		{
			m_fileId = inData["fileId"].as<FileId>();
			m_timestamp = inData["timestamp"].as<std::time_t>();
			m_permutation = inData["permutation"].as<uint32_t>();
			m_generation = inData["generation"].as<std::string>();
			m_regular.Deserialize(inData["regular"]);
			m_debug.Deserialize(inData["debug"]);
		},
		yamlDiagnostic))
	{
		*this = Entry();
	}
}

YAML::Node ShaderCache::ShaderCacheData::Serialize() const
{
	YAML::Node result(YAML::NodeType::Map);
	YAML::Node entries(YAML::NodeType::Map);
	for (const auto& fileEntries : m_data)
	{
		YAML::Node serializedEntries(YAML::NodeType::Sequence);
		for (const Entry& entry : *fileEntries.m_second)
		{
			serializedEntries.push_back(entry.Serialize());
		}
		entries[fileEntries.m_first] = serializedEntries;
	}
	result["entries"] = entries;
	return result;
}

void ShaderCache::ShaderCacheData::Deserialize(const YAML::Node& inData)
{
	std::string yamlDiagnostic;
	if (!Sailor::External::GuardYamlExceptions(
		[&]()
		{
			m_data = inData["entries"].as<TMap<FileId, TVector<Entry>>>();
		},
		yamlDiagnostic))
	{
		m_data.Clear();
	}
}

std::string ShaderCache::SerializeShaderCachePayload(const ShaderCacheData& cache)
{
	YAML::Node payload(YAML::NodeType::Map);
	payload["shaderCache"] = cache.Serialize();
	return YAML::Dump(payload);
}

bool ShaderCache::TryDeserializeShaderCachePayload(
	const std::string& payload,
	ShaderCacheData& outData,
	std::string& outDiagnostic) noexcept
{
	auto deserialize = [&]() -> bool
	{
	const YAML::Node root = YAML::Load(payload);
	if (!ValidateExactFields(root, { "shaderCache" }, "Shader cache payload root", outDiagnostic))
	{
		return false;
	}

	const YAML::Node shaderCache = FindField(root, "shaderCache");
	if (!ValidateExactFields(shaderCache, { "entries" }, "Shader cache payload", outDiagnostic))
	{
		return false;
	}

	const YAML::Node entriesNode = FindField(shaderCache, "entries");
	if (!entriesNode.IsMap())
	{
		outDiagnostic = "Shader cache field 'entries' must be a YAML map, including when empty.";
		return false;
	}

	auto parseMetadata = [&](const YAML::Node& node,
		ArtifactMetadata& outMetadata,
		const std::string& context) -> bool
	{
		if (!ValidateExactFields(node, { "byteLength", "checksum" }, context, outDiagnostic))
		{
			return false;
		}
		const YAML::Node byteLength = FindField(node, "byteLength");
		const YAML::Node checksum = FindField(node, "checksum");
		if (!byteLength.IsScalar() || !checksum.IsScalar())
		{
			outDiagnostic = context + " fields must be unsigned integer scalars.";
			return false;
		}
		outMetadata.m_byteLength = byteLength.as<uint64_t>();
		outMetadata.m_checksum = checksum.as<uint64_t>();
		if (!IsMetadataStructurallyValid(outMetadata))
		{
			outDiagnostic = context + " has a checksum for an absent artifact.";
			return false;
		}
		if (outMetadata.IsPresent() && outMetadata.m_byteLength % sizeof(uint32_t) != 0)
		{
			outDiagnostic = context + " byteLength is not aligned to uint32 SPIR-V words.";
			return false;
		}
		return true;
	};

	auto parseSet = [&](const YAML::Node& node,
		ArtifactSet& outSet,
		const std::string& context) -> bool
	{
		if (!ValidateExactFields(node, { "vertex", "fragment", "compute" }, context, outDiagnostic))
		{
			return false;
		}
		return parseMetadata(FindField(node, "vertex"), outSet.m_vertex, context + " vertex artifact") &&
			parseMetadata(FindField(node, "fragment"), outSet.m_fragment, context + " fragment artifact") &&
			parseMetadata(FindField(node, "compute"), outSet.m_compute, context + " compute artifact");
	};

	ShaderCacheData candidate;
	TSet<std::string> fileIds;
	for (const auto& serializedFileEntries : entriesNode)
	{
		if (!serializedFileEntries.first.IsScalar())
		{
			outDiagnostic = "Shader cache contains a non-scalar file id.";
			return false;
		}

		const std::string serializedFileId = serializedFileEntries.first.Scalar();
		if (!fileIds.Insert(serializedFileId))
		{
			outDiagnostic = "Shader cache contains duplicate file id '" + serializedFileId + "'.";
			return false;
		}

		const FileId fileId = serializedFileEntries.first.as<FileId>();
		if (!fileId)
		{
			outDiagnostic = "Shader cache contains invalid file id '" + serializedFileId + "'.";
			return false;
		}

		const YAML::Node entrySequence = serializedFileEntries.second;
		if (!entrySequence.IsSequence() || entrySequence.size() == 0)
		{
			outDiagnostic = "Shader cache file id '" + serializedFileId +
				"' must contain a non-empty entry sequence.";
			return false;
		}

		TVector<ShaderCacheData::Entry> entries;
		entries.Reserve(entrySequence.size());
		TSet<uint32_t> permutations;
		for (size_t index = 0; index < entrySequence.size(); ++index)
		{
			const std::string context = "Shader cache entry '" + serializedFileId +
				"' at index " + std::to_string(index);
			const YAML::Node entryNode = entrySequence[index];
			if (!ValidateExactFields(
				entryNode,
				{ "fileId", "timestamp", "permutation", "generation", "regular", "debug" },
				context,
				outDiagnostic))
			{
				return false;
			}

			const YAML::Node entryFileId = FindField(entryNode, "fileId");
			const YAML::Node timestamp = FindField(entryNode, "timestamp");
			const YAML::Node permutation = FindField(entryNode, "permutation");
			const YAML::Node generation = FindField(entryNode, "generation");
			if (!entryFileId.IsScalar() || !timestamp.IsScalar() ||
				!permutation.IsScalar() || !generation.IsScalar())
			{
				outDiagnostic = context + " contains a non-scalar identity or timestamp field.";
				return false;
			}

			ShaderCacheData::Entry entry;
			entry.m_fileId = entryFileId.as<FileId>();
			entry.m_timestamp = timestamp.as<std::time_t>();
			entry.m_permutation = permutation.as<uint32_t>();
			entry.m_generation = generation.as<std::string>();
			if (!entry.m_fileId || entry.m_fileId != fileId)
			{
				outDiagnostic = context + " has a mismatched fileId field.";
				return false;
			}
			if (!permutations.Insert(entry.m_permutation))
			{
				outDiagnostic = context + " duplicates permutation " +
					std::to_string(entry.m_permutation) + ".";
				return false;
			}
			if (!IsValidGeneration(entry.m_generation))
			{
				outDiagnostic = context + " has an invalid immutable artifact generation.";
				return false;
			}

			if (!parseSet(FindField(entryNode, "regular"), entry.m_regular, context + " regular artifacts") ||
				!parseSet(FindField(entryNode, "debug"), entry.m_debug, context + " debug artifacts"))
			{
				return false;
			}
			if (!IsValidArtifactSet(entry.m_regular, false))
			{
				outDiagnostic = context + " regular artifacts must contain a vertex/fragment pair or compute artifact.";
				return false;
			}
			if (!IsValidArtifactSet(entry.m_debug, false))
			{
				outDiagnostic = context + " debug artifacts must contain a vertex/fragment pair or compute artifact.";
				return false;
			}
			if (!HasMatchingArtifactTopology(entry.m_regular, entry.m_debug))
			{
				outDiagnostic = context + " regular and debug artifacts must contain identical shader stages.";
				return false;
			}

			entries.Add(std::move(entry));
		}

		candidate.m_data.Insert(fileId, std::move(entries));
	}

	outData.m_data = std::move(candidate.m_data);
	outDiagnostic.clear();
	return true;
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

std::string ShaderCache::GetShaderCacheFilepath()
{
	return GetCacheChildPath("ShaderCache.yaml").string();
}

std::string ShaderCache::GetPrecompiledShadersFolder()
{
	return GetCacheChildPath("PrecompiledShaders").string();
}

std::string ShaderCache::GetCompiledShadersFolder()
{
	return GetCacheChildPath("CompiledShaders").string();
}

std::string ShaderCache::GetCompiledShadersWithDebugFolder()
{
	return GetCacheChildPath("CompiledShadersWithDebug").string();
}

std::filesystem::path ShaderCache::GetPrecompiledShaderFilepath(
	const FileId& uid,
	int32_t permutation,
	const std::string& shaderKind)
{
	return GetShaderFilepath(
		GetCacheChildPath("PrecompiledShaders"),
		uid,
		permutation,
		shaderKind,
		PrecompiledShaderFileExtension);
}

std::filesystem::path ShaderCache::GetCachedShaderFilepath(
	const FileId& uid,
	int32_t permutation,
	const std::string& shaderKind)
{
	return GetShaderFilepath(
		GetCacheChildPath("CompiledShaders"),
		uid,
		permutation,
		shaderKind,
		CompiledShaderFileExtension);
}

std::filesystem::path ShaderCache::GetCachedShaderWithDebugFilepath(
	const FileId& uid,
	int32_t permutation,
	const std::string& shaderKind)
{
	return GetShaderFilepath(
		GetCacheChildPath("CompiledShadersWithDebug"),
		uid,
		permutation,
		shaderKind,
		CompiledShaderFileExtension);
}

uint64_t ShaderCache::CalculateArtifactChecksum(const void* data, uint64_t size) noexcept
{
	if (size != 0 && data == nullptr)
	{
		return 0;
	}

	uint64_t checksum = FnvOffsetBasis;
	const auto* bytes = static_cast<const uint8_t*>(data);
	for (uint64_t index = 0; index < size; ++index)
	{
		checksum ^= bytes[index];
		checksum *= FnvPrime;
	}
	return checksum;
}

ShaderCache::ArtifactMetadata ShaderCache::DescribeArtifact(const void* data, uint64_t size) noexcept
{
	ArtifactMetadata result;
	if (size == 0 || data == nullptr)
	{
		return result;
	}

	result.m_byteLength = size;
	result.m_checksum = CalculateArtifactChecksum(data, size);
	return result;
}

bool ShaderCache::ReadArtifactBytes(
	const std::filesystem::path& path,
	const ArtifactMetadata& metadata,
	TVector<uint8_t>& outBytes,
	std::string& outDiagnostic,
	bool& outIoFailure) noexcept
{
	outIoFailure = false;
	if (!IsMetadataStructurallyValid(metadata))
	{
		outDiagnostic = "Artifact metadata is internally inconsistent for '" + path.generic_string() + "'.";
		return false;
	}
	if (!metadata.IsPresent())
	{
		outBytes.Clear();
		outDiagnostic.clear();
		return true;
	}
	if (metadata.m_byteLength > std::numeric_limits<size_t>::max() ||
		metadata.m_byteLength > static_cast<uint64_t>(std::numeric_limits<std::streamsize>::max()))
	{
		outDiagnostic = "Artifact is too large to read safely: '" + path.generic_string() + "'.";
		return false;
	}

	std::error_code sizeError;
	const uint64_t actualSize = std::filesystem::file_size(path, sizeError);
	if (sizeError)
	{
		outIoFailure = sizeError != std::errc::no_such_file_or_directory &&
			sizeError != std::errc::not_a_directory;
		outDiagnostic = "Cannot inspect shader artifact '" + path.generic_string() + "': " + sizeError.message();
		return false;
	}
	if (actualSize != metadata.m_byteLength)
	{
		outDiagnostic = "Shader artifact '" + path.generic_string() + "' has byte length " +
			std::to_string(actualSize) + ", expected " + std::to_string(metadata.m_byteLength) + ".";
		return false;
	}

	std::ifstream stream(path, std::ios::binary);
	if (!stream.is_open())
	{
		std::error_code existsError;
		const bool bExists = std::filesystem::exists(path, existsError);
		outIoFailure = bExists || static_cast<bool>(existsError);
		outDiagnostic = "Cannot open shader artifact '" + path.generic_string() + "'.";
		return false;
	}

	TVector<uint8_t> candidate(static_cast<size_t>(metadata.m_byteLength));
	stream.read(
		reinterpret_cast<char*>(candidate.GetData()),
		static_cast<std::streamsize>(candidate.Num()));
	if (stream.gcount() != static_cast<std::streamsize>(candidate.Num()) || stream.bad())
	{
		outIoFailure = stream.bad();
		outDiagnostic = "Shader artifact read was incomplete for '" + path.generic_string() + "'.";
		return false;
	}
	if (CalculateArtifactChecksum(candidate.GetData(), candidate.Num()) != metadata.m_checksum)
	{
		outDiagnostic = "Shader artifact checksum mismatch for '" + path.generic_string() + "'.";
		return false;
	}

	outBytes = std::move(candidate);
	outDiagnostic.clear();
	return true;
}

bool ShaderCache::ReadSpirvArtifact(
	const std::filesystem::path& path,
	const ArtifactMetadata& metadata,
	TVector<uint32_t>& outSpirv,
	std::string& outDiagnostic) noexcept
{
	bool ignoredIoFailure = false;
	return ReadSpirvArtifactInternal(path, metadata, outSpirv, outDiagnostic, ignoredIoFailure);
}

bool ShaderCache::ValidateOwnedArtifactPath(
	const std::filesystem::path& cacheRoot,
	const std::filesystem::path& ownedDirectory,
	const std::filesystem::path& artifact,
	std::string& outDiagnostic) noexcept
{
	std::filesystem::path canonicalArtifact;
	bool ignoredIoFailure = false;
	return ResolveOwnedArtifactPath(
		cacheRoot,
		ownedDirectory,
		artifact,
		canonicalArtifact,
		outDiagnostic,
		ignoredIoFailure);
}

bool ShaderCache::ReadSpirvArtifactInternal(
	const std::filesystem::path& path,
	const ArtifactMetadata& metadata,
	TVector<uint32_t>& outSpirv,
	std::string& outDiagnostic,
	bool& outIoFailure) noexcept
{
	outIoFailure = false;
	if (metadata.IsPresent() && metadata.m_byteLength % sizeof(uint32_t) != 0)
	{
		outDiagnostic = "SPIR-V artifact byte length is not aligned to uint32 words for '" +
			path.generic_string() + "'.";
		return false;
	}

	TVector<uint8_t> bytes;
	if (!ReadArtifactBytes(path, metadata, bytes, outDiagnostic, outIoFailure))
	{
		return false;
	}

	TVector<uint32_t> candidate;
	candidate.Resize(bytes.Num() / sizeof(uint32_t));
	if (!bytes.IsEmpty())
	{
		std::memcpy(candidate.GetData(), bytes.GetData(), bytes.Num());
	}
	outSpirv = std::move(candidate);
	return true;
}

bool ShaderCache::IsValidArtifactSet(const ArtifactSet& artifacts, bool bAllowEmpty) noexcept
{
	const bool bHasVertex = artifacts.m_vertex.IsPresent();
	const bool bHasFragment = artifacts.m_fragment.IsPresent();
	const bool bHasCompute = artifacts.m_compute.IsPresent();
	if (!bHasVertex && !bHasFragment && !bHasCompute)
	{
		return bAllowEmpty;
	}
	return bHasVertex == bHasFragment && ((bHasVertex && bHasFragment) || bHasCompute);
}

bool ShaderCache::HasMatchingArtifactTopology(
	const ArtifactSet& regular,
	const ArtifactSet& debug) noexcept
{
	return regular.m_vertex.IsPresent() == debug.m_vertex.IsPresent() &&
		regular.m_fragment.IsPresent() == debug.m_fragment.IsPresent() &&
		regular.m_compute.IsPresent() == debug.m_compute.IsPresent();
}

bool ShaderCache::IsValidGeneration(const std::string& generation) noexcept
{
	if (generation.size() != 32)
	{
		return false;
	}
	return std::all_of(generation.begin(), generation.end(), [](unsigned char character)
		{
			return (character >= '0' && character <= '9') ||
				(character >= 'a' && character <= 'f');
		});
}

bool ShaderCache::DescribeArtifactSet(
	const TVector<uint32_t>& vertexSpirv,
	const TVector<uint32_t>& fragmentSpirv,
	const TVector<uint32_t>& computeSpirv,
	ArtifactSet& outMetadata,
	std::string& outDiagnostic) noexcept
{
	ArtifactSet metadata;
	metadata.m_vertex = DescribeArtifact(
		vertexSpirv.Num() == 0 ? nullptr : &vertexSpirv[0],
		static_cast<uint64_t>(vertexSpirv.Num()) * sizeof(uint32_t));
	metadata.m_fragment = DescribeArtifact(
		fragmentSpirv.Num() == 0 ? nullptr : &fragmentSpirv[0],
		static_cast<uint64_t>(fragmentSpirv.Num()) * sizeof(uint32_t));
	metadata.m_compute = DescribeArtifact(
		computeSpirv.Num() == 0 ? nullptr : &computeSpirv[0],
		static_cast<uint64_t>(computeSpirv.Num()) * sizeof(uint32_t));
	if (!IsValidArtifactSet(metadata, false))
	{
		outDiagnostic = "A shader artifact set must contain a vertex/fragment pair or compute artifact.";
		return false;
	}
	outMetadata = metadata;
	outDiagnostic.clear();
	return true;
}

Workspace::WorkspaceCacheIdentity ShaderCache::GetExpectedIdentityLocked() const
{
#if defined(SAILOR_SHADER_CACHE_TEST_HOOKS)
	if (m_identityOverride.has_value())
	{
		return *m_identityOverride;
	}
#endif
	return MakeExpectedIdentity();
}

std::filesystem::path ShaderCache::GetCacheFilepathLocked() const
{
	return m_cacheRoot / "ShaderCache.yaml";
}

std::filesystem::path ShaderCache::GetPrecompiledFolderLocked() const
{
	return m_cacheRoot / "PrecompiledShaders";
}

std::filesystem::path ShaderCache::GetCompiledFolderLocked() const
{
	return m_cacheRoot / "CompiledShaders";
}

std::filesystem::path ShaderCache::GetCompiledDebugFolderLocked() const
{
	return m_cacheRoot / "CompiledShadersWithDebug";
}

std::filesystem::path ShaderCache::GetArtifactPathLocked(
	const ShaderCacheData::Entry& entry,
	const std::string& shaderKind,
	bool bIsDebug) const
{
	return GetShaderFilepath(
		bIsDebug ? GetCompiledDebugFolderLocked() : GetCompiledFolderLocked(),
		entry.m_fileId,
		static_cast<int32_t>(entry.m_permutation),
		shaderKind,
		CompiledShaderFileExtension,
		entry.m_generation);
}

void ShaderCache::Initialize()
{
	SAILOR_PROFILE_FUNCTION();

	std::error_code error;
	m_cacheRoot = std::filesystem::path(AssetRegistry::GetCacheFolder());
	std::filesystem::create_directories(m_cacheRoot, error);
	if (!error)
	{
		m_cacheRoot = std::filesystem::weakly_canonical(m_cacheRoot, error);
	}

	{
		std::lock_guard<std::mutex> lock(m_cacheMutex);
		std::string diagnostic;
		m_bStorageReady = !error && EnsureOwnedDirectoriesLocked(diagnostic);
		if (!m_bStorageReady)
		{
			m_lastSaveDiagnostic = error
				? "Cannot initialize shader cache root: " + error.message()
				: std::move(diagnostic);
			SAILOR_LOG_ERROR("Shader cache storage initialization failed: %s", m_lastSaveDiagnostic.c_str());
		}
	}

	LoadCache();
}

void ShaderCache::Shutdown()
{
	ClearExpired();
	SaveCache();
}

void ShaderCache::SaveCache(bool bForcely)
{
	SAILOR_PROFILE_FUNCTION();

	std::lock_guard<std::mutex> lock(m_cacheMutex);
	SaveCacheLocked(bForcely);
}

bool ShaderCache::SaveCacheLocked(
	bool bForcely,
	Workspace::EWorkspaceCacheAtomicWriteFailurePoint failurePoint)
{
	if (m_bPreserveStorageAfterLoadFailure)
	{
		return true;
	}
	if (!bForcely && !m_bIsDirty)
	{
		return true;
	}
#if defined(SAILOR_SHADER_CACHE_TEST_HOOKS)
	if (failurePoint == Workspace::EWorkspaceCacheAtomicWriteFailurePoint::None &&
		m_nextSaveFailureForTests != Workspace::EWorkspaceCacheAtomicWriteFailurePoint::None)
	{
		failurePoint = m_nextSaveFailureForTests;
		m_nextSaveFailureForTests = Workspace::EWorkspaceCacheAtomicWriteFailurePoint::None;
	}
#endif

	std::string diagnostic;
	if (WriteCacheDataLocked(m_cache, diagnostic, failurePoint))
	{
		m_bIsDirty = false;
		m_committedCache = m_cache;
		m_bHasCommittedSnapshot = true;

		std::string sweepDiagnostic;
		if (SweepUnreferencedArtifactsLocked(m_committedCache, sweepDiagnostic))
		{
			m_lastSaveDiagnostic.clear();
			return true;
		}

		m_bIsDirty = true;
		m_lastSaveDiagnostic = std::move(sweepDiagnostic);
		SAILOR_LOG_ERROR("Shader cache post-commit cleanup failed: %s", m_lastSaveDiagnostic.c_str());
		return false;
	}

	m_bIsDirty = true;
	m_lastSaveDiagnostic = std::move(diagnostic);
	SAILOR_LOG_ERROR("Shader cache save failed: %s", m_lastSaveDiagnostic.c_str());
	return false;
}

void ShaderCache::LoadCache()
{
	SAILOR_PROFILE_FUNCTION();

	std::lock_guard<std::mutex> lock(m_cacheMutex);
	Workspace::WorkspaceCacheLoadResult loadResult;
	const auto identity = GetExpectedIdentityLocked();
	loadResult = Workspace::LoadWorkspaceCacheEnvelope(GetCacheFilepathLocked(), identity);

	if (loadResult.IsLoaded())
	{
		ShaderCacheData candidate;
		std::string diagnostic;
		bool bValidArtifacts = false;
		bool bArtifactIoFailure = false;
		if (TryDeserializeShaderCachePayload(loadResult.m_payload, candidate, diagnostic))
		{
			bValidArtifacts = ValidateAllArtifactsLocked(
				candidate,
				diagnostic,
				bArtifactIoFailure);
		}
		if (bValidArtifacts)
		{
			m_cache = std::move(candidate);
			m_committedCache = m_cache;
			m_bIsDirty = false;
			m_bPreserveStorageAfterLoadFailure = false;
			m_bHasCommittedSnapshot = true;
			m_quarantinedEntries.Clear();
			m_lastSaveDiagnostic.clear();
			m_lastLoadResult = std::move(loadResult);
			return;
		}

		loadResult.m_status = bArtifactIoFailure
			? Workspace::EWorkspaceCacheLoadStatus::IoFailure
			: Workspace::EWorkspaceCacheLoadStatus::Corrupt;
		loadResult.m_diagnostic = std::move(diagnostic);
		loadResult.m_payload.clear();
	}
	if (m_bPreserveStorageAfterLoadFailure)
	{
		m_cache.m_data.Clear();
		m_committedCache.m_data.Clear();
		m_bIsDirty = false;
		m_bHasCommittedSnapshot = false;
		m_lastSaveDiagnostic.clear();
		m_lastLoadResult = std::move(loadResult);
		SAILOR_LOG_ERROR(
			"Shader cache reload status=%s: %s Read-only I/O quarantine remains active until a fully successful reload or ClearAll.",
			CacheLoadStatusName(m_lastLoadResult.m_status),
			m_lastLoadResult.m_diagnostic.c_str());
		return;
	}
	if (!ShouldResetCache(loadResult.m_status))
	{
		m_cache.m_data.Clear();
		m_committedCache.m_data.Clear();
		m_bIsDirty = false;
		m_bPreserveStorageAfterLoadFailure = true;
		m_bHasCommittedSnapshot = false;
		m_lastSaveDiagnostic.clear();
		m_lastLoadResult = std::move(loadResult);
		SAILOR_LOG_ERROR(
			"Shader cache load status=%s: %s Existing cache metadata and artifact directories were preserved.",
			CacheLoadStatusName(m_lastLoadResult.m_status),
			m_lastLoadResult.m_diagnostic.c_str());
		return;
	}

	ResetInvalidCacheLocked(std::move(loadResult));
}

bool ShaderCache::WriteCacheDataLocked(
	const ShaderCacheData& cache,
	std::string& outDiagnostic,
	Workspace::EWorkspaceCacheAtomicWriteFailurePoint failurePoint)
{
	std::string envelope;
	const auto identity = GetExpectedIdentityLocked();
	if (!Workspace::SerializeWorkspaceCacheEnvelope(
		identity,
		SerializeShaderCachePayload(cache),
		envelope,
		outDiagnostic))
	{
		return false;
	}

	return Workspace::AtomicReplaceWorkspaceCacheText(
		GetCacheFilepathLocked(),
		envelope,
		outDiagnostic,
		failurePoint);
}

bool ShaderCache::WriteCacheLocked(std::string& outDiagnostic)
{
	return WriteCacheDataLocked(m_cache, outDiagnostic);
}

bool ShaderCache::CommitCandidateLocked(
	ShaderCacheData candidate,
	std::string& outDiagnostic,
	Workspace::EWorkspaceCacheAtomicWriteFailurePoint failurePoint)
{
	if (m_bPreserveStorageAfterLoadFailure)
	{
		outDiagnostic = "Shader cache storage is quarantined after an I/O failure; persistent mutation is disabled.";
		return false;
	}
	if (!WriteCacheDataLocked(candidate, outDiagnostic, failurePoint))
	{
		return false;
	}
	m_cache = std::move(candidate);
	m_committedCache = m_cache;
	m_bHasCommittedSnapshot = true;
	m_bIsDirty = false;
	m_lastSaveDiagnostic.clear();
	return true;
}

void ShaderCache::ResetInvalidCacheLocked(Workspace::WorkspaceCacheLoadResult loadResult)
{
	m_cache.m_data.Clear();
	m_committedCache.m_data.Clear();
	m_quarantinedEntries.Clear();
	m_bIsDirty = true;
	m_bPreserveStorageAfterLoadFailure = false;
	m_bHasCommittedSnapshot = false;
	m_lastLoadResult = std::move(loadResult);

	std::string resetDiagnostic;
	const bool bFilesReset = ClearOwnedCacheFilesLocked(resetDiagnostic);
	std::string writeDiagnostic;
	const bool bEnvelopeWritten = WriteCacheLocked(writeDiagnostic);
	if (bFilesReset && bEnvelopeWritten)
	{
		m_bIsDirty = false;
		m_bPreserveStorageAfterLoadFailure = false;
		m_committedCache = m_cache;
		m_bHasCommittedSnapshot = true;
		m_lastSaveDiagnostic.clear();
		AppendDiagnostic(
			m_lastLoadResult.m_diagnostic,
			"The shader cache and owned artifact directories were reset to an empty current envelope.");
		SAILOR_LOG(
			"Shader cache load status=%s: %s",
			CacheLoadStatusName(m_lastLoadResult.m_status),
			m_lastLoadResult.m_diagnostic.c_str());
		return;
	}

	m_bIsDirty = true;
	m_bPreserveStorageAfterLoadFailure = false;
	m_bHasCommittedSnapshot = false;
	m_lastSaveDiagnostic.clear();
	AppendDiagnostic(m_lastSaveDiagnostic, resetDiagnostic);
	AppendDiagnostic(m_lastSaveDiagnostic, writeDiagnostic);
	AppendDiagnostic(
		m_lastLoadResult.m_diagnostic,
		"The shader cache reset was incomplete: " + m_lastSaveDiagnostic);
	SAILOR_LOG_ERROR(
		"Shader cache load status=%s: %s",
		CacheLoadStatusName(m_lastLoadResult.m_status),
		m_lastLoadResult.m_diagnostic.c_str());
}

void ShaderCache::EnterStorageQuarantineLocked(std::string diagnostic)
{
	m_cache.m_data.Clear();
	m_committedCache.m_data.Clear();
	m_bIsDirty = false;
	m_bPreserveStorageAfterLoadFailure = true;
	m_bHasCommittedSnapshot = false;
	m_lastSaveDiagnostic.clear();
	m_lastLoadResult.m_status = Workspace::EWorkspaceCacheLoadStatus::IoFailure;
	m_lastLoadResult.m_diagnostic = std::move(diagnostic);
	m_lastLoadResult.m_payload.clear();
	SAILOR_LOG_ERROR(
		"Shader cache entered read-only I/O quarantine: %s Existing metadata and artifacts were preserved.",
		m_lastLoadResult.m_diagnostic.c_str());
}

bool ShaderCache::EnsureOwnedDirectoriesLocked(std::string& outDiagnostic)
{
	outDiagnostic.clear();
	std::error_code error;
	std::filesystem::create_directories(m_cacheRoot, error);
	if (error)
	{
		outDiagnostic = "Cannot create shader cache root '" + m_cacheRoot.generic_string() +
			"': " + error.message();
		return false;
	}

	bool bSuccess = true;
	const std::filesystem::path directories[] =
	{
		GetPrecompiledFolderLocked(),
		GetCompiledFolderLocked(),
		GetCompiledDebugFolderLocked()
	};
	for (const auto& directory : directories)
	{
		error.clear();
		const auto status = std::filesystem::symlink_status(directory, error);
		if (!error && std::filesystem::is_symlink(status))
		{
			AppendDiagnostic(
				outDiagnostic,
				"Refusing symlinked shader cache directory '" + directory.generic_string() + "'.");
			bSuccess = false;
			continue;
		}

		error.clear();
		std::filesystem::create_directories(directory, error);
		if (error)
		{
			AppendDiagnostic(
				outDiagnostic,
				"Cannot create shader cache directory '" + directory.generic_string() +
				"': " + error.message());
			bSuccess = false;
			continue;
		}

		std::filesystem::path canonical;
		std::string diagnostic;
		if (!ResolveDirectCacheChild(m_cacheRoot, directory, canonical, diagnostic))
		{
			AppendDiagnostic(outDiagnostic, diagnostic);
			bSuccess = false;
		}
	}

	m_bStorageReady = bSuccess;
	return bSuccess;
}

bool ShaderCache::ClearOwnedCacheFilesLocked(std::string& outDiagnostic)
{
	outDiagnostic.clear();
	bool bSuccess = true;
	bSuccess &= RemovePath(m_cacheRoot, GetCacheFilepathLocked(), false, outDiagnostic);
	bSuccess &= RemovePath(m_cacheRoot, GetPrecompiledFolderLocked(), true, outDiagnostic);
	bSuccess &= RemovePath(m_cacheRoot, GetCompiledFolderLocked(), true, outDiagnostic);
	bSuccess &= RemovePath(m_cacheRoot, GetCompiledDebugFolderLocked(), true, outDiagnostic);

	std::string createDiagnostic;
	if (!EnsureOwnedDirectoriesLocked(createDiagnostic))
	{
		AppendDiagnostic(outDiagnostic, createDiagnostic);
		bSuccess = false;
	}
	return bSuccess;
}

bool ShaderCache::ValidateArtifactSetLocked(
	const ShaderCacheData::Entry& entry,
	const ArtifactSet& artifacts,
	bool bIsDebug,
	std::string& outDiagnostic,
	bool& outIoFailure) const
{
	TVector<uint32_t> ignored;
	const std::filesystem::path ownedDirectory = bIsDebug
		? GetCompiledDebugFolderLocked()
		: GetCompiledFolderLocked();
	if (!ReadOwnedSpirvArtifactLocked(
		ownedDirectory,
		GetArtifactPathLocked(entry, VertexShaderTag, bIsDebug),
		artifacts.m_vertex,
		ignored,
		outDiagnostic,
		outIoFailure))
	{
		return false;
	}
	if (!ReadOwnedSpirvArtifactLocked(
		ownedDirectory,
		GetArtifactPathLocked(entry, FragmentShaderTag, bIsDebug),
		artifacts.m_fragment,
		ignored,
		outDiagnostic,
		outIoFailure))
	{
		return false;
	}
	return ReadOwnedSpirvArtifactLocked(
		ownedDirectory,
		GetArtifactPathLocked(entry, ComputeShaderTag, bIsDebug),
		artifacts.m_compute,
		ignored,
		outDiagnostic,
		outIoFailure);
}

bool ShaderCache::ReadOwnedSpirvArtifactLocked(
	const std::filesystem::path& ownedDirectory,
	const std::filesystem::path& artifact,
	const ArtifactMetadata& metadata,
	TVector<uint32_t>& outSpirv,
	std::string& outDiagnostic,
	bool& outIoFailure) const
{
#if defined(SAILOR_SHADER_CACHE_TEST_HOOKS)
	if (m_bArtifactReadIoFailureForTests)
	{
		outIoFailure = true;
		outDiagnostic = "Injected shader artifact read I/O failure for lifecycle validation.";
		return false;
	}
#endif

	std::filesystem::path canonicalArtifact;
	if (!ResolveOwnedArtifactPath(
		m_cacheRoot,
		ownedDirectory,
		artifact,
		canonicalArtifact,
		outDiagnostic,
		outIoFailure))
	{
		return false;
	}
	return ReadSpirvArtifactInternal(
		artifact,
		metadata,
		outSpirv,
		outDiagnostic,
		outIoFailure);
}

bool ShaderCache::ValidateAllArtifactsLocked(
	const ShaderCacheData& candidate,
	std::string& outDiagnostic,
	bool& outIoFailure) const
{
	outIoFailure = false;
	for (const auto& fileEntries : candidate.m_data)
	{
		for (const ShaderCacheData::Entry& entry : *fileEntries.m_second)
		{
			if (!ValidateArtifactSetLocked(
				entry,
				entry.m_regular,
				false,
				outDiagnostic,
				outIoFailure) ||
				!ValidateArtifactSetLocked(
					entry,
					entry.m_debug,
					true,
					outDiagnostic,
					outIoFailure))
			{
				outDiagnostic = "Shader cache artifact validation failed for fileId '" +
					entry.m_fileId.ToString() + "', permutation " +
					std::to_string(entry.m_permutation) + ": " + outDiagnostic;
				return false;
			}
		}
	}
	outDiagnostic.clear();
	return true;
}

bool ShaderCache::WriteSpirvSetLocked(
	const FileId& uid,
	uint32_t permutation,
	const std::string& generation,
	const TVector<uint32_t>& vertexSpirv,
	const TVector<uint32_t>& fragmentSpirv,
	const TVector<uint32_t>& computeSpirv,
	const ArtifactSet& metadata,
	bool bIsDebug,
	int32_t& artifactIndex,
	int32_t failArtifactIndex,
	std::string& outDiagnostic)
{
	if (!EnsureOwnedDirectoriesLocked(outDiagnostic))
	{
		return false;
	}

	if (!IsValidArtifactSet(metadata, false))
	{
		outDiagnostic = "A shader artifact set must contain a vertex/fragment pair or compute artifact.";
		return false;
	}

	ShaderCacheData::Entry pathEntry;
	pathEntry.m_fileId = uid;
	pathEntry.m_permutation = permutation;
	pathEntry.m_generation = generation;
	auto write = [&](const TVector<uint32_t>& spirv,
		const ArtifactMetadata& artifact,
		const char* shaderKind) -> bool
	{
		if (!artifact.IsPresent())
		{
			return true;
		}
		const auto path = GetArtifactPathLocked(pathEntry, shaderKind, bIsDebug);
		const auto ownedDirectory = bIsDebug
			? GetCompiledDebugFolderLocked()
			: GetCompiledFolderLocked();
		std::filesystem::path canonicalArtifact;
		bool ignoredIoFailure = false;
		if (!ResolveOwnedArtifactPath(
			m_cacheRoot,
			ownedDirectory,
			path,
			canonicalArtifact,
			outDiagnostic,
			ignoredIoFailure))
		{
			return false;
		}
		std::string diagnostic;
		const auto failurePoint = artifactIndex == failArtifactIndex
			? Workspace::EWorkspaceCacheAtomicWriteFailurePoint::BeforeReplace
			: Workspace::EWorkspaceCacheAtomicWriteFailurePoint::None;
		++artifactIndex;
		if (!Workspace::AtomicReplaceWorkspaceCacheBinary(
			path,
			&spirv[0],
			artifact.m_byteLength,
			diagnostic,
			failurePoint))
		{
			outDiagnostic = "Cannot atomically write shader artifact '" +
				path.generic_string() + "': " + diagnostic;
			return false;
		}
		return true;
	};

	if (!write(vertexSpirv, metadata.m_vertex, VertexShaderTag) ||
		!write(fragmentSpirv, metadata.m_fragment, FragmentShaderTag) ||
		!write(computeSpirv, metadata.m_compute, ComputeShaderTag))
	{
		return false;
	}

	outDiagnostic.clear();
	return true;
}

bool ShaderCache::GenerateUniqueGenerationLocked(
	const FileId& uid,
	uint32_t permutation,
	std::string& outGeneration,
	std::string& outDiagnostic) const
{
	std::random_device random;
	for (uint32_t attempt = 0; attempt < 64; ++attempt)
	{
		std::ostringstream stream;
		stream << std::hex << std::nouppercase << std::setfill('0');
		for (uint32_t word = 0; word < 4; ++word)
		{
			stream << std::setw(8) << static_cast<uint32_t>(random());
		}
		const std::string generation = stream.str();
		if (!IsValidGeneration(generation))
		{
			continue;
		}

		ShaderCacheData::Entry entry;
		entry.m_fileId = uid;
		entry.m_permutation = permutation;
		entry.m_generation = generation;
		const std::pair<std::filesystem::path, bool> candidates[] =
		{
			{ GetArtifactPathLocked(entry, VertexShaderTag, false), false },
			{ GetArtifactPathLocked(entry, FragmentShaderTag, false), false },
			{ GetArtifactPathLocked(entry, ComputeShaderTag, false), false },
			{ GetArtifactPathLocked(entry, VertexShaderTag, true), true },
			{ GetArtifactPathLocked(entry, FragmentShaderTag, true), true },
			{ GetArtifactPathLocked(entry, ComputeShaderTag, true), true }
		};
		bool bCollision = false;
		for (const auto& [candidate, bIsDebug] : candidates)
		{
			std::filesystem::path canonicalArtifact;
			bool ignoredIoFailure = false;
			const std::filesystem::path ownedDirectory = bIsDebug
				? GetCompiledDebugFolderLocked()
				: GetCompiledFolderLocked();
			if (!ResolveOwnedArtifactPath(
				m_cacheRoot,
				ownedDirectory,
				candidate,
				canonicalArtifact,
				outDiagnostic,
				ignoredIoFailure))
			{
				return false;
			}
			std::error_code error;
			if (std::filesystem::exists(candidate, error))
			{
				bCollision = true;
				break;
			}
			if (error)
			{
				outDiagnostic = "Cannot check immutable shader generation collision for '" +
					candidate.generic_string() + "': " + error.message();
				return false;
			}
		}
		if (!bCollision)
		{
			outGeneration = generation;
			outDiagnostic.clear();
			return true;
		}
	}
	outDiagnostic = "Cannot allocate a collision-free immutable shader artifact generation.";
	return false;
}

bool ShaderCache::CachePrecompiledGlsl(
	const FileId& uid,
	uint32_t permutation,
	const std::string& vertexGlsl,
	const std::string& fragmentGlsl,
	const std::string& computeGlsl)
{
	SAILOR_PROFILE_FUNCTION();

	if (!m_bSavePrecompiledGlsl)
	{
		return true;
	}

	std::lock_guard<std::mutex> lock(m_cacheMutex);
	if (m_bPreserveStorageAfterLoadFailure)
	{
		m_lastSaveDiagnostic = "Precompiled GLSL persistence is disabled during read-only shader cache quarantine.";
		return false;
	}
	std::string diagnostic;
	if (!EnsureOwnedDirectoriesLocked(diagnostic))
	{
		m_lastSaveDiagnostic = std::move(diagnostic);
		SAILOR_LOG_ERROR("Precompiled shader cache write failed: %s", m_lastSaveDiagnostic.c_str());
		return false;
	}

	auto write = [&](const std::string& glsl, const char* shaderKind) -> bool
	{
		if (glsl.empty())
		{
			return true;
		}
		const auto path = GetShaderFilepath(
			GetPrecompiledFolderLocked(),
			uid,
			static_cast<int32_t>(permutation),
			shaderKind,
			PrecompiledShaderFileExtension);
		std::filesystem::path canonicalArtifact;
		bool ignoredIoFailure = false;
		if (!ResolveOwnedArtifactPath(
			m_cacheRoot,
			GetPrecompiledFolderLocked(),
			path,
			canonicalArtifact,
			m_lastSaveDiagnostic,
			ignoredIoFailure))
		{
			SAILOR_LOG_ERROR("Precompiled shader cache write failed: %s", m_lastSaveDiagnostic.c_str());
			return false;
		}
		std::string writeDiagnostic;
		if (!Workspace::AtomicReplaceWorkspaceCacheText(path, glsl, writeDiagnostic))
		{
			m_lastSaveDiagnostic = "Cannot atomically write precompiled shader '" +
				path.generic_string() + "': " + writeDiagnostic;
			SAILOR_LOG_ERROR("Precompiled shader cache write failed: %s", m_lastSaveDiagnostic.c_str());
			return false;
		}
		return true;
	};

	return write(vertexGlsl, VertexShaderTag) &&
		write(fragmentGlsl, FragmentShaderTag) &&
		write(computeGlsl, ComputeShaderTag);
}

ShaderCache::QuarantinedEntry* ShaderCache::FindQuarantinedEntryLocked(
	const FileId& uid,
	uint32_t permutation)
{
	const size_t index = m_quarantinedEntries.FindIf([&](const QuarantinedEntry& entry)
		{
			return entry.m_fileId == uid && entry.m_permutation == permutation;
		});
	return index == static_cast<size_t>(-1) ? nullptr : &m_quarantinedEntries[index];
}

const ShaderCache::QuarantinedEntry* ShaderCache::FindQuarantinedEntryLocked(
	const FileId& uid,
	uint32_t permutation) const
{
	const size_t index = m_quarantinedEntries.FindIf([&](const QuarantinedEntry& entry)
		{
			return entry.m_fileId == uid && entry.m_permutation == permutation;
		});
	return index == static_cast<size_t>(-1) ? nullptr : &m_quarantinedEntries[index];
}

bool ShaderCache::CacheCompleteSpirvLocked(
	const FileId& uid,
	uint32_t permutation,
	const TVector<uint32_t>& vertexSpirv,
	const TVector<uint32_t>& fragmentSpirv,
	const TVector<uint32_t>& computeSpirv,
	const TVector<uint32_t>& debugVertexSpirv,
	const TVector<uint32_t>& debugFragmentSpirv,
	const TVector<uint32_t>& debugComputeSpirv,
	int32_t failArtifactIndex,
	std::string& outDiagnostic)
{
	ArtifactSet regularMetadata;
	ArtifactSet debugMetadata;
	if (!DescribeArtifactSet(vertexSpirv, fragmentSpirv, computeSpirv, regularMetadata, outDiagnostic) ||
		!DescribeArtifactSet(
			debugVertexSpirv,
			debugFragmentSpirv,
			debugComputeSpirv,
			debugMetadata,
			outDiagnostic))
	{
		return false;
	}
	if (!HasMatchingArtifactTopology(regularMetadata, debugMetadata))
	{
		outDiagnostic = "Regular and debug shader artifact sets must contain identical shader stages.";
		return false;
	}

	time_t timestamp = 0;
	GetTimeStamp(uid, timestamp);
	if (m_bPreserveStorageAfterLoadFailure)
	{
		QuarantinedEntry candidate;
		candidate.m_fileId = uid;
		candidate.m_permutation = permutation;
		candidate.m_timestamp = timestamp;
		candidate.m_vertex = vertexSpirv;
		candidate.m_fragment = fragmentSpirv;
		candidate.m_compute = computeSpirv;
		candidate.m_debugVertex = debugVertexSpirv;
		candidate.m_debugFragment = debugFragmentSpirv;
		candidate.m_debugCompute = debugComputeSpirv;
		if (QuarantinedEntry* existing = FindQuarantinedEntryLocked(uid, permutation))
		{
			*existing = std::move(candidate);
		}
		else
		{
			m_quarantinedEntries.Add(std::move(candidate));
		}
		outDiagnostic.clear();
		return true;
	}

	if (!EnsureOwnedDirectoriesLocked(outDiagnostic))
	{
		return false;
	}
	std::string generation;
	if (!GenerateUniqueGenerationLocked(uid, permutation, generation, outDiagnostic))
	{
		return false;
	}
	int32_t artifactIndex = 0;
	if (!WriteSpirvSetLocked(
		uid,
		permutation,
		generation,
		vertexSpirv,
		fragmentSpirv,
		computeSpirv,
		regularMetadata,
		false,
		artifactIndex,
		failArtifactIndex,
		outDiagnostic) ||
		!WriteSpirvSetLocked(
			uid,
			permutation,
			generation,
			debugVertexSpirv,
			debugFragmentSpirv,
			debugComputeSpirv,
			debugMetadata,
			true,
			artifactIndex,
			failArtifactIndex,
			outDiagnostic))
	{
		return false;
	}

	auto& entries = m_cache.m_data[uid];
	auto existing = std::find_if(
		std::begin(entries),
		std::end(entries),
		[permutation](const ShaderCacheData::Entry& entry)
		{
			return entry.m_permutation == permutation;
		});
	ShaderCacheData::Entry candidate;
	candidate.m_fileId = uid;
	candidate.m_permutation = permutation;
	candidate.m_timestamp = timestamp;
	candidate.m_generation = generation;
	candidate.m_regular = regularMetadata;
	candidate.m_debug = debugMetadata;
	if (existing == std::end(entries))
	{
		entries.Add(std::move(candidate));
	}
	else
	{
		*existing = std::move(candidate);
	}

	const size_t quarantinedIndex = m_quarantinedEntries.FindIf([&](const QuarantinedEntry& entry)
		{
			return entry.m_fileId == uid && entry.m_permutation == permutation;
		});
	if (quarantinedIndex != static_cast<size_t>(-1))
	{
		m_quarantinedEntries.RemoveAt(quarantinedIndex);
	}
	m_bIsDirty = true;
	outDiagnostic.clear();
	return true;
}

bool ShaderCache::CacheSpirv_ThreadSafe(
	const FileId& uid,
	uint32_t permutation,
	const TVector<uint32_t>& vertexSpirv,
	const TVector<uint32_t>& fragmentSpirv,
	const TVector<uint32_t>& computeSpirv,
	const TVector<uint32_t>& debugVertexSpirv,
	const TVector<uint32_t>& debugFragmentSpirv,
	const TVector<uint32_t>& debugComputeSpirv)
{
	SAILOR_PROFILE_FUNCTION();

	std::lock_guard<std::mutex> lock(m_cacheMutex);
	std::string diagnostic;
	if (!CacheCompleteSpirvLocked(
		uid,
		permutation,
		vertexSpirv,
		fragmentSpirv,
		computeSpirv,
		debugVertexSpirv,
		debugFragmentSpirv,
		debugComputeSpirv,
		-1,
		diagnostic))
	{
		m_lastSaveDiagnostic = std::move(diagnostic);
		SAILOR_LOG_ERROR("Complete SPIR-V cache publication failed: %s", m_lastSaveDiagnostic.c_str());
		return false;
	}
	m_lastSaveDiagnostic.clear();
	return true;
}

bool ShaderCache::GetSpirvCode(
	const FileId& uid,
	uint32_t permutation,
	TVector<uint32_t>& vertexSpirv,
	TVector<uint32_t>& fragmentSpirv,
	TVector<uint32_t>& computeSpirv,
	bool bIsDebug)
{
	SAILOR_PROFILE_FUNCTION();

	std::lock_guard<std::mutex> lock(m_cacheMutex);
	if (IsExpiredLocked(uid, permutation))
	{
		return false;
	}
	if (const QuarantinedEntry* quarantined = FindQuarantinedEntryLocked(uid, permutation))
	{
		const TVector<uint32_t>& candidateVertex = bIsDebug
			? quarantined->m_debugVertex
			: quarantined->m_vertex;
		const TVector<uint32_t>& candidateFragment = bIsDebug
			? quarantined->m_debugFragment
			: quarantined->m_fragment;
		const TVector<uint32_t>& candidateCompute = bIsDebug
			? quarantined->m_debugCompute
			: quarantined->m_compute;
		vertexSpirv = candidateVertex;
		fragmentSpirv = candidateFragment;
		computeSpirv = candidateCompute;
		return true;
	}

	auto& entries = m_cache.m_data[uid];
	auto entry = std::find_if(
		std::cbegin(entries),
		std::cend(entries),
		[permutation](const ShaderCacheData::Entry& candidate)
		{
			return candidate.m_permutation == permutation;
		});
	if (entry == std::cend(entries))
	{
		return false;
	}

	const ArtifactSet& artifacts = bIsDebug ? entry->m_debug : entry->m_regular;
	if (!IsValidArtifactSet(artifacts, false))
	{
		return false;
	}

	TVector<uint32_t> candidateVertex;
	TVector<uint32_t> candidateFragment;
	TVector<uint32_t> candidateCompute;
	std::string diagnostic;
	bool ignoredIoFailure = false;
	const std::filesystem::path ownedDirectory = bIsDebug
		? GetCompiledDebugFolderLocked()
		: GetCompiledFolderLocked();
	if (!ReadOwnedSpirvArtifactLocked(
		ownedDirectory,
		GetArtifactPathLocked(*entry, VertexShaderTag, bIsDebug),
		artifacts.m_vertex,
		candidateVertex,
		diagnostic,
		ignoredIoFailure) ||
		!ReadOwnedSpirvArtifactLocked(
			ownedDirectory,
			GetArtifactPathLocked(*entry, FragmentShaderTag, bIsDebug),
			artifacts.m_fragment,
			candidateFragment,
			diagnostic,
			ignoredIoFailure) ||
		!ReadOwnedSpirvArtifactLocked(
			ownedDirectory,
			GetArtifactPathLocked(*entry, ComputeShaderTag, bIsDebug),
			artifacts.m_compute,
		candidateCompute,
		diagnostic,
		ignoredIoFailure))
	{
		if (ignoredIoFailure)
		{
			EnterStorageQuarantineLocked(
				"Runtime shader artifact read failed for fileId '" + uid.ToString() +
				"', permutation " + std::to_string(permutation) + ": " + diagnostic);
		}
		return false;
	}

	vertexSpirv = std::move(candidateVertex);
	fragmentSpirv = std::move(candidateFragment);
	computeSpirv = std::move(candidateCompute);
	return true;
}

bool ShaderCache::Contains(const FileId& uid) const
{
	std::lock_guard<std::mutex> lock(m_cacheMutex);
	return m_cache.m_data.ContainsKey(uid) ||
		m_quarantinedEntries.ContainsIf([&](const QuarantinedEntry& entry)
			{
				return entry.m_fileId == uid;
			});
}

bool ShaderCache::IsExpired(const FileId& uid, uint32_t permutation)
{
	std::lock_guard<std::mutex> lock(m_cacheMutex);
	return IsExpiredLocked(uid, permutation);
}

bool ShaderCache::IsExpiredLocked(const FileId& uid, uint32_t permutation)
{
	if (const QuarantinedEntry* quarantined = FindQuarantinedEntryLocked(uid, permutation))
	{
		time_t timestamp = 0;
		const bool bHasAsset = GetTimeStamp(uid, timestamp);
		return !bHasAsset || quarantined->m_timestamp < timestamp;
	}
	if (!m_cache.m_data.ContainsKey(uid))
	{
		return true;
	}

	const auto& entries = m_cache.m_data[uid];
	const size_t index = entries.FindIf([permutation](const ShaderCacheData::Entry& entry)
		{
			return entry.m_permutation == permutation;
		});
	if (index == static_cast<size_t>(-1))
	{
		return true;
	}

	const ShaderCacheData::Entry& entry = entries[index];
	if (!IsValidArtifactSet(entry.m_regular, false) ||
		!IsValidArtifactSet(entry.m_debug, false) ||
		!HasMatchingArtifactTopology(entry.m_regular, entry.m_debug) ||
		!IsValidGeneration(entry.m_generation))
	{
		return true;
	}

	std::string diagnostic;
	bool bArtifactIoFailure = false;
	if (!ValidateArtifactSetLocked(
		entry,
		entry.m_regular,
		false,
		diagnostic,
		bArtifactIoFailure) ||
		!ValidateArtifactSetLocked(
			entry,
			entry.m_debug,
			true,
			diagnostic,
			bArtifactIoFailure))
	{
		if (bArtifactIoFailure)
		{
			EnterStorageQuarantineLocked(
				"Runtime shader artifact validation failed for fileId '" + uid.ToString() +
				"', permutation " + std::to_string(permutation) + ": " + diagnostic);
		}
		return true;
	}

	time_t timestamp = 0;
	const bool bHasAsset = GetTimeStamp(uid, timestamp);
	return !bHasAsset || entry.m_timestamp < timestamp;
}

bool ShaderCache::SweepUnreferencedArtifactsLocked(
	const ShaderCacheData& committedSnapshot,
	std::string& outDiagnostic)
{
	outDiagnostic.clear();
#if defined(SAILOR_SHADER_CACHE_TEST_HOOKS)
	if (m_bArtifactSweepFailureForTests)
	{
		m_bArtifactSweepFailureForTests = false;
		outDiagnostic = "Injected shader artifact sweep failure for lifecycle validation.";
		return false;
	}
#endif
	if (!m_bHasCommittedSnapshot)
	{
		outDiagnostic = "Cannot sweep shader artifacts without a successfully committed metadata snapshot.";
		return false;
	}
	if (!EnsureOwnedDirectoriesLocked(outDiagnostic))
	{
		return false;
	}

	TSet<std::string> whitelist;
	for (const auto& fileEntries : committedSnapshot.m_data)
	{
		for (const ShaderCacheData::Entry& entry : *fileEntries.m_second)
		{
			const std::pair<const ArtifactMetadata*, const char*> regular[] =
			{
				{ &entry.m_regular.m_vertex, VertexShaderTag },
				{ &entry.m_regular.m_fragment, FragmentShaderTag },
				{ &entry.m_regular.m_compute, ComputeShaderTag }
			};
			const std::pair<const ArtifactMetadata*, const char*> debug[] =
			{
				{ &entry.m_debug.m_vertex, VertexShaderTag },
				{ &entry.m_debug.m_fragment, FragmentShaderTag },
				{ &entry.m_debug.m_compute, ComputeShaderTag }
			};
			for (const auto& [metadata, kind] : regular)
			{
				if (metadata->IsPresent())
				{
					whitelist.Insert(GetArtifactPathLocked(entry, kind, false).lexically_normal().generic_string());
					if (m_bSavePrecompiledGlsl)
					{
						whitelist.Insert(GetShaderFilepath(
							GetPrecompiledFolderLocked(),
							entry.m_fileId,
							entry.m_permutation,
							kind,
							PrecompiledShaderFileExtension).lexically_normal().generic_string());
					}
				}
			}
			for (const auto& [metadata, kind] : debug)
			{
				if (metadata->IsPresent())
				{
					whitelist.Insert(GetArtifactPathLocked(entry, kind, true).lexically_normal().generic_string());
				}
			}
		}
	}

	bool bSuccess = true;
	const std::filesystem::path artifactFolders[] =
	{
		GetCompiledFolderLocked(),
		GetCompiledDebugFolderLocked(),
		GetPrecompiledFolderLocked()
	};
	for (const auto& folder : artifactFolders)
	{
		std::error_code error;
		for (std::filesystem::directory_iterator iterator(folder, error), end;
			!error && iterator != end;
			iterator.increment(error))
		{
			const auto& path = iterator->path();
			if (iterator->is_regular_file(error) &&
				!whitelist.Contains(path.lexically_normal().generic_string()))
			{
				std::string diagnostic;
				if (!RemoveOwnedArtifact(m_cacheRoot, folder, path, diagnostic))
				{
					AppendDiagnostic(outDiagnostic, diagnostic);
					bSuccess = false;
				}
			}
		}
		if (error)
		{
			AppendDiagnostic(
				outDiagnostic,
				"Cannot sweep shader cache directory '" + folder.generic_string() +
					"': " + error.message());
			bSuccess = false;
		}
	}
	return bSuccess;
}

bool ShaderCache::RemoveLocked(
	const FileId& uid,
	Workspace::EWorkspaceCacheAtomicWriteFailurePoint failurePoint,
	std::string& outDiagnostic)
{
	for (size_t index = m_quarantinedEntries.Num(); index > 0; --index)
	{
		if (m_quarantinedEntries[index - 1].m_fileId == uid)
		{
			m_quarantinedEntries.RemoveAt(index - 1);
		}
	}
	if (m_bPreserveStorageAfterLoadFailure)
	{
		outDiagnostic.clear();
		return true;
	}
	if (!m_cache.m_data.ContainsKey(uid))
	{
		outDiagnostic.clear();
		return true;
	}

	ShaderCacheData candidate = m_cache;
	candidate.m_data.Remove(uid);
	if (!CommitCandidateLocked(std::move(candidate), outDiagnostic, failurePoint))
	{
		return false;
	}
	std::string sweepDiagnostic;
	if (!SweepUnreferencedArtifactsLocked(m_committedCache, sweepDiagnostic))
	{
		m_bIsDirty = true;
		AppendDiagnostic(outDiagnostic, sweepDiagnostic);
		return false;
	}
	outDiagnostic.clear();
	return true;
}

void ShaderCache::Remove(const FileId& uid)
{
	SAILOR_PROFILE_FUNCTION();

	std::lock_guard<std::mutex> lock(m_cacheMutex);
	std::string diagnostic;
	if (!RemoveLocked(uid, Workspace::EWorkspaceCacheAtomicWriteFailurePoint::None, diagnostic))
	{
		m_lastSaveDiagnostic = std::move(diagnostic);
		SAILOR_LOG_ERROR("Shader cache remove failed: %s", m_lastSaveDiagnostic.c_str());
	}
}

bool ShaderCache::ClearExpiredLocked(
	Workspace::EWorkspaceCacheAtomicWriteFailurePoint failurePoint,
	std::string& outDiagnostic)
{
	if (m_bPreserveStorageAfterLoadFailure)
	{
		outDiagnostic.clear();
		return true;
	}

	TVector<ShaderCacheData::Entry> expired;
	const ShaderCacheData inspectedCache = m_cache;
	for (const auto& fileEntries : inspectedCache.m_data)
	{
		for (const ShaderCacheData::Entry& entry : *fileEntries.m_second)
		{
			if (IsExpiredLocked(entry.m_fileId, entry.m_permutation))
			{
				if (m_bPreserveStorageAfterLoadFailure)
				{
					outDiagnostic.clear();
					return true;
				}
				expired.Add(entry);
			}
		}
	}
	ShaderCacheData candidate = m_cache;
	for (const ShaderCacheData::Entry& entry : expired)
	{
		auto mapEntry = candidate.m_data.Find(entry.m_fileId);
		if (mapEntry == candidate.m_data.end())
		{
			continue;
		}
		auto& entries = candidate.m_data[entry.m_fileId];
		entries.Remove(entry);
		if (entries.Num() == 0)
		{
			candidate.m_data.Remove(entry.m_fileId);
		}
	}

	if (m_bIsDirty || expired.Num() != 0 || !m_bHasCommittedSnapshot)
	{
		if (!CommitCandidateLocked(std::move(candidate), outDiagnostic, failurePoint))
		{
			return false;
		}
	}
	if (!SweepUnreferencedArtifactsLocked(m_committedCache, outDiagnostic))
	{
		m_bIsDirty = true;
		return false;
	}
	outDiagnostic.clear();
	return true;
}

void ShaderCache::ClearExpired()
{
	SAILOR_PROFILE_FUNCTION();

	std::lock_guard<std::mutex> lock(m_cacheMutex);
	if (m_bPreserveStorageAfterLoadFailure)
	{
		SAILOR_LOG(
			"Shader cache cleanup skipped during read-only I/O quarantine; disk storage is preserved.");
		return;
	}
	std::string diagnostic;
	if (!ClearExpiredLocked(Workspace::EWorkspaceCacheAtomicWriteFailurePoint::None, diagnostic))
	{
		m_lastSaveDiagnostic = std::move(diagnostic);
		SAILOR_LOG_ERROR("Shader cache cleanup failed: %s", m_lastSaveDiagnostic.c_str());
	}
}

void ShaderCache::ClearAll()
{
	std::lock_guard<std::mutex> lock(m_cacheMutex);
	m_cache.m_data.Clear();
	m_committedCache.m_data.Clear();
	m_quarantinedEntries.Clear();
	m_bPreserveStorageAfterLoadFailure = false;
	m_bHasCommittedSnapshot = false;
	m_bIsDirty = true;
	std::string clearDiagnostic;
	const bool bCleared = ClearOwnedCacheFilesLocked(clearDiagnostic);
	std::string writeDiagnostic;
	const bool bEnvelopeWritten = WriteCacheLocked(writeDiagnostic);
	if (bCleared && bEnvelopeWritten)
	{
		m_bIsDirty = false;
		m_committedCache = m_cache;
		m_bHasCommittedSnapshot = true;
		m_lastSaveDiagnostic.clear();
	}
	else
	{
		m_bIsDirty = true;
		m_lastSaveDiagnostic.clear();
		AppendDiagnostic(m_lastSaveDiagnostic, clearDiagnostic);
		AppendDiagnostic(m_lastSaveDiagnostic, writeDiagnostic);
		SAILOR_LOG_ERROR("Shader cache clear failed: %s", m_lastSaveDiagnostic.c_str());
	}
}

Workspace::WorkspaceCacheLoadResult ShaderCache::GetLastLoadResult() const
{
	std::lock_guard<std::mutex> lock(m_cacheMutex);
	return m_lastLoadResult;
}

std::string ShaderCache::GetLastSaveDiagnostic() const
{
	std::lock_guard<std::mutex> lock(m_cacheMutex);
	return m_lastSaveDiagnostic;
}

bool ShaderCache::IsDirty() const
{
	std::lock_guard<std::mutex> lock(m_cacheMutex);
	return m_bIsDirty;
}

bool ShaderCache::GetTimeStamp(const FileId& uid, time_t& outTimestamp) const
{
#if defined(SAILOR_SHADER_CACHE_TEST_HOOKS)
	if (m_timestampOverrideForTests.has_value())
	{
		outTimestamp = *m_timestampOverrideForTests;
		return true;
	}
#endif

	auto* assetRegistry = App::GetSubmodule<AssetRegistry>();
	auto* shaderCompiler = App::GetSubmodule<ShaderCompiler>();
	if (!assetRegistry || !shaderCompiler)
	{
		return false;
	}

	if (ShaderAssetInfoPtr assetInfo = assetRegistry->GetAssetInfoPtr<ShaderAssetInfoPtr>(uid))
	{
		if (TSharedPtr<ShaderAsset> shaderAsset = shaderCompiler->LoadShaderAsset(assetInfo->GetFileId()).Lock())
		{
			outTimestamp = assetInfo->GetAssetLastModificationTime();
			for (const auto& include : shaderAsset->GetIncludes())
			{
				time_t includeTimestamp = 0;
				if (assetRegistry->GetContentFileModificationTime(include, includeTimestamp))
				{
					outTimestamp = std::max(outTimestamp, includeTimestamp);
				}
			}
			return true;
		}
	}
	return false;
}

#if defined(SAILOR_SHADER_CACHE_TEST_HOOKS)
bool ShaderCacheTestAccess::Configure(
	ShaderCache& cache,
	const std::filesystem::path& cacheRoot,
	std::time_t timestamp)
{
	std::error_code error;
	std::filesystem::create_directories(cacheRoot, error);
	if (error)
	{
		return false;
	}

	const std::filesystem::path canonicalRoot = std::filesystem::weakly_canonical(cacheRoot, error);
	if (error)
	{
		return false;
	}

	std::lock_guard<std::mutex> lock(cache.m_cacheMutex);
	cache.m_cacheRoot = canonicalRoot;
	cache.m_identityOverride = Workspace::MakeWorkspaceCacheIdentity(
		ShaderCacheKind,
		GetShaderCacheProducerIdentity(),
		ShaderCachePayloadVersion,
		"shader-cache-tests",
		canonicalRoot.parent_path());
	cache.m_timestampOverrideForTests = timestamp;
	std::string diagnostic;
	if (!cache.EnsureOwnedDirectoriesLocked(diagnostic))
	{
		return false;
	}

	return true;
}

std::string ShaderCacheTestAccess::GetGeneration(
	const ShaderCache& cache,
	const FileId& uid,
	uint32_t permutation)
{
	std::lock_guard<std::mutex> lock(cache.m_cacheMutex);
	if (!cache.m_cache.m_data.ContainsKey(uid))
	{
		return {};
	}
	const auto& entries = cache.m_cache.m_data[uid];
	const size_t index = entries.FindIf([permutation](const ShaderCache::ShaderCacheData::Entry& entry)
		{
			return entry.m_permutation == permutation;
		});
	return index == static_cast<size_t>(-1) ? std::string() : entries[index].m_generation;
}

std::filesystem::path ShaderCacheTestAccess::GetArtifactPath(
	const ShaderCache& cache,
	const FileId& uid,
	uint32_t permutation,
	const char* stage,
	bool bIsDebug)
{
	std::lock_guard<std::mutex> lock(cache.m_cacheMutex);
	if (!cache.m_cache.m_data.ContainsKey(uid))
	{
		return {};
	}
	const auto& entries = cache.m_cache.m_data[uid];
	const size_t index = entries.FindIf([permutation](const ShaderCache::ShaderCacheData::Entry& entry)
		{
			return entry.m_permutation == permutation;
		});
	return index == static_cast<size_t>(-1)
		? std::filesystem::path()
		: cache.GetArtifactPathLocked(entries[index], stage, bIsDebug);
}

std::filesystem::path ShaderCacheTestAccess::GetCachePath(const ShaderCache& cache)
{
	std::lock_guard<std::mutex> lock(cache.m_cacheMutex);
	return cache.GetCacheFilepathLocked();
}

bool ShaderCacheTestAccess::PublishWithArtifactFailure(
	ShaderCache& cache,
	const FileId& uid,
	uint32_t permutation,
	const TVector<uint32_t>& vertex,
	const TVector<uint32_t>& fragment,
	const TVector<uint32_t>& compute,
	const TVector<uint32_t>& debugVertex,
	const TVector<uint32_t>& debugFragment,
	const TVector<uint32_t>& debugCompute,
	int32_t failArtifactIndex,
	std::string& outDiagnostic)
{
	std::lock_guard<std::mutex> lock(cache.m_cacheMutex);
	return cache.CacheCompleteSpirvLocked(
		uid,
		permutation,
		vertex,
		fragment,
		compute,
		debugVertex,
		debugFragment,
		debugCompute,
		failArtifactIndex,
		outDiagnostic);
}

bool ShaderCacheTestAccess::RemoveWithEnvelopeFailure(
	ShaderCache& cache,
	const FileId& uid,
	std::string& outDiagnostic)
{
	std::lock_guard<std::mutex> lock(cache.m_cacheMutex);
	return cache.RemoveLocked(
		uid,
		Workspace::EWorkspaceCacheAtomicWriteFailurePoint::BeforeReplace,
		outDiagnostic);
}

bool ShaderCacheTestAccess::ClearExpiredWithEnvelopeFailure(
	ShaderCache& cache,
	std::string& outDiagnostic)
{
	std::lock_guard<std::mutex> lock(cache.m_cacheMutex);
	return cache.ClearExpiredLocked(
		Workspace::EWorkspaceCacheAtomicWriteFailurePoint::BeforeReplace,
		outDiagnostic);
}

void ShaderCacheTestAccess::FailNextSaveBeforeReplace(ShaderCache& cache)
{
	std::lock_guard<std::mutex> lock(cache.m_cacheMutex);
	cache.m_nextSaveFailureForTests =
		Workspace::EWorkspaceCacheAtomicWriteFailurePoint::BeforeReplace;
}

void ShaderCacheTestAccess::SetArtifactReadIoFailure(ShaderCache& cache, bool bEnabled)
{
	std::lock_guard<std::mutex> lock(cache.m_cacheMutex);
	cache.m_bArtifactReadIoFailureForTests = bEnabled;
}

void ShaderCacheTestAccess::FailNextArtifactSweep(ShaderCache& cache)
{
	std::lock_guard<std::mutex> lock(cache.m_cacheMutex);
	cache.m_bArtifactSweepFailureForTests = true;
}

std::string ShaderCacheTestAccess::PayloadWithMissingDebug(const ShaderCache& cache)
{
	std::lock_guard<std::mutex> lock(cache.m_cacheMutex);
	ShaderCache::ShaderCacheData candidate = cache.m_cache;
	for (auto fileEntries : candidate.m_data)
	{
		for (ShaderCache::ShaderCacheData::Entry& entry : *fileEntries.m_second)
		{
			entry.m_debug = {};
		}
	}
	return ShaderCache::SerializeShaderCachePayload(candidate);
}

std::string ShaderCacheTestAccess::PayloadWithMismatchedDebugTopology(const ShaderCache& cache)
{
	std::lock_guard<std::mutex> lock(cache.m_cacheMutex);
	ShaderCache::ShaderCacheData candidate = cache.m_cache;
	for (auto fileEntries : candidate.m_data)
	{
		for (ShaderCache::ShaderCacheData::Entry& entry : *fileEntries.m_second)
		{
			entry.m_debug = {};
			entry.m_debug.m_compute = entry.m_regular.m_vertex;
		}
	}
	return ShaderCache::SerializeShaderCachePayload(candidate);
}

bool ShaderCacheTestAccess::ParsePayload(
	const std::string& payload,
	std::string& outDiagnostic)
{
	ShaderCache::ShaderCacheData candidate;
	return ShaderCache::TryDeserializeShaderCachePayload(payload, candidate, outDiagnostic);
}

bool ShaderCacheTestAccess::IsQuarantined(const ShaderCache& cache)
{
	std::lock_guard<std::mutex> lock(cache.m_cacheMutex);
	return cache.m_bPreserveStorageAfterLoadFailure;
}
#endif
