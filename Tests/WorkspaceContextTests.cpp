#include "Workspace/WorkspaceContext.h"
#include "Workspace/WorkspacePathEncoding.h"
#include "Sailor.h"

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>
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
		std::string m_enginePath = "Engine";
		std::string m_engineReferenceKind = "source";
		bool m_includeEngineReferenceKind = true;
		bool m_includeBuildPath = true;
		bool m_includeLogicOutputPath = true;
		bool m_includeLogicModuleName = true;
		bool m_createEngineContent = true;
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

	void Require(bool condition, std::string_view message)
	{
		if (!condition)
		{
			throw std::runtime_error(std::string(message));
		}
	}

	std::filesystem::path Canonical(const std::filesystem::path& path)
	{
		std::error_code pathError;
		const std::filesystem::path canonical = std::filesystem::canonical(path, pathError);
		Require(!pathError, "test path should be canonicalizable: " + PathToUtf8(path));
		return canonical;
	}

	void CreateDirectoryLink(const std::filesystem::path& link, const std::filesystem::path& target)
	{
#if defined(_WIN32)
		const std::wstring command = L"cmd.exe /d /c mklink /J \"" + link.native() + L"\" \"" + target.native() + L"\" >NUL";
		Require(_wsystem(command.c_str()) == 0, "Windows junction fixture should be creatable");
#else
		std::error_code error;
		std::filesystem::create_directory_symlink(target, link, error);
		Require(!error, "directory symlink fixture should be creatable: " + error.message());
#endif
	}

	void WriteManifest(
		const std::filesystem::path& path,
		const ManifestSpec& spec = {})
	{
		if (spec.m_createEngineContent && !spec.m_enginePath.empty())
		{
			const std::filesystem::path engineReference = PathFromUtf8(spec.m_enginePath);
			const std::filesystem::path engineRoot = engineReference.is_absolute()
				? engineReference
				: path.parent_path() / engineReference;
			std::error_code createError;
			std::filesystem::create_directories(engineRoot / "Content", createError);
			Require(!createError,
				"test engine Content should be creatable: " + PathToUtf8(engineRoot));
		}

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
		if (spec.m_includeBuildPath)
		{
			manifest["buildPath"] = spec.m_build;
		}
		if (spec.m_includeLogicOutputPath)
		{
			manifest["logicOutputPath"] = spec.m_logicOutput;
		}
		if (spec.m_includeLogicModuleName)
		{
			manifest["logicModuleName"] = spec.m_moduleName;
		}

		std::ofstream output(path);
		output << manifest;
		Require(output.good(), "test manifest should be writable: " + PathToUtf8(path));
	}

	void WriteText(const std::filesystem::path& path, std::string_view value)
	{
		std::ofstream output(path);
		output << value;
		Require(output.good(), "test file should be writable: " + PathToUtf8(path));
	}

	std::string BuildManifestPayload(
		const std::string& versionDeclaration,
		const std::string& overriddenField = {},
		const std::string& overriddenValue = {})
	{
		std::string payload = versionDeclaration;
		auto appendField = [&](const char* fieldName, const char* defaultValue)
		{
			payload += fieldName;
			payload += ": ";
			payload += overriddenField == fieldName ? overriddenValue : defaultValue;
			payload += '\n';
		};

		appendField("workspaceId", "workspace-id");
		appendField("name", "Sandbox");
		appendField("enginePath", "Engine");
		appendField("engineReferenceKind", "source");
		appendField("contentPath", "Content");
		appendField("cachePath", "Cache");
		appendField("sourcePath", "Source");
		appendField("generatedProjectPath", "Generated");
		appendField("buildPath", "Cache/Build");
		appendField("logicOutputPath", "Binaries");
		appendField("logicModuleName", "SailorGame");
		return payload;
	}

	void RequireManifestRejectedWithoutMutation(
		const WorkspaceContextResolveResult& result,
		const TempDirectory& workspace,
		const std::string& scenario)
	{
		Require(result.m_status == EWorkspaceContextResolveStatus::ManifestInvalid,
			scenario + " should be rejected as an invalid manifest: " + result.m_message);
		Require(!result.IsSuccess(), scenario + " must not publish a successful context");
		Require(result.m_context.GetRoot().empty(),
			scenario + " must not publish a partial context");
		Require(result.m_context.GetManifest().empty(),
			scenario + " must not publish a manifest path");
		Require(!std::filesystem::exists(workspace.Path("Content")),
			scenario + " must be rejected before Content recovery");
		Require(!std::filesystem::exists(workspace.Path("Cache")),
			scenario + " must be rejected before Cache recovery");
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
		Require(result.m_context.GetEngineRoot() == result.m_context.GetRoot(),
			"legacy context should deduplicate the engine and workspace roots");
		Require(result.m_context.GetEngineContent() == result.m_context.GetContent(),
			"legacy context should deduplicate engine and workspace Content");
		Require(result.m_context.IsEngineMode(),
			"legacy same-root context should identify Engine mode");
		Require(result.m_context.GetCache() == Canonical(workspace.Path("Cache")),
			"legacy context should use the default Cache directory");
		Require(std::filesystem::is_directory(workspace.Path("Content")),
			"legacy resolution should recreate default Content");
		Require(std::filesystem::is_directory(workspace.Path("Cache")),
			"legacy resolution should recreate default Cache");
	}

	void TestUtf8CommandLinePathConversion()
	{
		const std::string utf8Component =
			reinterpret_cast<const char*>(u8"Physical-РАБОЧАЯ-AZ");
		const std::filesystem::path component = PathFromUtf8(utf8Component);
		Require(PathToUtf8(component) == utf8Component,
			"command-line workspace paths must round-trip through the native path type as UTF-8");
		std::string source = "prefix/" + utf8Component + "/ignored";
		const auto bounded = PathFromUtf8(std::string_view(source).substr(7, utf8Component.size()));
		source.assign(1024, 'x');
		Require(PathToUtf8(bounded) == utf8Component,
			"UTF-8 paths must own exactly the view's bytes after the source changes");
		Require(PathFromUtf8({}).empty(), "an empty view must produce an empty path");
		const char unterminated[] = { 'A', '\xc3', '\xa9' };
		Require(PathToUtf8(PathFromUtf8(std::string_view(unterminated, sizeof(unterminated)))) == "A\xc3\xa9",
			"UTF-8 path conversion must accept a bounded range without a terminator");

		TempDirectory fixture("utf8-command-line");
		const std::filesystem::path workspaceRoot = fixture.Path(component);
		std::filesystem::create_directories(workspaceRoot);
		const WorkspaceContextResolveResult result = ResolveWorkspaceContext(workspaceRoot);
		Require(result.IsSuccess(),
			"a workspace root decoded from UTF-8 command-line bytes should resolve: " + result.m_message);
		Require(result.m_context.GetRoot() == Canonical(workspaceRoot),
			"UTF-8 command-line path conversion must preserve the physical workspace root");
	}

	void TestManifestTextViews()
	{
		TempDirectory workspace("manifest-text-views");
		std::filesystem::create_directories(workspace.Path("Engine/Content"));
		std::filesystem::create_directories(workspace.Path("Content"));
		const auto manifestPath = workspace.Path("workspace.sailor");
		WorkspaceContextResolveResult result;
		{
			ManifestSpec spec;
			spec.m_createEngineContent = false;
			spec.m_workspaceId = " \tarchipelago-workspace\r\n";
			spec.m_name = " \tDrifting Archipelago\r\n";
			spec.m_enginePath = " \tEngine\r\n";
			spec.m_engineReferenceKind = " \tSOURCE\r\n";
			spec.m_content = " \t./Content/\r\n";
			spec.m_cache = " \t./Cache/\r\n";
			spec.m_moduleName = " \tArchipelagoLogic\r\n";
			WriteManifest(manifestPath, spec);
			result = ResolveWorkspaceContext(workspace.Get());
		}
		WriteText(manifestPath, "name: replaced\n");
		Require(result.IsSuccess(), "trimmed manifest fields should resolve: " + result.m_message);
		Require(result.m_context.GetWorkspaceId() == "archipelago-workspace" &&
			result.m_context.GetWorkspaceName() == "Drifting Archipelago" &&
			result.m_context.GetModuleName() == "ArchipelagoLogic",
			"workspace metadata must own trimmed text after its YAML document is destroyed");
		Require(result.m_context.GetEngineRoot() == Canonical(workspace.Path("Engine")) &&
			result.m_context.GetContent() == Canonical(workspace.Path("Content")) &&
			result.m_context.GetCache() == Canonical(workspace.Path("Cache")),
			"borrowed manifest values must preserve owned, normalized workspace paths");

		ManifestSpec emptyName;
		emptyName.m_name = " \t\r\n";
		WriteManifest(manifestPath, emptyName);
		const auto invalid = ResolveWorkspaceContext(workspace.Get());
		Require(!invalid.IsSuccess() && invalid.m_message == "Workspace manifest field 'name' is required.",
			"whitespace-only required text must preserve the readable field diagnostic");
	}

	void TestManifestPathsWithSpaces()
	{
		TempDirectory workspace("spaces");
		ManifestSpec spec;
		spec.m_workspaceId = "workspace with spaces";
		spec.m_name = "Sandbox With Spaces";
		spec.m_enginePath = "Engine Install With Spaces";
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
		Require(result.m_context.GetEngineRoot() == Canonical(workspace.Path(spec.m_enginePath)),
			"engine root should preserve spaces");
		Require(result.m_context.GetEngineContent() ==
			Canonical(workspace.Path(spec.m_enginePath) / "Content"),
			"engine Content should resolve below the engine root");
		Require(result.m_context.GetContent() == Canonical(workspace.Path(spec.m_content)),
			"custom content path should preserve spaces");
		Require(!result.m_context.IsEngineMode(),
			"a manifest with distinct Engine and Workspace Content should not identify Engine mode");
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

	void TestCanonicalPathComparison()
	{
#if defined(_WIN32)
		const auto root = std::filesystem::path(L"C:/Workspace/\u00c9tude").lexically_normal();
		const auto differentCase = std::filesystem::path(L"c:\\workspace\\\u00e9tude\\Cache").lexically_normal();
		Require(IsPathWithin(root, differentCase), "Windows containment must compare Unicode case ordinally and accept native separators");
#else
		const auto root = PathFromUtf8(reinterpret_cast<const char*>(u8"/Workspace/\u00c9tude"));
		const auto differentCase = PathFromUtf8(reinterpret_cast<const char*>(u8"/workspace/\u00e9tude/Cache"));
		Require(!IsPathWithin(root, differentCase), "POSIX containment must keep exact component comparison");
#endif
		Require(IsPathWithin(root, root) && IsPathWithin(root, root / "Cache/Build"),
			"a canonical root must contain itself and nested, possibly nonexistent paths");
		Require(!IsPathWithin(root, root.parent_path()), "an ancestor is not inside its child");
		auto sibling = root;
		sibling += "-other";
		Require(!IsPathWithin(root, sibling / "Cache"), "a shared text prefix is not a directory boundary");
		Require(!IsPathWithin(root, root.parent_path() / "Etude/Cache"),
			"path comparison must not fold distinct accented names linguistically");
	}

	void TestCompleteResolvedContextOwnership()
	{
		TempDirectory workspace("complete-context");
		const std::string name = reinterpret_cast<const char*>(u8"Project \u042f \u00e9 \u8239");
		ManifestSpec spec;
		spec.m_workspaceId = "id-" + name;
		spec.m_name = name;
		spec.m_enginePath = name + "/Engine";
		spec.m_content = name + "/Content";
		spec.m_cache = name + "/Cache";
		spec.m_source = name + "/Source";
		spec.m_generated = name + "/Generated";
		spec.m_build = name + "/Build";
		spec.m_logicOutput = name + "/Binaries";
		spec.m_moduleName = "OceanLogic";
		auto ownedPath = [&](std::string_view value)
		{
			return std::filesystem::weakly_canonical(workspace.Path(PathFromUtf8(value)));
		};
		std::filesystem::create_directories(ownedPath(spec.m_content));
		const auto manifest = workspace.Path("workspace.sailor");
		WriteManifest(manifest, spec);
		auto result = ResolveWorkspaceContext(workspace.Get());
		Require(result.IsSuccess(), "the Unicode context must resolve: " + result.m_message);
		auto context = std::move(result.m_context);
		result = {};
		WriteText(manifest, "manifestVersion: [");
		Require(!ResolveWorkspaceContext(workspace.Get()).IsSuccess(), "the replacement manifest must be rejected");
		Require(context.GetRoot() == Canonical(workspace.Get()) && context.GetManifest() == Canonical(manifest) &&
			context.GetEngineRoot() == ownedPath(spec.m_enginePath) &&
			context.GetEngineContent() == ownedPath(spec.m_enginePath + "/Content") &&
			context.GetContent() == ownedPath(spec.m_content) && context.GetCache() == ownedPath(spec.m_cache) &&
			context.GetSource() == ownedPath(spec.m_source) && context.GetGenerated() == ownedPath(spec.m_generated) &&
			context.GetBuild() == ownedPath(spec.m_build) && context.GetLogicOutput() == ownedPath(spec.m_logicOutput),
			"all resolved paths must survive moving the context and resolving a later invalid document");
		Require(context.GetWorkspaceId() == spec.m_workspaceId && context.GetWorkspaceName() == spec.m_name &&
			context.GetModuleName() == spec.m_moduleName && context.GetManifestVersion() == 1 &&
			!context.IsLegacy() && !context.IsEngineMode(), "the resolved context must retain all manifest metadata");
		Require(context.GetProjectSettingsPath() == Canonical(workspace.Get()) / "ProjectSettings.yaml" &&
			context.GetEditorSettingsPath() == context.GetCache() / "EditorSettings.yaml",
			"derived settings paths must remain owned by the resolved workspace and cache");
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

	void TestManifestDefaultsMissingOptionalV1Fields()
	{
		TempDirectory workspace("default-optional-fields");
		ManifestSpec spec;
		spec.m_includeEngineReferenceKind = false;
		spec.m_includeBuildPath = false;
		spec.m_includeLogicOutputPath = false;
		spec.m_includeLogicModuleName = false;
		WriteManifest(workspace.Path("workspace.sailor"), spec);

		const WorkspaceContextResolveResult result = ResolveWorkspaceContext(workspace.Get());

		Require(result.IsSuccess(),
			"v1 manifest without optional fields should use v1 defaults: " + result.m_message);
		Require(result.m_context.GetBuild() ==
			std::filesystem::weakly_canonical(workspace.Path("Cache/Build")),
			"missing buildPath should default to Cache/Build");
		Require(result.m_context.GetLogicOutput() ==
			std::filesystem::weakly_canonical(workspace.Path("Binaries")),
			"missing logicOutputPath should default to Binaries");
		Require(result.m_context.GetModuleName() == "SailorGame",
			"missing logicModuleName should default to SailorGame");
	}

	void TestExplicitOptionalFieldsAreInvalid()
	{
		const std::vector<std::string> fields
		{
			"engineReferenceKind",
			"buildPath",
			"logicOutputPath",
			"logicModuleName"
		};
		struct InvalidValue
		{
			const char* m_label;
			const char* m_yaml;
		};
		const std::vector<InvalidValue> invalidValues
		{
			{ "null", "null" },
			{ "empty", "''" }
		};

		for (const std::string& field : fields)
		{
			for (const InvalidValue& invalidValue : invalidValues)
			{
				const std::string scenario = "explicit-" + std::string(invalidValue.m_label) + "-" + field;
				TempDirectory workspace(scenario.c_str());
				WriteText(
					workspace.Path("workspace.sailor"),
					BuildManifestPayload(
						"manifestVersion: 1\n",
						field,
						invalidValue.m_yaml));

				const WorkspaceContextResolveResult result = ResolveWorkspaceContext(workspace.Get());

				RequireManifestRejectedWithoutMutation(result, workspace, scenario);
				Require(result.m_message.find(field) != std::string::npos,
					scenario + " diagnostic should identify its field: " + result.m_message);
			}
		}
	}

	void TestRelativeEnginePathOutsideWorkspace()
	{
		TempDirectory workspace("relative-engine-workspace");
		TempDirectory engine("relative-engine-root");
		std::filesystem::create_directories(engine.Path("Content"));
		std::error_code relativeError;
		const std::filesystem::path relativeEngine = std::filesystem::relative(
			engine.Get(),
			workspace.Get(),
			relativeError);
		Require(!relativeError, "relative engine fixture should be computable");

		ManifestSpec spec;
		spec.m_enginePath = PathToUtf8(relativeEngine);
		spec.m_createEngineContent = false;
		WriteManifest(workspace.Path("workspace.sailor"), spec);

		const WorkspaceContextResolveResult result = ResolveWorkspaceContext(workspace.Get());

		Require(result.IsSuccess(), "relative engine path should resolve outside the workspace: " + result.m_message);
		Require(result.m_context.GetEngineRoot() == Canonical(engine.Get()),
			"relative engine path should be canonicalized from the workspace root");
		Require(result.m_context.GetEngineContent() == Canonical(engine.Path("Content")),
			"relative engine Content should be canonicalized");
	}

	void TestAbsoluteEnginePath()
	{
		TempDirectory workspace("absolute-engine-workspace");
		TempDirectory engine("absolute-engine-root");
		std::filesystem::create_directories(engine.Path("Content"));

		ManifestSpec spec;
		spec.m_enginePath = PathToUtf8(engine.Get());
		spec.m_createEngineContent = false;
		WriteManifest(workspace.Path("workspace.sailor"), spec);

		const WorkspaceContextResolveResult result = ResolveWorkspaceContext(workspace.Get());

		Require(result.IsSuccess(), "absolute engine path should resolve: " + result.m_message);
		Require(result.m_context.GetEngineRoot() == Canonical(engine.Get()),
			"absolute engine root should be canonicalized without workspace containment");
		Require(result.m_context.GetEngineContent() == Canonical(engine.Path("Content")),
			"absolute engine Content should be canonicalized");
	}

	void TestMissingEngineContentDoesNotMutateWorkspace()
	{
		TempDirectory workspace("missing-engine-content-workspace");
		TempDirectory engine("missing-engine-content-root");
		ManifestSpec spec;
		spec.m_enginePath = PathToUtf8(engine.Get());
		spec.m_createEngineContent = false;
		WriteManifest(workspace.Path("workspace.sailor"), spec);

		const WorkspaceContextResolveResult result = ResolveWorkspaceContext(workspace.Get());

		Require(result.m_status == EWorkspaceContextResolveStatus::PathInvalid,
			"missing engine Content should be rejected: " + result.m_message);
		Require(result.m_message.find("Engine Content") != std::string::npos,
			"missing engine Content diagnostic should identify the invalid directory");
		Require(result.m_context.GetRoot().empty(),
			"missing engine Content must not publish a partial context");
		Require(!std::filesystem::exists(workspace.Path("Content")),
			"engine validation must precede workspace Content recovery");
		Require(!std::filesystem::exists(workspace.Path("Cache")),
			"engine validation must precede workspace Cache recovery");
	}

	void TestMissingInstalledEngineContentHasTargetedDiagnostic()
	{
		TempDirectory workspace("missing-installed-engine-content-workspace");
		TempDirectory engine("missing-installed-engine-content-root");
		ManifestSpec spec;
		spec.m_enginePath = PathToUtf8(engine.Get());
		spec.m_engineReferenceKind = "installed";
		spec.m_createEngineContent = false;
		WriteManifest(workspace.Path("workspace.sailor"), spec);

		const WorkspaceContextResolveResult result = ResolveWorkspaceContext(workspace.Get());

		Require(result.m_status == EWorkspaceContextResolveStatus::PathInvalid,
			"installed engine without runtime Content should be rejected: " + result.m_message);
		Require(result.m_message.find("Installed engine reference") != std::string::npos,
			"installed engine diagnostic should identify the reference kind");
		Require(result.m_message.find("Packaging runtime Content") != std::string::npos,
			"installed engine diagnostic should identify the known packaging limitation");
		Require(result.m_context.GetRoot().empty(),
			"missing installed engine Content must not publish a partial context");
		Require(!std::filesystem::exists(workspace.Path("Content")),
			"installed engine validation must precede workspace Content recovery");
		Require(!std::filesystem::exists(workspace.Path("Cache")),
			"installed engine validation must precede workspace Cache recovery");
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

	void TestManifestVersionPreflightDoesNotMutateWorkspace()
	{
		struct InvalidVersion
		{
			const char* m_label;
			const char* m_declaration;
			const char* m_expectedDiagnostic;
		};
		const std::vector<InvalidVersion> invalidVersions
		{
			{ "missing-version", "", "required" },
			{ "zero-version", "manifestVersion: 0\n", "unsupported manifestVersion" },
			{ "future-version", "manifestVersion: 999\n", "unsupported manifestVersion" },
			{ "duplicate-version", "manifestVersion: 1\nmanifestVersion: 1\n", "duplicate" },
			{ "non-scalar-version", "manifestVersion: [1]\n", "unsigned integer scalar" }
		};

		for (const InvalidVersion& invalidVersion : invalidVersions)
		{
			TempDirectory workspace(invalidVersion.m_label);
			WriteText(
				workspace.Path("workspace.sailor"),
				BuildManifestPayload(invalidVersion.m_declaration));

			const WorkspaceContextResolveResult result = ResolveWorkspaceContext(workspace.Get());

			RequireManifestRejectedWithoutMutation(result, workspace, invalidVersion.m_label);
			Require(result.m_message.find("manifestVersion") != std::string::npos,
				std::string(invalidVersion.m_label) +
					" diagnostic should identify manifestVersion: " + result.m_message);
			Require(result.m_message.find(invalidVersion.m_expectedDiagnostic) != std::string::npos,
				std::string(invalidVersion.m_label) +
					" diagnostic should explain the version failure: " + result.m_message);
		}

		TempDirectory workspace("future-version-precedence");
		WriteText(
			workspace.Path("workspace.sailor"),
			BuildManifestPayload(
				"manifestVersion: 999\n",
				"workspaceId",
				"[malformed, later, field]"));

		const WorkspaceContextResolveResult result = ResolveWorkspaceContext(workspace.Get());

		RequireManifestRejectedWithoutMutation(result, workspace, "future-version-precedence");
		Require(result.m_message.find("unsupported manifestVersion '999'") != std::string::npos,
			"future manifest diagnostic should precede malformed later fields: " + result.m_message);
		Require(result.m_message.find("workspaceId") == std::string::npos,
			"future manifest diagnostic must not be replaced by later field validation");
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

	void TestWorkspaceOwnedDirectoryLink()
	{
		TempDirectory workspace("physical-alias");
		const auto target = workspace.Path(PathFromUtf8(reinterpret_cast<const char*>(u8"Project \u042f \u00e9 \u8239")));
		std::filesystem::create_directories(target / "Content");
		CreateDirectoryLink(workspace.Path("OwnedLink"), target);
		ManifestSpec spec;
		spec.m_enginePath = "OwnedLink/Engine";
		spec.m_content = "OwnedLink/Content";
		spec.m_cache = "OwnedLink/Cache";
		spec.m_source = "OwnedLink/Source";
		spec.m_generated = "OwnedLink/Generated";
		spec.m_build = "OwnedLink/Cache/Build";
		spec.m_logicOutput = "OwnedLink/Binaries";
		WriteManifest(workspace.Path("workspace.sailor"), spec);
		const auto result = ResolveWorkspaceContext(workspace.Get());
		Require(result.IsSuccess(), "links to workspace-owned directories must resolve: " + result.m_message);
		const auto physicalRoot = Canonical(target);
		const auto& context = result.m_context;
		Require(context.GetContent() == physicalRoot / "Content" && context.GetCache() == physicalRoot / "Cache" &&
			context.GetSource() == physicalRoot / "Source" && context.GetGenerated() == physicalRoot / "Generated" &&
			context.GetBuild() == physicalRoot / "Cache/Build" && context.GetLogicOutput() == physicalRoot / "Binaries" &&
			context.GetEngineRoot() == physicalRoot / "Engine" && context.GetEngineContent() == physicalRoot / "Engine/Content",
			"existing and missing paths must resolve through the link to their physical workspace-owned location");
		Require(std::filesystem::is_directory(target / "Cache") && std::filesystem::equivalent(workspace.Path("OwnedLink"), target),
			"cache recovery must create the directory at the target without replacing the directory link");
	}

	void TestPhysicalEscape()
	{
		TempDirectory workspace("physical-escape");
		TempDirectory external("physical-external");
		std::filesystem::create_directories(external.Path("Content"));
		CreateDirectoryLink(workspace.Path("ExternalLink"), external.Get());

		for (auto field : { &ManifestSpec::m_content, &ManifestSpec::m_cache, &ManifestSpec::m_source,
			&ManifestSpec::m_generated, &ManifestSpec::m_build, &ManifestSpec::m_logicOutput })
		{
			ManifestSpec spec;
			spec.*field = "ExternalLink/Content";
			WriteManifest(workspace.Path("workspace.sailor"), spec);
			const auto result = ResolveWorkspaceContext(workspace.Get());
			Require(result.m_status == EWorkspaceContextResolveStatus::PathInvalid,
				"every physically escaping workspace-owned path must be rejected: " + result.m_message);
			Require(result.m_message.find("physical resolution") != std::string::npos,
				"physical escape diagnostic should explain containment failure");
			Require(result.m_context.GetRoot().empty() && !std::filesystem::exists(workspace.Path("Content")) &&
				!std::filesystem::exists(workspace.Path("Cache")), "path failure must not publish or recover a partial workspace");
		}
		WriteText(external.Path("Foreign.sailor"), "not a workspace document");
		const auto manifestResult = ResolveWorkspaceContext(workspace.Get(), "ExternalLink/Foreign.sailor");
		Require(manifestResult.m_status == EWorkspaceContextResolveStatus::PathInvalid &&
			manifestResult.m_context.GetRoot().empty(), "physical manifest ownership must be checked before parsing");
	}

	void TestAppWorkspaceSwitch()
	{
		TempDirectory first("switch-first");
		TempDirectory second("switch-second");
		ManifestSpec firstSpec;
		firstSpec.m_moduleName = "MissingFirstLogic";
		ManifestSpec secondSpec;
		secondSpec.m_workspaceId = "second-workspace";
		secondSpec.m_name = "Second project";
		secondSpec.m_enginePath = "OtherEngine";
		secondSpec.m_cache = "OtherCache";
		secondSpec.m_source = "OtherSource";
		secondSpec.m_generated = "OtherGenerated";
		secondSpec.m_build = "OtherCache/Build";
		secondSpec.m_logicOutput = "OtherBinaries";
		secondSpec.m_moduleName = "MissingSecondLogic";
		WriteManifest(first.Path("workspace.sailor"), firstSpec);
		WriteManifest(second.Path("workspace.sailor"), secondSpec);
		for (const TempDirectory* workspace : { &first, &second, &first })
		{
			const auto& spec = workspace == &first ? firstSpec : secondSpec;
			const auto root = Canonical(workspace->Get());
			const auto projectPath = [&](std::string_view path) { return root / PathFromUtf8(path); };
			const std::string rootArgument = PathToUtf8(root);
			const char* arguments[] = { "Sailor", "--noconsole", "--null-audio", "--workspace", rootArgument.c_str() };
			const auto initialized = Sailor::App::Initialize(arguments, static_cast<int32_t>(std::size(arguments)));
			const auto context = Sailor::App::GetWorkspaceContext();
			const auto publishedRoot = Sailor::App::GetWorkspace();
			const bool bHasRenderer = Sailor::App::IsRendererInitialized();
			const bool bShutdown = Sailor::App::Shutdown();
			Require(initialized == Sailor::EAppInitializationResult::Failed && !bHasRenderer && bShutdown,
				"workspace switching must stop at the missing logic module without creating a renderer");
			Require(context.GetRoot() == root && publishedRoot == rootArgument + "/" &&
				context.GetManifest() == root / "workspace.sailor" && context.GetContent() == root / "Content" &&
				context.GetEngineRoot() == projectPath(spec.m_enginePath) && context.GetEngineContent() == projectPath(spec.m_enginePath) / "Content" &&
				context.GetCache() == projectPath(spec.m_cache) && context.GetSource() == projectPath(spec.m_source) &&
				context.GetGenerated() == projectPath(spec.m_generated) && context.GetBuild() == projectPath(spec.m_build) &&
				context.GetLogicOutput() == projectPath(spec.m_logicOutput) && context.GetModuleName() == spec.m_moduleName &&
				context.GetWorkspaceId() == spec.m_workspaceId && context.GetWorkspaceName() == spec.m_name &&
				!context.IsLegacy() && context.GetManifestVersion() == 1,
				"switching A to B and back must replace every published workspace path and metadata field");
		}
	}

	void TestAppBootstrapArguments()
	{
		TempDirectory fixture("bootstrap-arguments");
		const auto root = fixture.Path(PathFromUtf8(reinterpret_cast<const char*>(u8"Skipper \u042f \u00e9 \u8239 \U0001f6a2")));
		std::filesystem::create_directories(root);
		const auto manifestPath = root / PathFromUtf8(reinterpret_cast<const char*>(u8"Project \u00e9.sailor"));
		ManifestSpec spec;
		spec.m_moduleName = "MissingBootstrapModule";
		WriteManifest(manifestPath, spec);
		const auto canonicalRoot = Canonical(root);
		const std::string expectedRoot = PathToUtf8(canonicalRoot) + "/";
		const std::vector<std::string> worldArguments{ Sailor::Utils::wchar_to_UTF8(L"\"literal \u00e9 \U0001f6a2\""),
			"", "\"", "\"unfinished", "Sea path with spaces.world", "--literal" };
		for (bool bExplicitRoot : { true, false })
		{
			for (const auto& expectedWorld : worldArguments)
			{
				Sailor::EAppInitializationResult initialization;
				{
					Sailor::TVector<std::string> storage{ "Sailor", "--noconsole", "--null-audio", "--no-title-stats",
						"--world", expectedWorld, "--workspace-manifest", PathToUtf8(manifestPath) };
					if (bExplicitRoot) storage.AddRange({ "--workspace", PathToUtf8(root) });
					Sailor::TVector<const char*> arguments;
					for (const auto& argument : storage) arguments.Add(argument.c_str());
					initialization = Sailor::App::Initialize(arguments.GetData(), static_cast<int32_t>(arguments.Num()));
					for (auto& argument : storage) argument.assign(4096, 'x');
				}
				const std::string world = Sailor::App::GetLoadedWorldPath();
				const std::string publishedRoot = Sailor::App::GetWorkspace();
				const auto context = Sailor::App::GetWorkspaceContext();
				const bool bHasRenderer = Sailor::App::IsRendererInitialized();
				const bool bShutdown = Sailor::App::Shutdown();
				Require(initialization == Sailor::EAppInitializationResult::Failed && !bHasRenderer && bShutdown,
					"the missing-module fixture must resolve its workspace and stop before creating a renderer");
				Require(context.GetRoot() == canonicalRoot && publishedRoot == expectedRoot &&
					context.GetCache() == Canonical(root / "Cache") && context.GetManifest() == Canonical(manifestPath),
					"App bootstrap must preserve UTF-8 workspace, manifest and project-owned cache paths after argv expires");
				Require(world == expectedWorld,
					"App arguments are already tokenized: literal quotes and Unicode must survive without reparsing or borrowing argv");
			}
		}

		const std::string manifestArgument = PathToUtf8(manifestPath);
		const char* missingValue[] = { "Sailor", "--noconsole", "--workspace-manifest", manifestArgument.c_str(), "--world" };
		Sailor::App::Initialize(missingValue, static_cast<int32_t>(std::size(missingValue)));
		const bool bEmptyWorld = Sailor::App::GetLoadedWorldPath().empty();
		Require(Sailor::App::Shutdown() && bEmptyWorld, "a missing final value must remain empty without reading beyond argv");
	}
}

int main()
{
	try
	{
		TestLegacyFallbackAndRecovery();
		TestUtf8CommandLinePathConversion();
		TestAppBootstrapArguments();
		TestAppWorkspaceSwitch();
		TestManifestTextViews();
		TestManifestPathsWithSpaces();
		TestCanonicalPathComparison();
		TestCompleteResolvedContextOwnership();
		TestManifestDefaultRecovery();
		TestManifestDefaultsMissingOptionalV1Fields();
		TestExplicitOptionalFieldsAreInvalid();
		TestRelativeEnginePathOutsideWorkspace();
		TestAbsoluteEnginePath();
		TestMissingEngineContentDoesNotMutateWorkspace();
		TestMissingInstalledEngineContentHasTargetedDiagnostic();
		TestMissingCustomContentDoesNotMutateWorkspace();
		TestRecoveryRollbackOnLaterFailure();
		TestManifestVersionPreflightDoesNotMutateWorkspace();
		TestManifestDiscoveryFailures();
		TestUnsafeOwnedPaths();
		TestWorkspaceOwnedDirectoryLink();
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
