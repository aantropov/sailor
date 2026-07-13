#include "Workspace/WorkspaceContext.h"

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>
#include <yaml-cpp/yaml.h>

using namespace Sailor::Workspace;

namespace
{
	struct ManifestSpec
	{
		uint32_t m_version = 1;
		std::string m_workspaceId = "workspace-id";
		std::string m_name = "Sandbox";
		std::string m_enginePath = "../Sailor";
		std::string m_engineReferenceKind = "source";
		bool m_includeEngineReferenceKind = true;
		std::string m_content = "Content";
		std::string m_cache = "Cache";
		std::string m_source = "Source";
		std::string m_generated = "Generated";
		std::string m_build = "Cache/Build";
		std::string m_logicOutput = "Binaries";
		std::string m_moduleName = "SailorGame";
	};

	class TempDirectory final
	{
	public:
		explicit TempDirectory(const char* label)
		{
			static uint64_t nextId = 0;
			const auto timestamp = std::chrono::steady_clock::now().time_since_epoch().count();
			m_path = std::filesystem::temp_directory_path() /
				("sailor-workspace-context-" + std::string(label) + "-" +
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

	std::filesystem::path Canonical(const std::filesystem::path& path)
	{
		std::error_code pathError;
		const std::filesystem::path canonical = std::filesystem::canonical(path, pathError);
		Require(!pathError, "test path should be canonicalizable: " + path.generic_string());
		return canonical;
	}

	void WriteManifest(
		const std::filesystem::path& path,
		const ManifestSpec& spec = {})
	{
		YAML::Node manifest;
		manifest["manifestVersion"] = spec.m_version;
		manifest["workspaceId"] = spec.m_workspaceId;
		manifest["name"] = spec.m_name;
		manifest["enginePath"] = spec.m_enginePath;
		if (spec.m_includeEngineReferenceKind)
		{
			manifest["engineReferenceKind"] = spec.m_engineReferenceKind;
		}
		manifest["contentPath"] = spec.m_content;
		manifest["cachePath"] = spec.m_cache;
		manifest["sourcePath"] = spec.m_source;
		manifest["generatedProjectPath"] = spec.m_generated;
		manifest["buildPath"] = spec.m_build;
		manifest["logicOutputPath"] = spec.m_logicOutput;
		manifest["logicModuleName"] = spec.m_moduleName;

		std::ofstream output(path);
		output << manifest;
		Require(output.good(), "test manifest should be writable: " + path.generic_string());
	}

	void WriteText(const std::filesystem::path& path, const std::string& value)
	{
		std::ofstream output(path);
		output << value;
		Require(output.good(), "test file should be writable: " + path.generic_string());
	}

	void TestLegacyFallbackAndRecovery()
	{
		TempDirectory workspace("legacy");

		const WorkspaceContextResolveResult result = ResolveWorkspaceContext(workspace.Get());

		Require(result.IsSuccess(), "legacy workspace should resolve: " + result.m_message);
		Require(result.m_status == EWorkspaceContextResolveStatus::Legacy,
			"legacy workspace should return the Legacy status");
		Require(result.m_context.IsLegacy(), "legacy context should identify itself");
		Require(result.m_context.GetRoot() == Canonical(workspace.Get()),
			"legacy context should canonicalize its root");
		Require(result.m_context.GetManifest().empty(), "legacy context should not invent a manifest");
		Require(result.m_context.GetContent() == Canonical(workspace.Path("Content")),
			"legacy context should use the default Content directory");
		Require(result.m_context.GetCache() == Canonical(workspace.Path("Cache")),
			"legacy context should use the default Cache directory");
		Require(std::filesystem::is_directory(workspace.Path("Content")),
			"legacy resolution should recreate default Content");
		Require(std::filesystem::is_directory(workspace.Path("Cache")),
			"legacy resolution should recreate default Cache");
	}

	void TestManifestPathsWithSpaces()
	{
		TempDirectory workspace("spaces");
		ManifestSpec spec;
		spec.m_workspaceId = "workspace with spaces";
		spec.m_name = "Sandbox With Spaces";
		spec.m_content = "Game Data/Content Files";
		spec.m_cache = "State Data/Cache Files";
		spec.m_source = "Game Code/Source Files";
		spec.m_generated = "Generated Project/Files";
		spec.m_build = "State Data/Build Output";
		spec.m_logicOutput = "Game Binaries/Modules";
		spec.m_moduleName = "SandboxLogic";
		std::filesystem::create_directories(workspace.Path(spec.m_content));
		const std::filesystem::path manifestPath = workspace.Path("Sandbox Project.sailor");
		WriteManifest(manifestPath, spec);

		const WorkspaceContextResolveResult result = ResolveWorkspaceContext(
			workspace.Path("."),
			manifestPath);

		Require(result.IsSuccess(), "manifest paths with spaces should resolve: " + result.m_message);
		Require(result.m_status == EWorkspaceContextResolveStatus::Success,
			"manifest context should return Success");
		Require(!result.m_context.IsLegacy(), "manifest context should not be legacy");
		Require(result.m_context.GetManifest() == Canonical(manifestPath),
			"manifest path should be canonical");
		Require(result.m_context.GetManifestVersion() == 1,
			"manifest version should be preserved");
		Require(result.m_context.GetWorkspaceId() == spec.m_workspaceId,
			"workspace id should be preserved");
		Require(result.m_context.GetWorkspaceName() == spec.m_name,
			"workspace name should be preserved");
		Require(result.m_context.GetContent() == Canonical(workspace.Path(spec.m_content)),
			"custom content path should preserve spaces");
		Require(result.m_context.GetCache() == Canonical(workspace.Path(spec.m_cache)),
			"custom cache path should preserve spaces and be created");
		Require(result.m_context.GetSource() ==
			std::filesystem::weakly_canonical(workspace.Path(spec.m_source)),
			"source path should preserve spaces");
		Require(result.m_context.GetGenerated() ==
			std::filesystem::weakly_canonical(workspace.Path(spec.m_generated)),
			"generated path should preserve spaces");
		Require(result.m_context.GetBuild() ==
			std::filesystem::weakly_canonical(workspace.Path(spec.m_build)),
			"build path should preserve spaces");
		Require(result.m_context.GetLogicOutput() ==
			std::filesystem::weakly_canonical(workspace.Path(spec.m_logicOutput)),
			"logic output path should preserve spaces");
		Require(result.m_context.GetModuleName() == spec.m_moduleName,
			"module name should be preserved");
	}

	void TestManifestDefaultRecovery()
	{
		TempDirectory workspace("default-recovery");
		WriteManifest(workspace.Path("workspace.sailor"));

		const WorkspaceContextResolveResult result = ResolveWorkspaceContext(workspace.Get());

		Require(result.IsSuccess(), "default manifest paths should recover: " + result.m_message);
		Require(std::filesystem::is_directory(workspace.Path("Content")),
			"missing default manifest Content should be created");
		Require(std::filesystem::is_directory(workspace.Path("Cache")),
			"missing default manifest Cache should be created");
	}

	void TestManifestDefaultsMissingEngineReferenceKindToSource()
	{
		TempDirectory workspace("default-engine-reference");
		ManifestSpec spec;
		spec.m_includeEngineReferenceKind = false;
		WriteManifest(workspace.Path("workspace.sailor"), spec);

		const WorkspaceContextResolveResult result = ResolveWorkspaceContext(workspace.Get());

		Require(result.IsSuccess(),
			"v1 manifest without engineReferenceKind should default to source: " + result.m_message);
	}

	void TestMissingCustomContentDoesNotMutateWorkspace()
	{
		TempDirectory workspace("missing-custom-content");
		ManifestSpec spec;
		spec.m_content = "Moved Content";
		spec.m_cache = "Disposable Cache";
		WriteManifest(workspace.Path("workspace.sailor"), spec);

		const WorkspaceContextResolveResult result = ResolveWorkspaceContext(workspace.Get());

		Require(result.m_status == EWorkspaceContextResolveStatus::ContentMissing,
			"missing custom content should be rejected: " + result.m_message);
		Require(!result.IsSuccess(), "missing custom content should not publish a context");
		Require(result.m_context.GetRoot().empty(), "failed resolution should keep context transactional");
		Require(!std::filesystem::exists(workspace.Path(spec.m_content)),
			"missing custom content must not be created");
		Require(!std::filesystem::exists(workspace.Path(spec.m_cache)),
			"cache recovery must not run after custom content validation fails");
	}

	void TestRecoveryRollbackOnLaterFailure()
	{
		TempDirectory workspace("recovery-rollback");
		WriteManifest(workspace.Path("workspace.sailor"));
		WriteText(workspace.Path("Cache"), "cache path is a file");

		const WorkspaceContextResolveResult result = ResolveWorkspaceContext(workspace.Get());

		Require(result.m_status == EWorkspaceContextResolveStatus::DirectoryCreationFailed,
			"cache recovery failure should be actionable: " + result.m_message);
		Require(!std::filesystem::exists(workspace.Path("Content")),
			"default Content created before a later failure should be rolled back");
		Require(std::filesystem::is_regular_file(workspace.Path("Cache")),
			"rollback must preserve pre-existing paths");
		Require(result.m_context.GetRoot().empty(), "failed recovery should not publish a partial context");
	}

	void TestManifestDiscoveryFailures()
	{
		{
			TempDirectory workspace("ambiguous");
			WriteManifest(workspace.Path("First.sailor"));
			WriteManifest(workspace.Path("Second.SAILOR"));

			const WorkspaceContextResolveResult result = ResolveWorkspaceContext(workspace.Get());
			Require(result.m_status == EWorkspaceContextResolveStatus::ManifestAmbiguous,
				"multiple manifests should be rejected: " + result.m_message);
			Require(!std::filesystem::exists(workspace.Path("Content")),
				"ambiguous discovery must not recover directories");
		}
		{
			TempDirectory workspace("corrupt");
			WriteText(workspace.Path("workspace.sailor"), "manifestVersion: [");

			const WorkspaceContextResolveResult result = ResolveWorkspaceContext(workspace.Get());
			Require(result.m_status == EWorkspaceContextResolveStatus::ManifestInvalid,
				"corrupt manifest should be rejected: " + result.m_message);
			Require(result.m_message.find("invalid YAML") != std::string::npos,
				"corrupt manifest should produce an actionable YAML diagnostic");
		}
		{
			TempDirectory workspace("future-version");
			ManifestSpec spec;
			spec.m_version = 999;
			WriteManifest(workspace.Path("workspace.sailor"), spec);

			const WorkspaceContextResolveResult result = ResolveWorkspaceContext(workspace.Get());
			Require(result.m_status == EWorkspaceContextResolveStatus::ManifestInvalid,
				"future manifest version should be rejected: " + result.m_message);
			Require(result.m_message.find("unsupported manifestVersion") != std::string::npos,
				"future version diagnostic should identify the version contract");
		}
		{
			TempDirectory workspace("missing-id");
			ManifestSpec spec;
			spec.m_workspaceId.clear();
			WriteManifest(workspace.Path("workspace.sailor"), spec);

			const WorkspaceContextResolveResult result = ResolveWorkspaceContext(workspace.Get());
			Require(result.m_status == EWorkspaceContextResolveStatus::ManifestInvalid,
				"missing workspace id should be rejected: " + result.m_message);
			Require(result.m_message.find("workspaceId") != std::string::npos,
				"missing field diagnostic should identify workspaceId");
		}
		{
			TempDirectory workspace("missing-engine-path");
			ManifestSpec spec;
			spec.m_enginePath.clear();
			WriteManifest(workspace.Path("workspace.sailor"), spec);

			const WorkspaceContextResolveResult result = ResolveWorkspaceContext(workspace.Get());
			Require(result.m_status == EWorkspaceContextResolveStatus::ManifestInvalid,
				"missing engine path should be rejected: " + result.m_message);
			Require(result.m_message.find("enginePath") != std::string::npos,
				"missing field diagnostic should identify enginePath");
		}
		{
			TempDirectory workspace("invalid-engine-reference");
			ManifestSpec spec;
			spec.m_engineReferenceKind = "remote";
			WriteManifest(workspace.Path("workspace.sailor"), spec);

			const WorkspaceContextResolveResult result = ResolveWorkspaceContext(workspace.Get());
			Require(result.m_status == EWorkspaceContextResolveStatus::ManifestInvalid,
				"unsupported engine reference kind should be rejected: " + result.m_message);
			Require(result.m_message.find("engineReferenceKind") != std::string::npos,
				"engine reference diagnostic should identify its field");
		}
		{
			TempDirectory workspace("missing-explicit");

			const WorkspaceContextResolveResult result = ResolveWorkspaceContext(
				workspace.Get(),
				workspace.Path("Missing.sailor"));
			Require(result.m_status == EWorkspaceContextResolveStatus::ManifestNotFound,
				"missing requested manifest should be distinguished");
		}
	}

	void TestUnsafeOwnedPaths()
	{
		struct OwnedPathField
		{
			const char* m_name;
			std::string ManifestSpec::* m_value;
		};

		const std::vector<OwnedPathField> fields
		{
			{ "contentPath", &ManifestSpec::m_content },
			{ "cachePath", &ManifestSpec::m_cache },
			{ "sourcePath", &ManifestSpec::m_source },
			{ "generatedProjectPath", &ManifestSpec::m_generated },
			{ "buildPath", &ManifestSpec::m_build },
			{ "logicOutputPath", &ManifestSpec::m_logicOutput }
		};
		const std::vector<std::string> unsafePaths
		{
			"../Outside",
			"Nested/../../Outside",
			"/Absolute/Content",
			"C:/Absolute/Content",
			"C:DriveRelative",
			"\\\\Server\\Share\\Content",
			std::string("Content\0Invalid", 15)
		};

		for (size_t fieldIndex = 0; fieldIndex < fields.size(); ++fieldIndex)
		{
			for (size_t pathIndex = 0; pathIndex < unsafePaths.size(); ++pathIndex)
			{
				TempDirectory workspace(("unsafe-" + std::to_string(fieldIndex) + "-" +
					std::to_string(pathIndex)).c_str());
				ManifestSpec spec;
				spec.*(fields[fieldIndex].m_value) = unsafePaths[pathIndex];
				WriteManifest(workspace.Path("workspace.sailor"), spec);

				const WorkspaceContextResolveResult result = ResolveWorkspaceContext(workspace.Get());
				Require(result.m_status == EWorkspaceContextResolveStatus::PathInvalid,
					"unsafe path should be rejected for '" + std::string(fields[fieldIndex].m_name) +
						"': '" + unsafePaths[pathIndex] + "': " + result.m_message);
				Require(result.m_message.find(fields[fieldIndex].m_name) != std::string::npos,
					"unsafe path diagnostic should identify its manifest field");
				Require(!std::filesystem::exists(workspace.Path("Content")),
					"unsafe path validation must precede default recovery");
			}
		}
	}

	void TestPhysicalEscape()
	{
		TempDirectory workspace("physical-escape");
		TempDirectory external("physical-external");
		std::filesystem::create_directories(external.Path("Content"));
		const std::filesystem::path linkPath = workspace.Path("ExternalLink");
#if defined(_WIN32)
		const std::string command = "cmd.exe /d /c mklink /J \"" +
			linkPath.string() + "\" \"" + external.Get().string() + "\" >NUL";
		Require(std::system(command.c_str()) == 0,
			"Windows junction fixture should be creatable");
#else
		std::error_code linkError;
		std::filesystem::create_directory_symlink(
			external.Get(),
			linkPath,
			linkError);
		Require(!linkError, "directory symlink fixture should be creatable: " + linkError.message());
#endif

		ManifestSpec spec;
		spec.m_content = "ExternalLink/Content";
		WriteManifest(workspace.Path("workspace.sailor"), spec);

		const WorkspaceContextResolveResult result = ResolveWorkspaceContext(workspace.Get());

		Require(result.m_status == EWorkspaceContextResolveStatus::PathInvalid,
			"physical content escape should be rejected: " + result.m_message);
		Require(result.m_message.find("physical resolution") != std::string::npos,
			"physical escape diagnostic should explain containment failure");
	}
}

int main()
{
	try
	{
		TestLegacyFallbackAndRecovery();
		TestManifestPathsWithSpaces();
		TestManifestDefaultRecovery();
		TestManifestDefaultsMissingEngineReferenceKindToSource();
		TestMissingCustomContentDoesNotMutateWorkspace();
		TestRecoveryRollbackOnLaterFailure();
		TestManifestDiscoveryFailures();
		TestUnsafeOwnedPaths();
		TestPhysicalEscape();
		std::cout << "[PASS] Workspace context contract" << std::endl;
		return 0;
	}
	catch (const std::exception& e)
	{
		std::cerr << "[FAIL] Workspace context contract: " << e.what() << std::endl;
		return 1;
	}
}
