#include "Workspace/WorkspaceContext.h"
#include "Workspace/WorkspacePathEncoding.h"

#include <algorithm>
#include <cctype>
#include <fstream>
#include <limits>
#include <unordered_set>
#include <utility>
#include <vector>
#include <yaml-cpp/yaml.h>

namespace
{
	using namespace Sailor::Workspace;

	constexpr uint32_t SupportedManifestVersion = 1;
	constexpr uintmax_t MaxManifestSize = 1024 * 1024;
	constexpr const char* DefaultContentPath = "Content";
	constexpr const char* DefaultCachePath = "Cache";
	constexpr const char* DefaultSourcePath = "Source";
	constexpr const char* DefaultGeneratedPath = "Generated";
	constexpr const char* DefaultBuildPath = "Cache/Build";
	constexpr const char* DefaultLogicOutputPath = "Binaries";
	constexpr const char* DefaultModuleName = "SailorGame";

	struct WorkspaceContextCandidate
	{
		std::filesystem::path m_root;
		std::filesystem::path m_manifest;
		std::filesystem::path m_engineRoot;
		std::filesystem::path m_engineContent;
		std::filesystem::path m_content;
		std::filesystem::path m_cache;
		std::filesystem::path m_source;
		std::filesystem::path m_generated;
		std::filesystem::path m_build;
		std::filesystem::path m_logicOutput;
		std::string m_moduleName;
		std::string m_workspaceId;
		std::string m_workspaceName;
		uint32_t m_manifestVersion = 0;
		bool m_bLegacy = false;
	};

	struct ManifestFields
	{
		std::string m_workspaceId;
		std::string m_workspaceName;
		std::string m_enginePath;
		std::string m_engineReferenceKind = "source";
		std::string m_content = DefaultContentPath;
		std::string m_cache = DefaultCachePath;
		std::string m_source = DefaultSourcePath;
		std::string m_generated = DefaultGeneratedPath;
		std::string m_build = DefaultBuildPath;
		std::string m_logicOutput = DefaultLogicOutputPath;
		std::string m_moduleName = DefaultModuleName;
		uint32_t m_manifestVersion = 0;
	};

	class CreatedDirectoriesRollback final
	{
	public:
		~CreatedDirectoriesRollback() noexcept
		{
			if (m_bCommitted)
			{
				return;
			}

			for (auto it = m_directories.rbegin(); it != m_directories.rend(); ++it)
			{
				std::error_code removeError;
				std::filesystem::remove(*it, removeError);
			}
		}

		void Track(std::vector<std::filesystem::path> directories)
		{
			m_directories.insert(
				m_directories.end(),
				std::make_move_iterator(directories.begin()),
				std::make_move_iterator(directories.end()));
		}

		void Commit() noexcept { m_bCommitted = true; }

	private:
		std::vector<std::filesystem::path> m_directories;
		bool m_bCommitted = false;
	};

	WorkspaceContextResolveResult Fail(
		EWorkspaceContextResolveStatus status,
		std::string message) noexcept
	{
		WorkspaceContextResolveResult result;
		result.m_status = status;
		try
		{
			result.m_message = std::move(message);
		}
		catch (...)
		{
			result.m_message.clear();
		}
		return result;
	}

	std::string Trim(std::string value)
	{
		value.erase(value.begin(), std::find_if(value.begin(), value.end(), [](unsigned char character)
			{
				return std::isspace(character) == 0;
			}));
		value.erase(std::find_if(value.rbegin(), value.rend(), [](unsigned char character)
			{
				return std::isspace(character) == 0;
			}).base(), value.end());
		return value;
	}

	bool IsCIdentifier(const std::string& value)
	{
		if (value.empty())
		{
			return false;
		}

		auto isIdentifierStart = [](char character)
		{
			return character == '_' ||
				(character >= 'A' && character <= 'Z') ||
				(character >= 'a' && character <= 'z');
		};

		if (!isIdentifierStart(value.front()))
		{
			return false;
		}

		return std::all_of(value.begin() + 1, value.end(), [&](char character)
			{
				return isIdentifierStart(character) ||
					(character >= '0' && character <= '9');
			});
	}

	bool HasSailorExtension(const std::filesystem::path& path)
	{
		std::string extension = PathToUtf8(path.extension());
		std::transform(extension.begin(), extension.end(), extension.begin(), [](unsigned char character)
			{
				return static_cast<char>(std::tolower(character));
			});
		return extension == ".sailor";
	}

	bool IsDefaultContentPath(const std::string& path)
	{
#if defined(_WIN32)
		return path.size() == std::char_traits<char>::length(DefaultContentPath) &&
			std::equal(path.begin(), path.end(), DefaultContentPath, [](unsigned char left, unsigned char right)
				{
					return std::tolower(left) == std::tolower(right);
				});
#else
		return path == DefaultContentPath;
#endif
	}

	bool IsInside(
		const std::filesystem::path& root,
		const std::filesystem::path& candidate)
	{
		auto rootPart = root.begin();
		auto candidatePart = candidate.begin();
		for (; rootPart != root.end(); ++rootPart, ++candidatePart)
		{
			if (candidatePart == candidate.end())
			{
				return false;
			}

			std::string rootValue = PathToUtf8(*rootPart);
			std::string candidateValue = PathToUtf8(*candidatePart);
#if defined(_WIN32)
			std::transform(rootValue.begin(), rootValue.end(), rootValue.begin(), [](unsigned char character)
				{
					return character >= 'A' && character <= 'Z'
						? static_cast<char>(character + ('a' - 'A'))
						: static_cast<char>(character);
				});
			std::transform(candidateValue.begin(), candidateValue.end(), candidateValue.begin(), [](unsigned char character)
				{
					return character >= 'A' && character <= 'Z'
						? static_cast<char>(character + ('a' - 'A'))
						: static_cast<char>(character);
				});
#endif
			if (rootValue != candidateValue)
			{
				return false;
			}
		}

		return true;
	}

	YAML::Node FindField(const YAML::Node& document, const char* fieldName)
	{
		for (const auto& field : document)
		{
			if (field.first.IsScalar() && field.first.Scalar() == fieldName)
			{
				return field.second;
			}
		}

		return YAML::Node(YAML::NodeType::Undefined);
	}

	bool ValidateManifestMap(const YAML::Node& document, std::string& outError)
	{
		if (!document.IsMap())
		{
			outError = "Workspace manifest must contain a YAML map.";
			return false;
		}

		std::unordered_set<std::string> keys;
		keys.reserve(document.size());
		for (const auto& field : document)
		{
			if (!field.first.IsScalar())
			{
				outError = "Workspace manifest contains a non-scalar field name.";
				return false;
			}

			const std::string key = field.first.Scalar();
			if (key.empty() || !keys.emplace(key).second)
			{
				outError = "Workspace manifest contains duplicate or empty field '" + key + "'.";
				return false;
			}
		}

		return true;
	}

	bool ReadManifestVersion(
		const YAML::Node& document,
		const std::filesystem::path& manifestPath,
		uint32_t& outVersion,
		std::string& outError)
	{
		if (!document.IsMap())
		{
			outError = "Workspace manifest must contain a YAML map.";
			return false;
		}

		YAML::Node version;
		size_t versionCount = 0;
		for (const auto& field : document)
		{
			if (field.first.IsScalar() && field.first.Scalar() == "manifestVersion")
			{
				version = field.second;
				++versionCount;
			}
		}

		if (versionCount == 0)
		{
			outError = "Workspace manifest field 'manifestVersion' is required.";
			return false;
		}
		if (versionCount > 1)
		{
			outError = "Workspace manifest contains duplicate field 'manifestVersion'.";
			return false;
		}
		if (version.IsNull() || !version.IsScalar())
		{
			outError = "Workspace manifest field 'manifestVersion' must be an unsigned integer scalar.";
			return false;
		}

		try
		{
			outVersion = version.as<uint32_t>();
		}
		catch (const YAML::Exception&)
		{
			outError = "Workspace manifest field 'manifestVersion' must be an unsigned integer scalar.";
			return false;
		}

		if (outVersion != SupportedManifestVersion)
		{
			outError = "Workspace manifest '" + PathToUtf8(manifestPath) +
				"' has unsupported manifestVersion '" +
				std::to_string(outVersion) + "'.";
			return false;
		}

		return true;
	}

	bool ReadScalarField(
		const YAML::Node& document,
		const char* fieldName,
		bool bRequired,
		std::string& outValue,
		std::string& outError)
	{
		const YAML::Node field = FindField(document, fieldName);
		if (!field.IsDefined())
		{
			if (bRequired)
			{
				outError = "Workspace manifest field '" + std::string(fieldName) + "' is required.";
				return false;
			}
			return true;
		}
		if (field.IsNull() || !field.IsScalar())
		{
			outError = "Workspace manifest field '" + std::string(fieldName) + "' must be scalar.";
			return false;
		}

		outValue = Trim(field.Scalar());
		if (outValue.empty())
		{
			outError = bRequired
				? "Workspace manifest field '" + std::string(fieldName) + "' is required."
				: "Workspace manifest field '" + std::string(fieldName) + "' must not be empty.";
			return false;
		}
		return true;
	}

	bool ParseManifest(
		const std::filesystem::path& manifestPath,
		ManifestFields& outFields,
		std::string& outError)
	{
		std::error_code fileError;
		const uintmax_t fileSize = std::filesystem::file_size(manifestPath, fileError);
		if (fileError || fileSize > MaxManifestSize ||
			fileSize > static_cast<uintmax_t>((std::numeric_limits<std::streamsize>::max)()))
		{
			outError = "Workspace manifest '" + PathToUtf8(manifestPath) +
				"' is unreadable or exceeds the size limit.";
			return false;
		}

		std::ifstream stream(manifestPath, std::ios::binary);
		if (!stream.is_open())
		{
			outError = "Workspace manifest could not be opened: '" + PathToUtf8(manifestPath) + "'.";
			return false;
		}

		std::string payload(static_cast<size_t>(fileSize), '\0');
		if (fileSize > 0 && !stream.read(payload.data(), static_cast<std::streamsize>(fileSize)))
		{
			outError = "Workspace manifest could not be read: '" + PathToUtf8(manifestPath) + "'.";
			return false;
		}

		const YAML::Node document = YAML::Load(payload);
		if (!ReadManifestVersion(
				document,
				manifestPath,
				outFields.m_manifestVersion,
				outError))
		{
			return false;
		}
		if (!ValidateManifestMap(document, outError))
		{
			return false;
		}

		if (!ReadScalarField(document, "workspaceId", true, outFields.m_workspaceId, outError) ||
			!ReadScalarField(document, "name", true, outFields.m_workspaceName, outError) ||
			!ReadScalarField(document, "enginePath", true, outFields.m_enginePath, outError) ||
			!ReadScalarField(document, "engineReferenceKind", false, outFields.m_engineReferenceKind, outError) ||
			!ReadScalarField(document, "contentPath", true, outFields.m_content, outError) ||
			!ReadScalarField(document, "cachePath", true, outFields.m_cache, outError) ||
			!ReadScalarField(document, "sourcePath", true, outFields.m_source, outError) ||
			!ReadScalarField(document, "generatedProjectPath", true, outFields.m_generated, outError) ||
			!ReadScalarField(document, "buildPath", false, outFields.m_build, outError) ||
			!ReadScalarField(document, "logicOutputPath", false, outFields.m_logicOutput, outError) ||
			!ReadScalarField(document, "logicModuleName", false, outFields.m_moduleName, outError))
		{
			return false;
		}

		if (!IsCIdentifier(outFields.m_moduleName))
		{
			outError = "Workspace manifest logicModuleName '" + outFields.m_moduleName +
				"' must be a valid C identifier.";
			return false;
		}

		std::transform(
			outFields.m_engineReferenceKind.begin(),
			outFields.m_engineReferenceKind.end(),
			outFields.m_engineReferenceKind.begin(),
			[](unsigned char character)
			{
				return static_cast<char>(std::tolower(character));
			});
		if (outFields.m_engineReferenceKind != "source" &&
			outFields.m_engineReferenceKind != "installed")
		{
			outError = "Workspace manifest engineReferenceKind must be 'source' or 'installed'.";
			return false;
		}

		return true;
	}

	bool NormalizeOwnedRelativePath(
		const std::string& rawValue,
		const char* fieldName,
		std::filesystem::path& outPath,
		std::string& outNormalized,
		std::string& outError)
	{
		std::string normalized = Trim(rawValue);
		std::replace(normalized.begin(), normalized.end(), '\\', '/');
		while (normalized.rfind("./", 0) == 0)
		{
			normalized.erase(0, 2);
		}
		while (normalized.size() > 1 && normalized.back() == '/')
		{
			normalized.pop_back();
		}

		const bool bWindowsDrivePath = normalized.size() >= 2 &&
			std::isalpha(static_cast<unsigned char>(normalized[0])) != 0 &&
			normalized[1] == ':';
		const std::filesystem::path path = PathFromUtf8(normalized);
		if (normalized.empty() || normalized.find('\0') != std::string::npos ||
			normalized.front() == '/' || bWindowsDrivePath ||
			path.is_absolute() || path.has_root_name() || path.has_root_directory())
		{
			outError = "Workspace manifest field '" + std::string(fieldName) +
				"' must be a safe relative path: '" + rawValue + "'.";
			return false;
		}

		for (const std::filesystem::path& component : path)
		{
			if (component == "..")
			{
				outError = "Workspace manifest field '" + std::string(fieldName) +
					"' contains traversal: '" + rawValue + "'.";
				return false;
			}
		}

		outPath = path.lexically_normal();
		outNormalized = PathToUtf8(outPath);
		return true;
	}

	bool ResolveOwnedPath(
		const std::filesystem::path& root,
		const std::filesystem::path& relativePath,
		const char* fieldName,
		std::filesystem::path& outPath,
		std::string& outError)
	{
		std::error_code pathError;
		outPath = std::filesystem::weakly_canonical(root / relativePath, pathError);
		if (pathError || outPath.empty())
		{
			outError = "Workspace path '" + std::string(fieldName) +
				"' could not be resolved: '" + PathToUtf8(root / relativePath) + "'.";
			return false;
		}
		if (!IsInside(root, outPath))
		{
			outError = "Workspace path '" + std::string(fieldName) +
				"' escapes the workspace after physical resolution: '" + PathToUtf8(outPath) + "'.";
			return false;
		}
		return true;
	}

	bool ResolveEnginePaths(
		const std::filesystem::path& workspaceRoot,
		const std::string& rawEnginePath,
		const std::string& engineReferenceKind,
		std::filesystem::path& outEngineRoot,
		std::filesystem::path& outEngineContent,
		std::string& outError)
	{
		if (rawEnginePath.find('\0') != std::string::npos)
		{
			outError = "Workspace manifest field 'enginePath' contains an invalid null character.";
			return false;
		}

		const std::filesystem::path engineReference = PathFromUtf8(rawEnginePath);
		const std::filesystem::path requestedEngineRoot = engineReference.is_absolute()
			? engineReference
			: workspaceRoot / engineReference;
		std::error_code pathError;
		outEngineRoot = std::filesystem::canonical(requestedEngineRoot, pathError);
		if (pathError || !std::filesystem::is_directory(outEngineRoot, pathError) || pathError)
		{
			outError = "Workspace manifest field 'enginePath' must resolve to an existing directory: '" +
				PathToUtf8(requestedEngineRoot) + "'.";
			return false;
		}

		const std::filesystem::path requestedEngineContent = outEngineRoot / DefaultContentPath;
		pathError.clear();
		outEngineContent = std::filesystem::canonical(requestedEngineContent, pathError);
		if (pathError || !std::filesystem::is_directory(outEngineContent, pathError) || pathError)
		{
			if (engineReferenceKind == "installed")
			{
				outError = "Installed engine reference does not provide runtime Content at '" +
					PathToUtf8(requestedEngineContent) +
					"'. Packaging runtime Content for installed engines is not supported by this build.";
			}
			else
			{
				outError = "Engine Content must be an existing directory: '" +
					PathToUtf8(requestedEngineContent) + "'.";
			}
			return false;
		}

		return true;
	}

	bool EnsureDirectory(
		const std::filesystem::path& root,
		const std::filesystem::path& directory,
		const char* fieldName,
		CreatedDirectoriesRollback& rollback,
		std::string& outError)
	{
		std::error_code pathError;
		const bool bExists = std::filesystem::exists(directory, pathError);
		if (pathError)
		{
			outError = "Workspace directory '" + std::string(fieldName) +
				"' could not be inspected: '" + PathToUtf8(directory) + "'.";
			return false;
		}
		if (bExists)
		{
			if (!std::filesystem::is_directory(directory, pathError) || pathError)
			{
				outError = "Workspace path '" + std::string(fieldName) +
					"' is not a directory: '" + PathToUtf8(directory) + "'.";
				return false;
			}
			return true;
		}

		std::vector<std::filesystem::path> missingDirectories;
		for (std::filesystem::path current = directory;
			current != root && !current.empty();
			current = current.parent_path())
		{
			pathError.clear();
			if (std::filesystem::exists(current, pathError))
			{
				break;
			}
			if (pathError)
			{
				outError = "Workspace directory '" + std::string(fieldName) +
					"' could not be inspected: '" + PathToUtf8(current) + "'.";
				return false;
			}
			missingDirectories.emplace_back(current);
		}

		std::reverse(missingDirectories.begin(), missingDirectories.end());
		for (const std::filesystem::path& missingDirectory : missingDirectories)
		{
			pathError.clear();
			const std::filesystem::path parent = std::filesystem::canonical(
				missingDirectory.parent_path(),
				pathError);
			if (pathError || !IsInside(root, parent))
			{
				outError = "Workspace directory '" + std::string(fieldName) +
					"' parent escapes the workspace during recovery: '" +
					PathToUtf8(missingDirectory.parent_path()) + "'.";
				return false;
			}

			const std::filesystem::path verifiedDirectory = parent / missingDirectory.filename();
			pathError.clear();
			const bool bCreated = std::filesystem::create_directory(verifiedDirectory, pathError);
			if (pathError)
			{
				outError = "Workspace directory '" + std::string(fieldName) +
					"' could not be created: '" + PathToUtf8(verifiedDirectory) +
					"': " + pathError.message() + ".";
				return false;
			}
			if (bCreated)
			{
				rollback.Track({ verifiedDirectory });
			}

			pathError.clear();
			const std::filesystem::path physicalDirectory = std::filesystem::canonical(
				verifiedDirectory,
				pathError);
			if (pathError || !IsInside(root, physicalDirectory) ||
				!std::filesystem::is_directory(physicalDirectory, pathError) || pathError)
			{
				outError = "Workspace directory '" + std::string(fieldName) +
					"' escaped the workspace during recovery: '" +
					PathToUtf8(verifiedDirectory) + "'.";
				return false;
			}
		}

		return true;
	}

	bool RecanonicalizeDirectory(
		const std::filesystem::path& root,
		std::filesystem::path& directory,
		const char* fieldName,
		std::string& outError)
	{
		std::error_code pathError;
		directory = std::filesystem::canonical(directory, pathError);
		if (pathError || !IsInside(root, directory))
		{
			outError = "Workspace directory '" + std::string(fieldName) +
				"' escapes the workspace after recovery: '" + PathToUtf8(directory) + "'.";
			return false;
		}
		return true;
	}

	bool DiscoverManifest(
		const std::filesystem::path& root,
		const std::filesystem::path& requestedManifest,
		std::filesystem::path& outManifest,
		EWorkspaceContextResolveStatus& outFailureStatus,
		std::string& outError)
	{
		std::error_code pathError;
		if (!requestedManifest.empty())
		{
			const std::filesystem::path candidate = requestedManifest.is_absolute()
				? requestedManifest
				: root / requestedManifest;
			if (!std::filesystem::is_regular_file(candidate, pathError) || pathError)
			{
				outFailureStatus = EWorkspaceContextResolveStatus::ManifestNotFound;
				outError = "Workspace manifest was not found: '" + PathToUtf8(candidate) + "'.";
				return false;
			}

			outManifest = std::filesystem::canonical(candidate, pathError);
			if (pathError || !IsInside(root, outManifest))
			{
				outFailureStatus = EWorkspaceContextResolveStatus::PathInvalid;
				outError = "Workspace manifest resolves outside the workspace: '" +
					PathToUtf8(outManifest) + "'.";
				return false;
			}
			return true;
		}

		const std::filesystem::path defaultManifest = root / "workspace.sailor";
		pathError.clear();
		if (std::filesystem::is_regular_file(defaultManifest, pathError) && !pathError)
		{
			outManifest = std::filesystem::canonical(defaultManifest, pathError);
			if (pathError || !IsInside(root, outManifest))
			{
				outFailureStatus = EWorkspaceContextResolveStatus::PathInvalid;
				outError = "Workspace manifest resolves outside the workspace: '" +
					PathToUtf8(outManifest) + "'.";
				return false;
			}
			return true;
		}

		std::vector<std::filesystem::path> manifests;
		for (std::filesystem::directory_iterator it(root, pathError), end;
			!pathError && it != end;
			it.increment(pathError))
		{
			std::error_code entryError;
			if (it->is_regular_file(entryError) && !entryError && HasSailorExtension(it->path()))
			{
				manifests.emplace_back(it->path());
			}
		}
		if (pathError)
		{
			outFailureStatus = EWorkspaceContextResolveStatus::WorkspaceInvalid;
			outError = "Workspace manifests could not be enumerated in '" + PathToUtf8(root) +
				"': " + pathError.message() + ".";
			return false;
		}

		std::sort(manifests.begin(), manifests.end());
		if (manifests.empty())
		{
			outManifest.clear();
			return true;
		}
		if (manifests.size() > 1)
		{
			outFailureStatus = EWorkspaceContextResolveStatus::ManifestAmbiguous;
			outError = "Multiple .sailor manifests were found in '" + PathToUtf8(root) +
				"'. Select one explicitly.";
			return false;
		}

		outManifest = std::filesystem::canonical(manifests.front(), pathError);
		if (pathError || !IsInside(root, outManifest))
		{
			outFailureStatus = EWorkspaceContextResolveStatus::PathInvalid;
			outError = "Workspace manifest resolves outside the workspace: '" +
				PathToUtf8(outManifest) + "'.";
			return false;
		}
		return true;
	}
}

Sailor::Workspace::WorkspaceContextResolveResult Sailor::Workspace::ResolveWorkspaceContext(
	const std::filesystem::path& requestedRoot,
	const std::filesystem::path& requestedManifest) noexcept
{
	try
	{
		if (requestedRoot.empty())
		{
			return Fail(
				EWorkspaceContextResolveStatus::WorkspaceInvalid,
				"Workspace root must not be empty.");
		}

		std::error_code pathError;
		const std::filesystem::path absoluteRoot = std::filesystem::absolute(requestedRoot, pathError);
		const std::filesystem::path root = pathError
			? std::filesystem::path{}
			: std::filesystem::canonical(absoluteRoot, pathError);
		if (pathError || root.empty() || !std::filesystem::is_directory(root))
		{
			return Fail(
				EWorkspaceContextResolveStatus::WorkspaceInvalid,
				"Workspace root must be an existing directory: '" + PathToUtf8(requestedRoot) + "'.");
		}

		WorkspaceContextCandidate candidate;
		candidate.m_root = root;
		EWorkspaceContextResolveStatus discoveryFailure = EWorkspaceContextResolveStatus::ManifestNotFound;
		std::string error;
		if (!DiscoverManifest(
				root,
				requestedManifest,
				candidate.m_manifest,
				discoveryFailure,
				error))
		{
			return Fail(discoveryFailure, std::move(error));
		}

		ManifestFields fields;
		if (candidate.m_manifest.empty())
		{
			candidate.m_bLegacy = true;
			candidate.m_moduleName = DefaultModuleName;
		}
		else
		{
			try
			{
				if (!ParseManifest(candidate.m_manifest, fields, error))
				{
					return Fail(EWorkspaceContextResolveStatus::ManifestInvalid, std::move(error));
				}
			}
			catch (const YAML::Exception& e)
			{
				return Fail(
					EWorkspaceContextResolveStatus::ManifestInvalid,
					"Workspace manifest '" + PathToUtf8(candidate.m_manifest) +
						"' is invalid YAML: " + e.what());
			}
			candidate.m_manifestVersion = fields.m_manifestVersion;
			candidate.m_workspaceId = fields.m_workspaceId;
			candidate.m_workspaceName = fields.m_workspaceName;
			candidate.m_moduleName = fields.m_moduleName;
			if (!ResolveEnginePaths(
					root,
					fields.m_enginePath,
					fields.m_engineReferenceKind,
					candidate.m_engineRoot,
					candidate.m_engineContent,
					error))
			{
				return Fail(EWorkspaceContextResolveStatus::PathInvalid, std::move(error));
			}
		}

		std::filesystem::path contentRelative;
		std::filesystem::path cacheRelative;
		std::filesystem::path sourceRelative;
		std::filesystem::path generatedRelative;
		std::filesystem::path buildRelative;
		std::filesystem::path logicOutputRelative;
		std::string normalizedContent;
		std::string unusedNormalized;
		if (!NormalizeOwnedRelativePath(fields.m_content, "contentPath", contentRelative, normalizedContent, error) ||
			!NormalizeOwnedRelativePath(fields.m_cache, "cachePath", cacheRelative, unusedNormalized, error) ||
			!NormalizeOwnedRelativePath(fields.m_source, "sourcePath", sourceRelative, unusedNormalized, error) ||
			!NormalizeOwnedRelativePath(fields.m_generated, "generatedProjectPath", generatedRelative, unusedNormalized, error) ||
			!NormalizeOwnedRelativePath(fields.m_build, "buildPath", buildRelative, unusedNormalized, error) ||
			!NormalizeOwnedRelativePath(fields.m_logicOutput, "logicOutputPath", logicOutputRelative, unusedNormalized, error))
		{
			return Fail(EWorkspaceContextResolveStatus::PathInvalid, std::move(error));
		}

		if (!ResolveOwnedPath(root, contentRelative, "contentPath", candidate.m_content, error) ||
			!ResolveOwnedPath(root, cacheRelative, "cachePath", candidate.m_cache, error) ||
			!ResolveOwnedPath(root, sourceRelative, "sourcePath", candidate.m_source, error) ||
			!ResolveOwnedPath(root, generatedRelative, "generatedProjectPath", candidate.m_generated, error) ||
			!ResolveOwnedPath(root, buildRelative, "buildPath", candidate.m_build, error) ||
			!ResolveOwnedPath(root, logicOutputRelative, "logicOutputPath", candidate.m_logicOutput, error))
		{
			return Fail(EWorkspaceContextResolveStatus::PathInvalid, std::move(error));
		}

		pathError.clear();
		const bool bContentExists = std::filesystem::exists(candidate.m_content, pathError);
		if (pathError)
		{
			return Fail(
				EWorkspaceContextResolveStatus::PathInvalid,
				"Workspace content path could not be inspected: '" +
					PathToUtf8(candidate.m_content) + "'.");
		}
		if (bContentExists && !std::filesystem::is_directory(candidate.m_content))
		{
			return Fail(
				EWorkspaceContextResolveStatus::PathInvalid,
				"Workspace content path is not a directory: '" +
					PathToUtf8(candidate.m_content) + "'.");
		}
		if (!bContentExists && !IsDefaultContentPath(normalizedContent))
		{
			return Fail(
				EWorkspaceContextResolveStatus::ContentMissing,
				"Custom workspace content directory is missing and will not be created: '" +
					PathToUtf8(candidate.m_content) + "'.");
		}

		CreatedDirectoriesRollback rollback;
		if (!bContentExists &&
			!EnsureDirectory(root, candidate.m_content, "contentPath", rollback, error))
		{
			return Fail(EWorkspaceContextResolveStatus::DirectoryCreationFailed, std::move(error));
		}
		if (!EnsureDirectory(root, candidate.m_cache, "cachePath", rollback, error))
		{
			return Fail(EWorkspaceContextResolveStatus::DirectoryCreationFailed, std::move(error));
		}

		if (!RecanonicalizeDirectory(root, candidate.m_content, "contentPath", error) ||
			!RecanonicalizeDirectory(root, candidate.m_cache, "cachePath", error))
		{
			return Fail(EWorkspaceContextResolveStatus::PathInvalid, std::move(error));
		}
		if (candidate.m_bLegacy)
		{
			candidate.m_engineRoot = candidate.m_root;
			candidate.m_engineContent = candidate.m_content;
		}

		WorkspaceContextResolveResult result;
		result.m_status = candidate.m_bLegacy
			? EWorkspaceContextResolveStatus::Legacy
			: EWorkspaceContextResolveStatus::Success;
		result.m_message = candidate.m_bLegacy
			? "No workspace manifest was found; using legacy Content and Cache directories under '" +
				PathToUtf8(root) + "'."
			: "Resolved workspace manifest '" + PathToUtf8(candidate.m_manifest) + "'.";
		result.m_context.m_root = std::move(candidate.m_root);
		result.m_context.m_manifest = std::move(candidate.m_manifest);
		result.m_context.m_engineRoot = std::move(candidate.m_engineRoot);
		result.m_context.m_engineContent = std::move(candidate.m_engineContent);
		result.m_context.m_content = std::move(candidate.m_content);
		result.m_context.m_cache = std::move(candidate.m_cache);
		result.m_context.m_source = std::move(candidate.m_source);
		result.m_context.m_generated = std::move(candidate.m_generated);
		result.m_context.m_build = std::move(candidate.m_build);
		result.m_context.m_logicOutput = std::move(candidate.m_logicOutput);
		result.m_context.m_moduleName = std::move(candidate.m_moduleName);
		result.m_context.m_workspaceId = std::move(candidate.m_workspaceId);
		result.m_context.m_workspaceName = std::move(candidate.m_workspaceName);
		result.m_context.m_manifestVersion = candidate.m_manifestVersion;
		result.m_context.m_bLegacy = candidate.m_bLegacy;
		rollback.Commit();
		return result;
	}
	catch (const std::exception& e)
	{
		return Fail(
			EWorkspaceContextResolveStatus::ManifestInvalid,
			"Workspace context resolution failed: " + std::string(e.what()));
	}
	catch (...)
	{
		return Fail(
			EWorkspaceContextResolveStatus::ManifestInvalid,
			"Workspace context resolution failed with an unknown error.");
	}
}
