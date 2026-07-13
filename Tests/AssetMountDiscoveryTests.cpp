#include "AssetRegistry/AssetMountDiscovery.h"

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace
{
	using namespace Sailor;

	void Require(bool condition, const std::string& message)
	{
		if (!condition)
		{
			throw std::runtime_error(message);
		}
	}

	class TempDirectory final
	{
	public:
		explicit TempDirectory(const char* label)
		{
			static uint64_t counter = 0;
			m_path = std::filesystem::temp_directory_path() /
				("sailor-asset-mount-" + std::string(label) + "-" +
					std::to_string(++counter));
			std::filesystem::remove_all(m_path);
			std::filesystem::create_directories(m_path);
		}

		~TempDirectory()
		{
			std::error_code error;
			std::filesystem::remove_all(m_path, error);
		}

		const std::filesystem::path& Get() const noexcept { return m_path; }
		std::filesystem::path Path(const std::filesystem::path& relative) const { return m_path / relative; }

	private:
		std::filesystem::path m_path;
	};

	void WriteFile(const std::filesystem::path& path, const std::string& content = "fixture")
	{
		std::filesystem::create_directories(path.parent_path());
		std::ofstream stream(path);
		stream << content;
	}

	void CreateDirectoryLink(const std::filesystem::path& link, const std::filesystem::path& target)
	{
#if defined(_WIN32)
		const std::string command = "cmd.exe /d /c mklink /J \"" +
			link.string() + "\" \"" + target.string() + "\" >NUL";
		Require(std::system(command.c_str()) == 0,
			"Windows junction fixture should be creatable: " + link.string());
#else
		std::error_code error;
		std::filesystem::create_directory_symlink(target, link, error);
		Require(!error, "Directory symlink fixture should be creatable: " + error.message());
#endif
	}

	AssetMountDescriptor Mount(
		const std::filesystem::path& root,
		EAssetMountKind kind,
		int32_t priority,
		bool bWritable)
	{
		return AssetMountDescriptor{ root, kind, priority, bWritable };
	}

	const AssetMountDiscoveredFile& FindFile(
		const AssetMountDiscoveryResult& discovery,
		EAssetMountKind kind,
		const std::string& virtualPath)
	{
		const auto it = std::find_if(discovery.m_files.begin(), discovery.m_files.end(),
			[&](const AssetMountDiscoveredFile& file)
			{
				return file.m_mount.m_kind == kind && file.m_virtualPath == virtualPath;
			});
		Require(it != discovery.m_files.end(), "Expected discovered file: " + virtualPath);
		return *it;
	}

	AssetMountCandidate Candidate(
		const AssetMountDiscoveredFile& file,
		std::string virtualPath,
		std::string fileId)
	{
		return AssetMountCandidate{
			file.m_mount,
			file.m_physicalPath,
			std::move(virtualPath),
			std::move(fileId)
		};
	}

	bool HasDiagnostic(
		const std::vector<AssetMountDiagnostic>& diagnostics,
		EAssetMountDiagnosticCode code)
	{
		return std::any_of(diagnostics.begin(), diagnostics.end(), [&](const AssetMountDiagnostic& diagnostic)
			{
				return diagnostic.m_code == code;
			});
	}

	const AssetMountDiagnostic& FindDiagnostic(
		const std::vector<AssetMountDiagnostic>& diagnostics,
		EAssetMountDiagnosticCode code)
	{
		const auto it = std::find_if(diagnostics.begin(), diagnostics.end(), [&](const AssetMountDiagnostic& diagnostic)
			{
				return diagnostic.m_code == code;
			});
		Require(it != diagnostics.end(), "Expected mount diagnostic was not produced");
		return *it;
	}

	std::vector<std::string> DiagnosticMessages(const AssetMountResolutionResult& result)
	{
		std::vector<std::string> messages;
		for (const AssetMountDiagnostic& diagnostic : result.GetDiagnostics())
		{
			messages.emplace_back(diagnostic.m_message);
		}
		return messages;
	}

	void TestIdenticalRootsAreDeduplicatedToWorkspace()
	{
		TempDirectory content("dedupe");
		TempDirectory aliases("dedupe-alias");
		WriteFile(content.Path("asset.txt"));
		const std::filesystem::path workspaceAlias = aliases.Path("WorkspaceContent");
		CreateDirectoryLink(workspaceAlias, content.Get());

		const AssetMountDiscoveryResult result = DiscoverAssetMountFiles({
			Mount(content.Get(), EAssetMountKind::Engine, 100, false),
			Mount(workspaceAlias, EAssetMountKind::Workspace, 0, true)
		});

		Require(result.m_mounts.size() == 1, "Identical physical roots should be scanned once");
		Require(result.m_mounts.front().m_kind == EAssetMountKind::Workspace,
			"Workspace should win an identical-root mount collision");
		Require(result.m_mounts.front().m_bWritable,
			"The retained workspace mount should preserve its writable property");
		Require(result.m_files.size() == 1, "Deduplicated roots should not duplicate files");
		Require(HasDiagnostic(result.m_diagnostics, EAssetMountDiagnosticCode::DuplicateMountRoot),
			"Identical roots should emit a deterministic dedupe diagnostic");
	}

	void TestInvalidMountRootIsFatal()
	{
		TempDirectory valid("invalid-root");
		WriteFile(valid.Path("valid.asset"));
		const std::filesystem::path missing = valid.Path("MissingContent");

		const AssetMountDiscoveryResult result = DiscoverAssetMountFiles({
			Mount(valid.Get(), EAssetMountKind::Workspace, 10, true),
			Mount(missing, EAssetMountKind::Engine, 0, false)
		});

		Require(result.HasFatalErrors(), "An invalid mount root should be reported as fatal");
		Require(result.m_files.empty(), "A fatal mount set must not publish partial discovery files");
		Require(HasDiagnostic(result.m_diagnostics, EAssetMountDiagnosticCode::InvalidMountRoot),
			"Invalid roots should remain distinguishable from overlap failures");
	}

	void TestDiscoveryIsSortedByVirtualPath()
	{
		TempDirectory content("sorted");
		WriteFile(content.Path("z-last.txt"));
		WriteFile(content.Path("nested/c-middle.txt"));
		WriteFile(content.Path("a-first.txt"));

		const AssetMountDiscoveryResult result = DiscoverAssetMountFiles({
			Mount(content.Get(), EAssetMountKind::Workspace, 10, true)
		});

		std::vector<std::string> virtualPaths;
		for (const AssetMountDiscoveredFile& file : result.m_files)
		{
			virtualPaths.emplace_back(file.m_virtualPath);
		}
		Require(virtualPaths == std::vector<std::string>{
			"a-first.txt",
			"nested/c-middle.txt",
			"z-last.txt"
		}, "Discovery output should be globally sorted by normalized virtual path");
	}

	void TestOverlappingRootsAreRejectedInBothDirections()
	{
		{
			TempDirectory workspace("workspace-contains-engine");
			const std::filesystem::path engine = workspace.Path("Libraries/EngineContent");
			WriteFile(workspace.Path("workspace.asset"));
			WriteFile(engine / "engine.asset");

			const AssetMountDiscoveryResult result = DiscoverAssetMountFiles({
				Mount(workspace.Get(), EAssetMountKind::Workspace, 10, true),
				Mount(engine, EAssetMountKind::Engine, 0, false)
			});

			Require(result.HasFatalErrors(),
				"Workspace roots containing Engine roots must be rejected as fatal");
			Require(result.m_files.empty(),
				"Fatal root overlap must stop discovery before either root is scanned");
			Require(HasDiagnostic(result.m_diagnostics, EAssetMountDiagnosticCode::OverlappingMountRoot),
				"Workspace-parent overlap should identify the root conflict");
		}

		{
			TempDirectory engine("engine-contains-workspace");
			const std::filesystem::path workspace = engine.Path("Projects/WorkspaceContent");
			WriteFile(engine.Path("engine.asset"));
			WriteFile(workspace / "workspace.asset");

			const AssetMountDiscoveryResult result = DiscoverAssetMountFiles({
				Mount(engine.Get(), EAssetMountKind::Engine, 0, false),
				Mount(workspace, EAssetMountKind::Workspace, 10, true)
			});

			Require(result.HasFatalErrors(),
				"Engine roots containing Workspace roots must be rejected as fatal");
			Require(result.m_files.empty(),
				"Reverse root overlap must stop discovery before either root is scanned");
			Require(HasDiagnostic(result.m_diagnostics, EAssetMountDiagnosticCode::OverlappingMountRoot),
				"Engine-parent overlap should identify the root conflict");
		}
	}

	void TestDiscoverySkipsPhysicalEscapesAndCycles()
	{
		TempDirectory content("safe-root");
		TempDirectory outside("outside-root");
		WriteFile(content.Path("safe.txt"));
		WriteFile(outside.Path("secret.txt"));
		CreateDirectoryLink(content.Path("Escape"), outside.Get());
		CreateDirectoryLink(content.Path("Cycle"), content.Get());

		const AssetMountDiscoveryResult result = DiscoverAssetMountFiles({
			Mount(content.Get(), EAssetMountKind::Workspace, 10, true)
		});

		Require(result.m_files.size() == 1 && result.m_files.front().m_virtualPath == "safe.txt",
			"Escaping and cyclic directory links must not contribute files");
		Require(HasDiagnostic(result.m_diagnostics, EAssetMountDiagnosticCode::PhysicalEscapeSkipped),
			"Physical escapes should produce a diagnostic");
		Require(HasDiagnostic(result.m_diagnostics, EAssetMountDiagnosticCode::DirectoryCycleSkipped),
			"Directory cycles should produce a diagnostic");
	}

	void TestWorkspaceWinsVirtualPathIndependentOfInputOrder()
	{
		TempDirectory engine("virtual-engine");
		TempDirectory workspace("virtual-workspace");
		WriteFile(engine.Path("Materials/shared.mat"));
		WriteFile(workspace.Path("Materials/shared.mat"));
		const AssetMountDiscoveryResult discovery = DiscoverAssetMountFiles({
			Mount(engine.Get(), EAssetMountKind::Engine, 100, false),
			Mount(workspace.Get(), EAssetMountKind::Workspace, 0, true)
		});

		const AssetMountCandidate engineCandidate = Candidate(
			FindFile(discovery, EAssetMountKind::Engine, "Materials/shared.mat"),
			"Materials/shared.mat",
			"{ENGINE-ID}");
		const AssetMountCandidate workspaceCandidate = Candidate(
			FindFile(discovery, EAssetMountKind::Workspace, "Materials/shared.mat"),
			"Materials/shared.mat",
			"{WORKSPACE-ID}");

		const AssetMountResolutionResult forward = ResolveAssetMountCandidates({
			engineCandidate,
			workspaceCandidate
		});
		const AssetMountResolutionResult reverse = ResolveAssetMountCandidates({
			workspaceCandidate,
			engineCandidate
		});
		const AssetMountCandidate* winner = forward.FindByVirtualPath("Materials/shared.mat");
		Require(winner && winner->m_mount.m_kind == EAssetMountKind::Workspace,
			"Workspace should override Engine for a colliding virtual path");
		Require(winner->m_fileId == "{WORKSPACE-ID}",
			"Virtual-path resolution should retain winner collision data");
		Require(reverse.FindByVirtualPath("Materials\\shared.mat") &&
			reverse.FindByVirtualPath("Materials\\shared.mat")->m_fileId == "{WORKSPACE-ID}",
			"Virtual lookup should normalize separators");
		Require(DiagnosticMessages(forward) == DiagnosticMessages(reverse),
			"Collision diagnostics must not depend on candidate input order");
		Require(HasDiagnostic(forward.GetDiagnostics(), EAssetMountDiagnosticCode::VirtualPathCollision),
			"Virtual-path collision data should be reported");
		const AssetMountDiagnostic& diagnostic = FindDiagnostic(
			forward.GetDiagnostics(),
			EAssetMountDiagnosticCode::VirtualPathCollision);
		Require(diagnostic.m_winnerKind == EAssetMountKind::Workspace &&
			diagnostic.m_conflictingKind == EAssetMountKind::Engine &&
			diagnostic.m_winnerFileId == "{WORKSPACE-ID}" &&
			diagnostic.m_conflictingFileId == "{ENGINE-ID}",
			"Virtual-path diagnostics should carry deterministic winner and loser provenance");
	}

	void TestWorkspaceWinsFileIdWhileUniqueVirtualPathsRemainAddressable()
	{
		TempDirectory engine("fileid-engine");
		TempDirectory workspace("fileid-workspace");
		WriteFile(engine.Path("Engine/asset.mat"));
		WriteFile(workspace.Path("Workspace/asset.mat"));
		const AssetMountDiscoveryResult discovery = DiscoverAssetMountFiles({
			Mount(engine.Get(), EAssetMountKind::Engine, 100, false),
			Mount(workspace.Get(), EAssetMountKind::Workspace, 0, true)
		});

		const AssetMountCandidate engineCandidate = Candidate(
			FindFile(discovery, EAssetMountKind::Engine, "Engine/asset.mat"),
			"Engine/asset.mat",
			"{SHARED-ID}");
		const AssetMountCandidate workspaceCandidate = Candidate(
			FindFile(discovery, EAssetMountKind::Workspace, "Workspace/asset.mat"),
			"Workspace/asset.mat",
			"{SHARED-ID}");
		const AssetMountResolutionResult result = ResolveAssetMountCandidates({
			engineCandidate,
			workspaceCandidate
		});

		Require(result.FindByFileId("{SHARED-ID}") &&
			result.FindByFileId("{SHARED-ID}")->m_mount.m_kind == EAssetMountKind::Workspace,
			"Workspace should override Engine for a colliding FileId");
		Require(result.FindByVirtualPath("Engine/asset.mat") != nullptr &&
			result.FindByVirtualPath("Workspace/asset.mat") != nullptr,
			"Different virtual paths should remain independently addressable");
		Require(HasDiagnostic(result.GetDiagnostics(), EAssetMountDiagnosticCode::FileIdCollision),
			"FileId collision data should be reported");
		const AssetMountDiagnostic& diagnostic = FindDiagnostic(
			result.GetDiagnostics(),
			EAssetMountDiagnosticCode::FileIdCollision);
		Require(diagnostic.m_key == "{SHARED-ID}" &&
			diagnostic.m_winnerKind == EAssetMountKind::Workspace &&
			diagnostic.m_conflictingKind == EAssetMountKind::Engine,
			"FileId diagnostics should identify the key and winning mount explicitly");
	}

	void TestPriorityBreaksTiesWithinTheSameMountKind()
	{
		TempDirectory first("priority-first");
		TempDirectory second("priority-second");
		WriteFile(first.Path("shared.asset"));
		WriteFile(second.Path("shared.asset"));
		const AssetMountDiscoveryResult discovery = DiscoverAssetMountFiles({
			Mount(first.Get(), EAssetMountKind::Engine, 1, false),
			Mount(second.Get(), EAssetMountKind::Engine, 20, false)
		});

		const AssetMountResolutionResult result = ResolveAssetMountCandidates({
			Candidate(FindFile(discovery, EAssetMountKind::Engine, "shared.asset"), "shared.asset", "{LOW}"),
			AssetMountCandidate{
				Mount(second.Get(), EAssetMountKind::Engine, 20, false),
				std::filesystem::canonical(second.Path("shared.asset")),
				"shared.asset",
				"{HIGH}"
			}
		});

		Require(result.FindByVirtualPath("shared.asset") &&
			result.FindByVirtualPath("shared.asset")->m_fileId == "{HIGH}",
			"Priority should break ties within the same mount kind");
	}

	void TestInvalidCandidatesAreRejected()
	{
		TempDirectory root("invalid-candidate");
		WriteFile(root.Path("asset.mat"));
		const AssetMountCandidate invalid{
			Mount(root.Get(), EAssetMountKind::Workspace, 10, true),
			std::filesystem::canonical(root.Path("asset.mat")),
			"../escape.mat",
			"{INVALID}"
		};

		const AssetMountResolutionResult result = ResolveAssetMountCandidates({ invalid });
		Require(result.GetCandidates().empty(), "Traversal candidates should not enter winner indexes");
		Require(HasDiagnostic(result.GetDiagnostics(), EAssetMountDiagnosticCode::InvalidCandidate),
			"Rejected candidates should produce an actionable diagnostic");
	}

	void TestEmptyWorkspaceDiscoversRealEngineContent(const std::filesystem::path& engineContent)
	{
		TempDirectory workspace("real-engine-content");
		const AssetMountDiscoveryResult discovery = DiscoverAssetMountFiles({
			Mount(engineContent, EAssetMountKind::Engine, 0, false),
			Mount(workspace.Get(), EAssetMountKind::Workspace, 100, true)
		});

		Require(!discovery.HasFatalErrors(),
			"An empty workspace should accept the repository Engine Content mount");
		Require(std::none_of(discovery.m_files.begin(), discovery.m_files.end(), [](const AssetMountDiscoveredFile& file)
			{
				return file.m_mount.m_kind == EAssetMountKind::Workspace;
			}), "The workspace fixture should remain empty during Engine Content discovery");
		const AssetMountDiscoveredFile& editorWorld = FindFile(
			discovery,
			EAssetMountKind::Engine,
			"Editor.world");
		Require(editorWorld.m_physicalPath ==
			std::filesystem::canonical(engineContent / "Editor.world"),
			"The default editor world should resolve from Engine Content");
		Require(!editorWorld.m_mount.m_bWritable,
			"The real Engine Content smoke must preserve read-only provenance");
		FindFile(discovery, EAssetMountKind::Engine, "Editor.world.asset");
		FindFile(discovery, EAssetMountKind::Engine, "Shaders/Constants.glsl");
	}
}

int main(int argc, char** argv)
{
	try
	{
		Require(argc == 2, "Expected the repository Engine Content path as the only argument");
		TestIdenticalRootsAreDeduplicatedToWorkspace();
		TestInvalidMountRootIsFatal();
		TestDiscoveryIsSortedByVirtualPath();
		TestOverlappingRootsAreRejectedInBothDirections();
		TestDiscoverySkipsPhysicalEscapesAndCycles();
		TestWorkspaceWinsVirtualPathIndependentOfInputOrder();
		TestWorkspaceWinsFileIdWhileUniqueVirtualPathsRemainAddressable();
		TestPriorityBreaksTiesWithinTheSameMountKind();
		TestInvalidCandidatesAreRejected();
		TestEmptyWorkspaceDiscoversRealEngineContent(argv[1]);
		std::cout << "Asset mount discovery tests passed.\n";
		return 0;
	}
	catch (const std::exception& error)
	{
		std::cerr << "Asset mount discovery tests failed: " << error.what() << '\n';
		return 1;
	}
}
