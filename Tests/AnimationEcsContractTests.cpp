#include "AssetRegistry/Animation/AnimationImporter.h"
#include "AssetRegistry/AssetRegistry.h"
#include "AssetRegistry/Model/ModelImporter.h"
#include "Components/AnimatorComponent.h"
#include "Components/MeshRendererComponent.h"
#include "ECS/AnimationECS.h"
#include "ECS/StaticMeshRendererECS.h"
#include "ECS/TransformECS.h"
#include "Engine/GameObject.h"
#include "Engine/World.h"
#include "Support/TaskTestApp.h"
#include "Support/TempDirectory.h"

#include <cmath>
#include <chrono>
#include <iostream>
#include <stdexcept>
#include <string_view>

using namespace Sailor;

namespace Sailor
{
	class AnimationImporterTestAccess
	{
	public:
		static AnimationPtr Create(AnimationImporter& importer)
		{
			auto clip = AnimationPtr::Make(importer.m_allocator, FileId::CreateNewFileId());
			importer.m_loadedAnimations.Insert(clip->GetFileId(), clip);
			return clip;
		}

		static void SetPending(AnimationImporter& importer, AnimationPtr clip, Tasks::TaskPtr<AnimationPtr> task)
		{
			importer.m_promises.At_Lock(clip->GetFileId()) = std::move(task);
			importer.m_promises.Unlock(clip->GetFileId());
		}
	};
}

namespace
{
	void Require(bool condition, std::string_view message)
	{
		if (!condition) throw std::runtime_error(std::string(message));
	}

	class AnimationWorld final : public World
	{
	public:
		AnimationWorld() : World("Animation pose contracts", 0, CreateSystems()) {}
	private:
		static TVector<ECS::TBaseSystemPtr> CreateSystems()
		{
			TVector<ECS::TBaseSystemPtr> systems;
			systems.Add(TUniquePtr<TransformECS>::Make());
			systems.Add(TUniquePtr<AnimationECS>::Make());
			systems.Add(TUniquePtr<StaticMeshRendererECS>::Make());
			return systems;
		}
	};

	class BindPoseModel final : public Model
	{
	public:
		BindPoseModel() : Model(FileId::Invalid) {}
		bool IsReady() const override { return true; }
	};

	void SetClip(AnimationPtr clip, float start, float end, float duration = 1.0f, uint32_t bones = 1)
	{
		clip->m_numBones = bones;
		clip->m_numFrames = 2;
		clip->m_duration = duration;
		clip->m_fps = 1.0f / duration;
		clip->m_skeletonSignature = bones;
		clip->m_frames.Resize(2 * bones);
		clip->m_restPose.Resize(bones);
		clip->m_parentBoneIndices.Resize(bones);
		for (uint32_t bone = 0; bone < bones; ++bone)
		{
			Math::Transform pose;
			pose.m_position.x = start;
			clip->m_restPose[bone] = clip->m_frames[bone] = pose;
			pose.m_position.x = end;
			clip->m_frames[bones + bone] = pose;
			clip->m_parentBoneIndices[bone] = -1;
		}
		++clip->m_revision;
	}

	class Fixture
	{
	public:
		Fixture() : m_registry(Workspace::ResolveWorkspaceContext(m_directory.Get()).m_context, nullptr),
			m_handler(&m_registry), m_importer(m_app.AddAnimationImporter(m_handler))
		{
			m_app.GetScheduler().Initialize();
			m_controller = AnimationControllerPtr::Make(m_world.GetAllocator(), FileId::CreateNewFileId());
			m_set = AnimationSetPtr::Make(m_world.GetAllocator(), FileId::CreateNewFileId());
			m_owner = m_world.Instantiate("Animated owner");
			m_animator = m_owner->AddComponent<AnimatorComponent>();
			m_animator->Stop();
		}

		~Fixture()
		{
			CompleteLoads();
			m_world.Clear();
			for (auto& model : m_models) model.DestroyObject(m_world.GetAllocator());
			m_controller.DestroyObject(m_world.GetAllocator());
			m_set.DestroyObject(m_world.GetAllocator());
		}

		AnimationPtr Clip(float start, float end, float duration = 1.0f, uint32_t bones = 1)
		{
			auto clip = AnimationImporterTestAccess::Create(m_importer);
			SetClip(clip, start, end, duration, bones);
			return clip;
		}

		TObjectPtr<BindPoseModel> MakeModel(float translation)
		{
			auto model = TObjectPtr<BindPoseModel>::Make(m_world.GetAllocator());
			model->GetInverseBind().Add(glm::mat4(1.0f));
			model->GetInverseBind()[0][3].x = translation;
			model->Flush();
			m_models.Add(model);
			return model;
		}

		AnimationPtr PendingClip(float position, uint32_t bones = 1, bool bSucceeds = true)
		{
			auto clip = AnimationImporterTestAccess::Create(m_importer);
			auto gate = Tasks::CreateTask("Release animation fixture"_h, [] {}, EThreadType::Main);
			auto load = Tasks::CreateTaskWithResult<AnimationPtr>("Prepare animation fixture"_h,
				[clip, position, bones, bSucceeds]
				{
					if (!bSucceeds) return AnimationPtr{};
					SetClip(clip, position, position, 1.0f, bones);
					return clip;
				});
			load->Join(gate);
			AnimationImporterTestAccess::SetPending(m_importer, clip, load);
			m_gates.Add(gate);
			m_loads.Add(load);
			load->Run();
			return clip;
		}

		void CompleteLoads()
		{
			for (auto& gate : m_gates) if (gate) gate->Run();
			m_gates.Clear();
			m_app.GetScheduler().ProcessTasksOnMainThread();
			for (auto& load : m_loads) load->Wait();
			m_loads.Clear();
		}

		void CompleteLoad(size_t index)
		{
			m_gates[index]->Run();
			m_gates[index].Clear();
			m_app.GetScheduler().ProcessTasksOnMainThread();
			m_loads[index]->Wait();
		}

		void Configure(AnimationPtr idle, AnimationPtr moving, float blend = 0.0f)
		{
			m_asset.GetStates() = { { 10, "Idle", "Idle", 1.0f, false }, { 20, "Moving", "Moving", 1.0f, false } };
			m_asset.SetDefaultStateId(10);
			m_asset.GetParameters() = { { 1, "Move", EAnimationParameterType::Bool } };
			AnimationTransitionDefinition transition;
			transition.m_id = 100;
			transition.m_fromStateId = 10;
			transition.m_toStateId = 20;
			transition.m_duration = blend;
			AnimationTransitionCondition condition;
			condition.m_parameterId = 1;
			condition.m_boolValue = true;
			transition.m_conditions.Add(condition);
			m_asset.GetTransitions() = { transition };
			Require(m_controller->Initialize(m_asset), "fixture controller must initialize");
			SetClips(idle, moving);
			m_animator->SetController(m_controller);
			m_animator->SetAnimationSet(m_set);
		}

		void SetClips(AnimationPtr idle, AnimationPtr moving)
		{
			AnimationSetAsset asset;
			asset.GetEntries() = { { "Idle", idle->GetFileId() }, { "Moving", moving->GetFileId() } };
			Require(m_set->Initialize(asset), "fixture animation set must initialize");
		}

		RHI::RHISceneViewPtr Tick(float delta = 0.0f)
		{
			auto* animations = m_world.GetECS<AnimationECS>();
			animations->Tick(delta);
			auto scene = RHI::RHISceneViewPtr::Make();
			animations->FillAnimationData(scene);
			return scene;
		}

		Tests::TaskTestApp m_app;
		Tests::TempDirectory m_directory{ "animation-pose" };
		AssetRegistry m_registry;
		AnimationAssetInfoHandler m_handler;
		AnimationImporter& m_importer;
		AnimationWorld m_world;
		AnimationControllerPtr m_controller;
		AnimationSetPtr m_set;
		AnimationControllerAsset m_asset;
		TVector<TObjectPtr<BindPoseModel>> m_models;
		GameObjectPtr m_owner;
		TObjectPtr<AnimatorComponent> m_animator;
		TVector<Tasks::TaskPtr<>> m_gates;
		TVector<Tasks::TaskPtr<AnimationPtr>> m_loads;
	};

	float Position(const RHI::RHISceneViewPtr& scene, size_t index = 0)
	{
		Require(scene->m_cpuBoneMatrices && index < scene->m_cpuBoneMatrices->Num(), "expected a published bone");
		return (*scene->m_cpuBoneMatrices)[index][3].x;
	}

	void TestPausedControllerAndBlend()
	{
		for (const float blend : { 0.0f, 1.0f })
		{
			Fixture fixture;
			fixture.Configure(fixture.Clip(2, 2), fixture.Clip(10, 10), blend);
			const auto initial = fixture.Tick();
			for (uint32_t tick = 0; tick < 200; ++tick)
			{
				const auto paused = fixture.Tick(1.0f / 60.0f);
				Require(paused->m_cpuBoneMatrices == initial->m_cpuBoneMatrices &&
					paused->m_animationRevision == initial->m_animationRevision,
					"paused controllers must reuse the published palette and shadow revision");
			}
			Require(fixture.m_animator->SetBool("Move"_h, true), "the controller parameter must exist");
			const auto transition = fixture.Tick();
			if (blend == 0.0f)
			{
				Require(Position(transition) == 10 && transition->m_animationRevision == initial->m_animationRevision + 1,
					"a zero-duration parameter transition must publish while paused");
			}
			else
			{
				Require(fixture.m_animator->GetData().GetControllerInstance().IsTransitioning() &&
					transition->m_cpuBoneMatrices == initial->m_cpuBoneMatrices,
					"a paused blend must start without advancing its weight or republishing the source pose");
				fixture.m_animator->Play();
				const auto halfway = fixture.Tick(0.5f);
				Require(Position(halfway) == 6 && halfway->m_animationRevision == initial->m_animationRevision + 1,
					"resuming must publish the interpolated local pose");
				fixture.m_animator->Stop();
				Require(fixture.Tick(1.0f)->m_cpuBoneMatrices == halfway->m_cpuBoneMatrices,
					"pausing a running blend must retain its weight and palette");
			}
			Require(Position(initial) == 2, "retained frame matrices must remain immutable");
		}
	}

	void TestReloadUsesRemappedClipDuration()
	{
		Fixture fixture;
		fixture.Configure(fixture.Clip(2, 2, 10), fixture.Clip(10, 10, 1));
		fixture.m_animator->Play();
		fixture.Tick(2.0f);
		fixture.m_animator->Stop();
		std::swap(fixture.m_asset.GetStates()[0], fixture.m_asset.GetStates()[1]);
		auto& transition = fixture.m_asset.GetTransitions()[0];
		transition.m_bHasExitTime = true;
		transition.m_exitTime = 0.5f;
		transition.m_conditions.Clear();
		Require(fixture.m_controller->Initialize(fixture.m_asset), "reordered controller must initialize");
		const auto reloaded = fixture.Tick();
		Require(Position(reloaded) == 2 && fixture.m_animator->GetData().GetControllerInstance().GetActiveStateIndex() == 1,
			"hot reload must test exit time against the remapped ten-second clip, not the old index's one-second clip");
		fixture.m_animator->Play();
		Require(Position(fixture.Tick(3.0f)) == 10, "the transition must occur at the preserved active state's actual exit time");
	}

	void TestPendingClipsKeepPoseAndRefreshBindMatrices()
	{
		Fixture fixture;
		auto idle = fixture.Clip(2, 2);
		fixture.Configure(idle, fixture.Clip(10, 10));
		auto model = fixture.MakeModel(0);
		fixture.m_owner->AddComponent<MeshRendererComponent>()->SetModel(model);
		const auto initial = fixture.Tick();
		auto pending = fixture.PendingClip(20);
		fixture.SetClips(pending, pending);
		for (uint32_t tick = 0; tick < 200; ++tick)
		{
			Require(fixture.Tick(0.1f)->m_cpuBoneMatrices == initial->m_cpuBoneMatrices && pending->m_numBones == 0,
				"unfinished importer results must not replace or resample the last complete pose");
		}
		model->GetInverseBind()[0][3].x = 3;
		model->Flush();
		const auto rebound = fixture.Tick();
		Require(Position(rebound) == 5 && rebound->m_animationRevision == initial->m_animationRevision + 1,
			"inverse-bind edits must reskin the completed pose even while replacement clips are pending");
		model->Flush();
		Require(fixture.Tick()->m_cpuBoneMatrices == rebound->m_cpuBoneMatrices,
			"publishing unchanged inverse-bind matrices must not invalidate the pose");
		fixture.CompleteLoads();
		const auto loaded = fixture.Tick();
		Require(Position(loaded) == 23 && loaded->m_animationRevision == rebound->m_animationRevision + 1 &&
			Position(initial) == 2, "the completed clip set must replace the preserved pose exactly once");
	}

	void TestHotReloadPreservesPausedTimeAndCompactsOnce()
	{
		Fixture fixture;
		auto idle = fixture.Clip(2, 6);
		fixture.Configure(idle, fixture.Clip(10, 10));
		auto neighbor = fixture.m_world.Instantiate("Neighbor")->AddComponent<AnimatorComponent>();
		neighbor->SetAnimation(fixture.Clip(8, 8));
		neighbor->Stop();
		fixture.m_animator->Play();
		const auto initial = fixture.Tick(0.25f);
		fixture.m_animator->Stop();
		Require(Position(initial) == 3 && neighbor->GetSkeletonOffset() == 1, "fixture must start between keyframes");
		SetClip(idle, 10, 14);
		const auto reloaded = fixture.Tick();
		Require(Position(reloaded) == 11 && Position(initial) == 3 &&
			reloaded->m_animationRevision == initial->m_animationRevision + 1 && neighbor->GetSkeletonOffset() == 1,
			"same-sized clip reload must preserve the paused time and neighboring range");
		SetClip(idle, 20, 24, 1, 3);
		const auto resized = fixture.Tick();
		Require(resized->m_cpuBoneMatrices->Num() == 4 && neighbor->GetSkeletonOffset() == 3 &&
			Position(resized, 2) == 21 && Position(resized, 3) == 8 &&
			resized->m_animationRevision == reloaded->m_animationRevision + 1,
			"a changed skeleton must publish its pose and compact neighboring offsets in one revision");
		Require(fixture.Tick()->m_cpuBoneMatrices == resized->m_cpuBoneMatrices,
			"a completed skeleton refresh must not repeat its layout publication");
	}

	void TestColdPendingAndSupersededClipSets()
	{
		Fixture fixture;
		auto first = fixture.PendingClip(20, 2);
		fixture.Configure(first, first);
		const auto empty = fixture.Tick();
		Require(empty->m_cpuBoneMatrices->IsEmpty(), "an unfinished initial clip must not publish partial bones");
		auto latest = fixture.PendingClip(30, 3);
		fixture.SetClips(latest, latest);
		fixture.Tick();
		fixture.CompleteLoad(0);
		for (uint32_t tick = 0; tick < 200; ++tick)
		{
			Require(fixture.Tick()->m_cpuBoneMatrices == empty->m_cpuBoneMatrices,
				"completion of a superseded request must not publish while the current request is pending");
		}
		fixture.CompleteLoad(1);
		const auto completed = fixture.Tick();
		Require(completed->m_cpuBoneMatrices->Num() == 3 && Position(completed, 2) == 30 &&
			completed->m_animationRevision == empty->m_animationRevision + 1,
			"only the current completed clip set may establish the initial palette");
		Require(fixture.Tick()->m_cpuBoneMatrices == completed->m_cpuBoneMatrices,
			"the completed request must be consumed only once");
	}

	void TestRemovalWhileClipsArePending()
	{
		Fixture fixture;
		fixture.Configure(fixture.Clip(2, 2), fixture.Clip(10, 10));
		auto neighbor = fixture.m_world.Instantiate("Neighbor")->AddComponent<AnimatorComponent>();
		neighbor->SetAnimation(fixture.Clip(8, 8));
		neighbor->Stop();
		const auto initial = fixture.Tick();
		auto pending = fixture.PendingClip(20, 3);
		fixture.SetClips(pending, pending);
		fixture.Tick();
		const size_t removedSlot = fixture.m_world.GetECS<AnimationECS>()->GetComponentIndex(&fixture.m_animator->GetData());
		fixture.m_world.DestroyImmediate(fixture.m_owner);
		auto replacement = fixture.m_world.Instantiate("Replacement")->AddComponent<AnimatorComponent>();
		replacement->SetAnimation(fixture.Clip(40, 40));
		replacement->Stop();
		Require(fixture.m_world.GetECS<AnimationECS>()->GetComponentIndex(&replacement->GetData()) == removedSlot,
			"the fixture must reuse the removed component's slot");
		const auto replaced = fixture.Tick();
		Require(Position(replaced) == 40 && Position(replaced, 1) == 8 && Position(initial) == 2 &&
			replaced->m_animationRevision == initial->m_animationRevision + 1,
			"removal and slot reuse must publish one complete palette without changing a retained frame");
		fixture.CompleteLoads();
		Require(fixture.Tick()->m_cpuBoneMatrices == replaced->m_cpuBoneMatrices,
			"a removed animator's completed load must not affect the reused slot");
	}

	void TestFailedAndMissingClipsUseFallback()
	{
		Fixture fixture;
		auto idle = fixture.Clip(2, 2);
		fixture.m_animator->SetAnimation(fixture.Clip(9, 9));
		fixture.Configure(idle, fixture.Clip(10, 10));
		const auto initial = fixture.Tick();
		auto failed = fixture.PendingClip(20, 1, false);
		fixture.SetClips(failed, failed);
		Require(fixture.Tick()->m_cpuBoneMatrices == initial->m_cpuBoneMatrices,
			"the last valid pose must remain visible until the failed request completes");
		fixture.CompleteLoads();
		const auto fallback = fixture.Tick();
		Require(Position(fallback) == 9 && fallback->m_animationRevision == initial->m_animationRevision + 1,
			"a completed failed request must use the authored fallback instead of partial clip data");
		Require(fixture.Tick()->m_cpuBoneMatrices == fallback->m_cpuBoneMatrices,
			"a failed request must not be retried and republished every tick");
		AnimationSetAsset missing;
		missing.GetEntries() = { { "Idle", idle->GetFileId() } };
		Require(fixture.m_set->Initialize(missing), "the fixture may intentionally leave a controller slot unmapped");
		fixture.Tick();
		Require(fixture.m_animator->SetBool("Move"_h, true), "the transition parameter must remain available");
		const auto rest = fixture.Tick();
		Require(Position(rest) == 2, "a missing destination must use the prepared controller skeleton's rest pose");
		fixture.SetClips(idle, fixture.Clip(30, 30));
		const auto repaired = fixture.Tick();
		Require(Position(repaired) == 30 && fixture.Tick()->m_cpuBoneMatrices == repaired->m_cpuBoneMatrices,
			"an explicit set revision must repair the missing clip without resetting its active state");
	}

	void TestFallbackEditsAfterControllerFailure()
	{
		for (const uint32_t bones : { 3u, 0u })
		{
			Fixture fixture;
			fixture.m_animator->SetAnimation(fixture.Clip(9, 9));
			auto failed = fixture.PendingClip(20, 1, false);
			fixture.Configure(failed, failed);
			auto neighbor = fixture.m_world.Instantiate("Neighbor")->AddComponent<AnimatorComponent>();
			neighbor->SetAnimation(fixture.Clip(8, 8));
			neighbor->Stop();
			fixture.CompleteLoads();
			const auto initial = fixture.Tick();
			Require(Position(initial) == 9 && neighbor->GetSkeletonOffset() == 1,
				"failed controller clips must start with the fallback's bone layout");
			fixture.m_animator->SetAnimation(bones ? fixture.Clip(7, 7, 1, bones) : AnimationPtr{});
			const auto changed = fixture.Tick();
			Require(fixture.m_animator->GetData().GetBonesCount() == bones &&
				changed->m_cpuBoneMatrices->Num() == bones + 1 && neighbor->GetSkeletonOffset() == bones &&
				Position(changed, bones) == 8 && (!bones || Position(changed, bones - 1) == 7),
				"empty controller clip slots must not block resizing or removing the active fallback skeleton");
			Require(changed->m_animationRevision == initial->m_animationRevision + 1 && Position(initial) == 9,
				"a fallback layout edit must publish once and preserve retained frame matrices");
			for (uint32_t tick = 0; tick < 200; ++tick)
			{
				Require(fixture.Tick(0.1f)->m_cpuBoneMatrices == changed->m_cpuBoneMatrices,
					"a completed fallback edit must not repeat palette publication");
			}
		}
	}

	void TestFallbackEditWhileControllerLoads()
	{
		Fixture fixture;
		fixture.m_animator->SetAnimation(fixture.Clip(4, 4, 1, 2));
		const auto initial = fixture.Tick();
		auto pending = fixture.PendingClip(20, 4);
		fixture.Configure(pending, pending);
		fixture.Tick();
		fixture.m_animator->SetAnimation(fixture.Clip(7, 7, 1, 3));
		const auto loading = fixture.Tick();
		Require(loading->m_cpuBoneMatrices == initial->m_cpuBoneMatrices && fixture.m_animator->GetData().GetBonesCount() == 2,
			"editing the fallback must not change the completed layout while controller clips are pending");
		fixture.CompleteLoads();
		const auto loaded = fixture.Tick();
		Require(loaded->m_cpuBoneMatrices->Num() == 4 && Position(loaded, 3) == 20,
			"the ready controller must publish its own skeleton after a pending fallback edit");
	}

	void TestModelReplacementReskinsPausedPose()
	{
		Fixture fixture;
		fixture.m_animator->SetAnimation(fixture.Clip(2, 2));
		auto mesh = fixture.m_owner->AddComponent<MeshRendererComponent>();
		auto first = fixture.MakeModel(3);
		auto second = fixture.MakeModel(5);
		Require(first->GetSkeletonRevision() == second->GetSkeletonRevision(),
			"the fixture must distinguish model identity from its local revision");
		mesh->SetModel(first);
		const auto initial = fixture.Tick();
		mesh->SetModel(second);
		const auto replaced = fixture.Tick();
		Require(Position(initial) == 5 && Position(replaced) == 7 &&
			replaced->m_animationRevision == initial->m_animationRevision + 1,
			"model replacement must apply the new inverse-bind matrices to the unchanged pose");
		Require(fixture.Tick()->m_cpuBoneMatrices == replaced->m_cpuBoneMatrices,
			"the replaced model must not trigger repeated skinning publication");
		mesh->SetModel({});
		Require(Position(fixture.Tick()) == 2, "removing the model must restore identity inverse-bind matrices");
	}

	void TestSingleFrameAndClampedPoseReuse()
	{
		Fixture fixture;
		auto single = fixture.Clip(4, 4);
		single->m_numFrames = 1;
		single->m_frames.Resize(1);
		fixture.m_animator->SetAnimation(single);
		const auto initial = fixture.Tick();
		for (const auto mode : { EAnimationPlayMode::Repeat, EAnimationPlayMode::Once, EAnimationPlayMode::PingPong })
		{
			fixture.m_animator->SetPlayMode(mode);
			fixture.m_animator->Play();
			for (uint32_t tick = 0; tick < 200; ++tick)
			{
				Require(fixture.Tick(0.1f)->m_cpuBoneMatrices == initial->m_cpuBoneMatrices,
					"single-frame clips must reuse their pose in every playback mode");
			}
		}
		fixture.m_animator->SetAnimation(fixture.Clip(1, 5));
		fixture.m_animator->SetPlayMode(EAnimationPlayMode::Once);
		fixture.m_animator->GetData().m_bForward = true;
		fixture.m_animator->Play();
		const auto clamped = fixture.Tick(4.0f);
		Require(Position(clamped) == 5 && !fixture.m_animator->GetData().m_bIsPlaying,
			"Once playback must stop at the final sampled frame");
		for (uint32_t tick = 0; tick < 200; ++tick)
		{
			Require(fixture.Tick(1.0f)->m_cpuBoneMatrices == clamped->m_cpuBoneMatrices,
				"a clamped final frame must not resample or republish");
		}
	}

	void BenchmarkPoseReuse()
	{
		constexpr uint32_t ticks = 500;
		for (const uint32_t bones : { 1u, 4096u })
		{
			Fixture fixture;
			fixture.m_animator->SetAnimation(fixture.Clip(0, 1, 1, bones));
			auto scene = fixture.Tick();
			for (const bool bPlaying : { false, true })
			{
				if (bPlaying) fixture.m_animator->Play();
				else fixture.m_animator->Stop();
				const auto previousRevision = scene->m_animationRevision;
				const auto started = std::chrono::steady_clock::now();
				for (uint32_t tick = 0; tick < ticks; ++tick) scene = fixture.Tick(1.0f / 240.0f);
				const double milliseconds = std::chrono::duration<double, std::milli>(
					std::chrono::steady_clock::now() - started).count();
				Require(scene->m_animationRevision == previousRevision + (bPlaying ? ticks : 0),
					"the benchmark must reuse paused poses and actually publish every advancing pose");
				std::cout << "Pose benchmark: bones=" << bones << " playing=" << bPlaying
					<< " ticks=" << ticks << " milliseconds=" << milliseconds << '\n';
			}
		}
	}
}

int main(int argc, char** argv)
{
	bool bPassed = true;
	const std::pair<const char*, void(*)()> tests[] = {
		{ "PausedControllerAndBlend", TestPausedControllerAndBlend },
		{ "ReloadUsesRemappedClipDuration", TestReloadUsesRemappedClipDuration },
		{ "PendingClipsKeepPoseAndRefreshBindMatrices", TestPendingClipsKeepPoseAndRefreshBindMatrices },
		{ "HotReloadPreservesPausedTimeAndCompactsOnce", TestHotReloadPreservesPausedTimeAndCompactsOnce },
		{ "ColdPendingAndSupersededClipSets", TestColdPendingAndSupersededClipSets },
		{ "RemovalWhileClipsArePending", TestRemovalWhileClipsArePending },
		{ "FailedAndMissingClipsUseFallback", TestFailedAndMissingClipsUseFallback },
		{ "FallbackEditsAfterControllerFailure", TestFallbackEditsAfterControllerFailure },
		{ "FallbackEditWhileControllerLoads", TestFallbackEditWhileControllerLoads },
		{ "ModelReplacementReskinsPausedPose", TestModelReplacementReskinsPausedPose },
		{ "SingleFrameAndClampedPoseReuse", TestSingleFrameAndClampedPoseReuse }
	};
	for (const auto& [name, test] : tests)
	{
		try
		{
			test();
			std::cout << "[PASS] " << name << '\n';
		}
		catch (const std::exception& error)
		{
			std::cerr << "[FAIL] " << name << ": " << error.what() << '\n';
			bPassed = false;
		}
	}
	if (bPassed && argc == 2 && std::string_view(argv[1]) == "--benchmark")
	{
		try { BenchmarkPoseReuse(); }
		catch (const std::exception& error)
		{
			std::cerr << "[FAIL] Pose benchmark: " << error.what() << '\n';
			bPassed = false;
		}
	}
	return bPassed ? 0 : 1;
}
