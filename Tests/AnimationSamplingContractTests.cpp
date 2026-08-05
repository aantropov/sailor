#include "AssetRegistry/Animation/AnimationClipSampler.h"
#include "AssetRegistry/Animation/AnimationController.h"
#include "AssetRegistry/Animation/AnimationPose.h"
#include "Memory/ObjectAllocator.hpp"

#include <cmath>
#include <functional>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>
#include <utility>

using namespace Sailor;

namespace
{
	void Require(bool condition, const std::string& message)
	{
		if (!condition)
		{
			throw std::runtime_error(message);
		}
	}

	bool NearlyEqual(float lhs, float rhs, float epsilon = 0.0001f)
	{
		return std::abs(lhs - rhs) <= epsilon;
	}

	void TestTimestampValidationAndNonUniformSpan()
	{
		Require(!AnimationClipSampler::ValidateTimestamps({}),
			"empty timestamp arrays must be rejected");
		Require(!AnimationClipSampler::ValidateTimestamps({ -1.0f, 0.0f }),
			"negative glTF timestamps must be rejected");
		Require(!AnimationClipSampler::ValidateTimestamps({ 0.0f, 0.0f }),
			"duplicate glTF timestamps must be rejected");
		Require(!AnimationClipSampler::ValidateTimestamps({
			0.0f,
			(std::numeric_limits<float>::quiet_NaN)()
		}), "non-finite glTF timestamps must be rejected");

		AnimationKeyframeSpan span;
		Require(AnimationClipSampler::ResolveKeyframeSpan(
			{ 0.0f, 0.25f, 2.0f },
			1.125f,
			span), "valid non-uniform timestamps must resolve");
		Require(span.m_first == 1 && span.m_second == 2,
			"sampling must select keyframes by time rather than array index");
		Require(NearlyEqual(span.m_alpha, 0.5f) && NearlyEqual(span.m_duration, 1.75f),
			"non-uniform keyframe interpolation must use the selected time interval");
	}

	void TestLinearAndStepVectorSampling()
	{
		const TVector<float> timestamps{ 0.0f, 2.0f };
		const TVector<glm::vec4> values{
			glm::vec4(0.0f),
			glm::vec4(10.0f, 4.0f, -2.0f, 0.0f)
		};

		glm::vec4 sampled;
		Require(AnimationClipSampler::SampleVector(
			timestamps,
			values,
			EAnimationInterpolation::Linear,
			0.5f,
			sampled), "LINEAR vector channels must sample");
		Require(NearlyEqual(sampled.x, 2.5f) &&
			NearlyEqual(sampled.y, 1.0f) &&
			NearlyEqual(sampled.z, -0.5f),
			"LINEAR vector channels must interpolate using glTF timestamps");

		Require(AnimationClipSampler::SampleVector(
			timestamps,
			values,
			EAnimationInterpolation::Step,
			1.999f,
			sampled), "STEP vector channels must sample");
		Require(NearlyEqual(sampled.x, 0.0f),
			"STEP vector channels must retain the preceding keyframe");

		Require(AnimationClipSampler::SampleVector(
			timestamps,
			values,
			EAnimationInterpolation::Step,
			2.0f,
			sampled) && NearlyEqual(sampled.x, 10.0f),
			"STEP vector channels must reach the final keyframe at its timestamp");
	}

	void TestCubicSplineSamplingScalesTangentsByInterval()
	{
		const TVector<float> timestamps{ 0.0f, 2.0f };
		const TVector<glm::vec4> values{
			glm::vec4(0.0f),
			glm::vec4(0.0f),
			glm::vec4(2.0f, 0.0f, 0.0f, 0.0f),
			glm::vec4(2.0f, 0.0f, 0.0f, 0.0f),
			glm::vec4(4.0f, 0.0f, 0.0f, 0.0f),
			glm::vec4(0.0f)
		};

		glm::vec4 sampled;
		Require(AnimationClipSampler::SampleVector(
			timestamps,
			values,
			EAnimationInterpolation::CubicSpline,
			1.0f,
			sampled), "CUBICSPLINE vector channels must sample");
		Require(NearlyEqual(sampled.x, 2.0f),
			"CUBICSPLINE tangents must be scaled by the keyframe interval");
	}

	void TestRotationSamplingProducesNormalizedShortestPath()
	{
		const float halfSqrt = std::sqrt(0.5f);
		glm::quat sampled;
		Require(AnimationClipSampler::SampleRotation(
			{ 0.0f, 1.0f },
			{
				glm::vec4(0.0f, 0.0f, 0.0f, 1.0f),
				glm::vec4(0.0f, 0.0f, 0.0f, -1.0f)
			},
			EAnimationInterpolation::Linear,
			0.5f,
			sampled), "LINEAR rotation channels must sample");
		Require(NearlyEqual(std::abs(sampled.w), 1.0f),
			"LINEAR quaternion sampling must use the shortest normalized path");

		Require(AnimationClipSampler::SampleRotation(
			{ 0.0f, 1.0f },
			{
				glm::vec4(0.0f),
				glm::vec4(0.0f, 0.0f, 0.0f, 1.0f),
				glm::vec4(0.0f),
				glm::vec4(0.0f),
				glm::vec4(0.0f, 0.0f, 1.0f, 0.0f),
				glm::vec4(0.0f)
			},
			EAnimationInterpolation::CubicSpline,
			0.5f,
			sampled), "CUBICSPLINE rotation channels must sample");
		Require(NearlyEqual(sampled.z, halfSqrt) &&
			NearlyEqual(sampled.w, halfSqrt) &&
			NearlyEqual(glm::length(sampled), 1.0f),
			"CUBICSPLINE quaternion output must be normalized after Hermite interpolation");
	}

	AnimationControllerAsset MakeControllerAsset()
	{
		AnimationControllerAsset asset;
		asset.SetDefaultStateId(100);
		asset.GetParameters() = {
			AnimationParameterDefinition{
				.m_id = 1,
				.m_name = "Speed",
				.m_type = EAnimationParameterType::Float
			},
			AnimationParameterDefinition{
				.m_id = 2,
				.m_name = "Grounded",
				.m_type = EAnimationParameterType::Bool,
				.m_defaultBool = true
			},
			AnimationParameterDefinition{
				.m_id = 3,
				.m_name = "Mode",
				.m_type = EAnimationParameterType::Int
			},
			AnimationParameterDefinition{
				.m_id = 4,
				.m_name = "Jump",
				.m_type = EAnimationParameterType::Trigger
			}
		};
		asset.GetStates() = {
			AnimationStateDefinition{
				.m_id = 100,
				.m_name = "Idle",
				.m_clipSlot = "Idle",
				.m_editorX = 10.0f,
				.m_editorY = 20.0f
			},
			AnimationStateDefinition{
				.m_id = 200,
				.m_name = "Walk",
				.m_clipSlot = "Walk"
			},
			AnimationStateDefinition{
				.m_id = 300,
				.m_name = "Jump",
				.m_clipSlot = "Jump",
				.m_bLoop = false
			}
		};

		AnimationTransitionDefinition walk;
		walk.m_id = 1000;
		walk.m_fromStateId = 100;
		walk.m_toStateId = 200;
		walk.m_priority = 1;
		walk.m_duration = 0.2f;
		walk.m_conditions = {
			AnimationTransitionCondition{
				.m_parameterId = 1,
				.m_operation = EAnimationConditionOperation::Greater,
				.m_floatValue = 0.5f
			},
			AnimationTransitionCondition{
				.m_parameterId = 2,
				.m_operation = EAnimationConditionOperation::Equal,
				.m_boolValue = true
			},
			AnimationTransitionCondition{
				.m_parameterId = 3,
				.m_operation = EAnimationConditionOperation::Equal,
				.m_intValue = 2
			}
		};

		AnimationTransitionDefinition jump;
		jump.m_id = 1001;
		jump.m_fromStateId = 100;
		jump.m_toStateId = 300;
		jump.m_priority = 10;
		jump.m_duration = 0.2f;
		jump.m_conditions = {
			AnimationTransitionCondition{
				.m_parameterId = 4,
				.m_operation = EAnimationConditionOperation::IsSet
			}
		};

		AnimationTransitionDefinition returnToIdle;
		returnToIdle.m_id = 1002;
		returnToIdle.m_fromStateId = 300;
		returnToIdle.m_toStateId = 100;
		returnToIdle.m_priority = 0;
		returnToIdle.m_duration = 0.0f;
		returnToIdle.m_bHasExitTime = true;
		returnToIdle.m_exitTime = 0.5f;

		asset.GetTransitions() = {
			std::move(walk),
			std::move(jump),
			std::move(returnToIdle)
		};
		return asset;
	}

	void TestAnimationControllerSerializationAndValidation()
	{
		AnimationControllerAsset source = MakeControllerAsset();
		AnimationControllerAsset roundTrip;
		roundTrip.Deserialize(source.Serialize());

		Require(roundTrip.GetDefaultStateId() == 100 &&
			roundTrip.GetParameters().Num() == 4 &&
			roundTrip.GetStates().Num() == 3 &&
			roundTrip.GetTransitions().Num() == 3,
			"animation controller YAML must retain graph structure and stable ids");
		Require(NearlyEqual(roundTrip.GetStates()[0].m_editorX, 10.0f) &&
			NearlyEqual(roundTrip.GetStates()[0].m_editorY, 20.0f),
			"animation controller YAML must retain source-controlled editor layout");

		YAML::Node invalidParameterType = source.Serialize();
		invalidParameterType["parameters"][0]["type"] = "Vector";
		AnimationControllerAsset invalidParameterAsset;
		invalidParameterAsset.Deserialize(invalidParameterType);
		Require(invalidParameterAsset.GetParameters()[0].m_type ==
			EAnimationParameterType::Invalid,
			"unknown animation parameter types must not become Float");

		YAML::Node invalidConditionOperation = source.Serialize();
		invalidConditionOperation["transitions"][0]["conditions"][0]["operation"] = "Approximate";
		AnimationControllerAsset invalidOperationAsset;
		invalidOperationAsset.Deserialize(invalidConditionOperation);
		Require(invalidOperationAsset.GetTransitions()[0].m_conditions[0].m_operation ==
			EAnimationConditionOperation::Invalid,
			"unknown animation condition operations must not become Equal");

		auto allocator = Memory::ObjectAllocatorPtr::Make(
			Memory::EAllocationPolicy::SharedMemory_MultiThreaded);
		auto controller = AnimationControllerPtr::Make(allocator, FileId{});
		TVector<std::string> errors;
		Require(!controller->Initialize(invalidParameterAsset, &errors) && !errors.IsEmpty(),
			"unknown animation parameter types must fail graph validation");
		Require(!controller->Initialize(invalidOperationAsset, &errors) && !errors.IsEmpty(),
			"unknown animation condition operations must fail graph validation");
		Require(controller->Initialize(roundTrip, &errors) && errors.IsEmpty(),
			"a valid controller graph must compile without diagnostics");

		roundTrip.GetStates()[1].m_id = 100;
		Require(!controller->Initialize(roundTrip, &errors) && !errors.IsEmpty(),
			"duplicate state ids must produce validation diagnostics");

		AnimationSetAsset setSource;
		setSource.GetEntries().Add({ "Idle", FileId::CreateNewFileId() });
		AnimationSetAsset setRoundTrip;
		setRoundTrip.Deserialize(setSource.Serialize());
		Require(setRoundTrip.GetEntries().Num() == 1 &&
			setRoundTrip.GetEntries()[0].m_slot == "Idle" &&
			setRoundTrip.GetEntries()[0].m_animation ==
				setSource.GetEntries()[0].m_animation,
			"animation set YAML must retain logical slots and clip FileIds");
		auto animationSet = AnimationSetPtr::Make(allocator, FileId{});
		Require(animationSet->Initialize(setRoundTrip, &errors),
			"a valid animation set must compile without diagnostics");
		setRoundTrip.GetEntries().Add(setRoundTrip.GetEntries()[0]);
		Require(!animationSet->Initialize(setRoundTrip, &errors) && !errors.IsEmpty(),
			"duplicate animation set slots must produce validation diagnostics");
		animationSet.DestroyObject(allocator);
		controller.DestroyObject(allocator);
	}

	void TestAnimationControllerTransitionsAndIndependentInstances()
	{
		auto allocator = Memory::ObjectAllocatorPtr::Make(
			Memory::EAllocationPolicy::SharedMemory_MultiThreaded);
		auto controller = AnimationControllerPtr::Make(allocator, FileId{});
		AnimationControllerAsset asset = MakeControllerAsset();
		Require(controller->Initialize(asset),
			"controller fixture must compile");

		AnimationControllerInstance first;
		AnimationControllerInstance second;
		Require(first.SetController(controller) && second.SetController(controller),
			"each Animator must create an independent controller instance");
		Require(first.SetFloat("Speed", 1.0f) &&
			first.SetBool("Grounded", true) &&
			first.SetInt("Mode", 2) &&
			first.SetTrigger("Jump"),
			"typed controller parameters must accept matching values");
		Require(!first.SetFloat("Mode", 1.0f) &&
			!first.SetTrigger("Missing"),
			"typed controller parameters must reject mismatched or missing fields");

		first.Tick(0.0f, 1.0f);
		Require(first.IsTransitioning() &&
			first.GetDestinationStateIndex() == 2,
			"the highest-priority eligible transition must win deterministically");
		Require(!second.IsTransitioning() && second.GetActiveStateIndex() == 0,
			"shared controller assets must not share mutable state or parameters");

		first.Tick(0.1f, 1.0f);
		Require(NearlyEqual(first.GetTransitionAlpha(), 0.5f),
			"crossfade progress must advance independently from state clocks");
		first.Tick(0.1f, 1.0f);
		Require(!first.IsTransitioning() && first.GetActiveStateIndex() == 2,
			"crossfade completion must promote the destination state");

		first.Tick(0.29f, 1.0f);
		Require(first.GetActiveStateIndex() == 2,
			"normalized exit-time transitions must not fire early");
		first.Tick(0.01f, 1.0f);
		Require(first.GetActiveStateIndex() == 0 && !first.IsTransitioning(),
			"zero-duration transitions must complete at the normalized exit time");

		first.SetFloat("Speed", 0.0f);
		first.Tick(0.0f, 1.0f);
		Require(first.GetActiveStateIndex() == 0 && !first.IsTransitioning(),
			"a trigger must be consumed only by the transition that selected it");

		AnimationStateDefinition alternateJump;
		alternateJump.m_id = 400;
		alternateJump.m_name = "Alternate Jump";
		alternateJump.m_clipSlot = "AlternateJump";
		asset.GetStates().Add(alternateJump);
		AnimationTransitionDefinition authoredLater;
		authoredLater.m_id = 1003;
		authoredLater.m_fromStateId = 100;
		authoredLater.m_toStateId = 400;
		authoredLater.m_priority = 10;
		authoredLater.m_conditions = {
			AnimationTransitionCondition{
				.m_parameterId = 4,
				.m_operation = EAnimationConditionOperation::IsSet
			}
		};
		asset.GetTransitions().Add(std::move(authoredLater));
		Require(controller->Initialize(asset),
			"equal-priority transition fixture must compile");
		AnimationControllerInstance authoredOrder;
		Require(authoredOrder.SetController(controller) &&
			authoredOrder.SetTrigger("Jump"),
			"equal-priority transition fixture must accept its trigger");
		authoredOrder.Tick(0.0f, 1.0f);
		Require(authoredOrder.IsTransitioning() &&
			controller->GetStates()[authoredOrder.GetDestinationStateIndex()].m_id == 300,
			"equal-priority transitions must select the first authored transition deterministically");

		first = AnimationControllerInstance{};
		second = AnimationControllerInstance{};
		authoredOrder = AnimationControllerInstance{};
		controller.DestroyObject(allocator);
	}

	void TestAnimationControllerHotReloadPreservesStableState()
	{
		auto allocator = Memory::ObjectAllocatorPtr::Make(
			Memory::EAllocationPolicy::SharedMemory_MultiThreaded);
		auto controller = AnimationControllerPtr::Make(allocator, FileId{});
		AnimationControllerAsset asset = MakeControllerAsset();
		asset.GetTransitions()[0].m_duration = 0.0f;
		Require(controller->Initialize(asset),
			"controller fixture must compile before hot reload");

		AnimationControllerInstance instance;
		Require(instance.SetController(controller) &&
			instance.SetFloat("Speed", 1.0f) &&
			instance.SetBool("Grounded", true) &&
			instance.SetInt("Mode", 2),
			"controller instance must accept initial typed values");
		instance.Tick(0.0f, 1.0f);
		Require(controller->GetStates()[instance.GetActiveStateIndex()].m_id == 200,
			"fixture must enter the stable Walk state before reload");

		std::swap(asset.GetStates()[0], asset.GetStates()[1]);
		std::swap(asset.GetParameters()[0], asset.GetParameters()[3]);
		const uint64_t previousRevision = controller->GetRevision();
		Require(controller->Initialize(asset) &&
			controller->GetRevision() == previousRevision + 1,
			"a valid hot reload must publish one new controller revision");
		instance.Tick(0.0f, 1.0f);
		Require(controller->GetStates()[instance.GetActiveStateIndex()].m_id == 200,
			"hot reload must preserve the active state by stable id rather than array index");
		Require(instance.SetFloat("Speed", 0.25f) && instance.SetTrigger("Jump"),
			"hot reload must rebind typed parameters after source order changes");

		for (auto& parameter : asset.GetParameters())
		{
			if (parameter.m_id == 1)
			{
				parameter.m_type = EAnimationParameterType::Bool;
				parameter.m_defaultBool = true;
				break;
			}
		}
		asset.GetTransitions()[0].m_conditions[0].m_operation =
			EAnimationConditionOperation::Equal;
		asset.GetTransitions()[0].m_conditions[0].m_boolValue = true;
		AnimationTransitionDefinition resetChangedType;
		resetChangedType.m_id = 2000;
		resetChangedType.m_fromStateId = 200;
		resetChangedType.m_toStateId = 100;
		resetChangedType.m_duration = 0.0f;
		resetChangedType.m_conditions = {
			AnimationTransitionCondition{
				.m_parameterId = 1,
				.m_operation = EAnimationConditionOperation::Equal,
				.m_boolValue = true
			}
		};
		asset.GetTransitions().Add(std::move(resetChangedType));
		Require(controller->Initialize(asset),
			"a compatible graph with a changed parameter type must hot reload");
		instance.Tick(0.0f, 1.0f);
		Require(controller->GetStates()[instance.GetActiveStateIndex()].m_id == 100 &&
			!instance.SetFloat("Speed", 0.5f) && instance.SetBool("Speed", false),
			"hot reload must reset a stable parameter id to its new typed default");

		const uint64_t lastValidRevision = controller->GetRevision();
		asset.GetStates()[1].m_id = asset.GetStates()[0].m_id;
		TVector<std::string> errors;
		Require(!controller->Initialize(asset, &errors) &&
			controller->GetRevision() == lastValidRevision &&
			controller->GetStates()[instance.GetActiveStateIndex()].m_id == 100,
			"an invalid hot reload must retain the last valid immutable runtime graph");

		Require(instance.SetController(controller) && instance.SetBool("Speed", false),
			"active-state reset fixture must restart the last valid controller");
		instance.Tick(0.75f, 1.0f);
		Require(NearlyEqual(instance.GetActiveStateTime(), 0.75f),
			"active-state reset fixture must accumulate playback time");

		AnimationControllerAsset fallbackAsset;
		fallbackAsset.SetDefaultStateId(300);
		fallbackAsset.GetStates().Add(AnimationStateDefinition{
			.m_id = 300,
			.m_name = "Fallback",
			.m_clipSlot = "Fallback"
		});
		Require(controller->Initialize(fallbackAsset),
			"a hot reload may remove the previously active state");
		instance.Tick(0.0f, 1.0f);
		Require(controller->GetStates()[instance.GetActiveStateIndex()].m_id == 300 &&
			NearlyEqual(instance.GetActiveStateTime(), 0.0f) &&
			!instance.IsTransitioning(),
			"a removed active state must fall back with a fresh clock and no stale transition");

		instance = AnimationControllerInstance{};
		controller.DestroyObject(allocator);
	}

	void TestLocalPoseSamplingAndComposition()
	{
		auto allocator = Memory::ObjectAllocatorPtr::Make(
			Memory::EAllocationPolicy::SharedMemory_MultiThreaded);
		auto animation = AnimationPtr::Make(allocator, FileId{});
		animation->m_numFrames = 2;
		animation->m_numBones = 2;
		animation->m_fps = 1.0f;
		animation->m_duration = 1.0f;
		animation->m_parentBoneIndices = { -1, 0 };
		animation->m_frames = {
			Math::Transform(glm::vec4(0.0f, 0.0f, 0.0f, 1.0f)),
			Math::Transform(glm::vec4(1.0f, 0.0f, 0.0f, 1.0f)),
			Math::Transform(glm::vec4(2.0f, 0.0f, 0.0f, 1.0f)),
			Math::Transform(glm::vec4(1.0f, 0.0f, 0.0f, 1.0f))
		};

		TVector<Math::Transform> localPose;
		uint32_t frameIndex = 0;
		float lerp = 0.0f;
		Require(AnimationPose::Sample(
			animation,
			0.5f,
			false,
			localPose,
			frameIndex,
			lerp) &&
			frameIndex == 0 && NearlyEqual(lerp, 0.5f),
			"pose sampling must interpolate baked local-space frames by clip time");

		TVector<glm::mat4> globalMatrices;
		TVector<uint8_t> composeState;
		Require(AnimationPose::ComposeLocalPose(
			localPose,
			animation->m_parentBoneIndices,
			globalMatrices,
			composeState),
			"a valid local bone hierarchy must compose successfully");
		Require(NearlyEqual(globalMatrices[0][3].x, 1.0f) &&
			NearlyEqual(globalMatrices[1][3].x, 2.0f),
			"child matrices must inherit their sampled parent transform exactly once");

		TVector<Math::Transform> destinationPose = localPose;
		destinationPose[0].m_position.x = 3.0f;
		TVector<Math::Transform> blendedPose;
		Require(AnimationPose::BlendLocalPoses(
			localPose,
			destinationPose,
			0.5f,
			blendedPose) &&
			NearlyEqual(blendedPose[0].m_position.x, 2.0f),
			"crossfades must blend sampled local TRS poses before hierarchy composition");

		animation->m_parentBoneIndices = { 1, 0 };
		Require(!AnimationPose::ComposeLocalPose(
			localPose,
			animation->m_parentBoneIndices,
			globalMatrices,
			composeState),
			"cyclic skeletons must be diagnosed without unbounded recursion");

		animation.DestroyObject(allocator);
	}
}

int main()
{
	const std::pair<const char*, std::function<void()>> tests[] = {
		{ "TimestampValidationAndNonUniformSpan", TestTimestampValidationAndNonUniformSpan },
		{ "LinearAndStepVectorSampling", TestLinearAndStepVectorSampling },
		{ "CubicSplineSamplingScalesTangentsByInterval", TestCubicSplineSamplingScalesTangentsByInterval },
		{ "RotationSamplingProducesNormalizedShortestPath", TestRotationSamplingProducesNormalizedShortestPath },
		{ "AnimationControllerSerializationAndValidation", TestAnimationControllerSerializationAndValidation },
		{ "AnimationControllerTransitionsAndIndependentInstances", TestAnimationControllerTransitionsAndIndependentInstances },
		{ "AnimationControllerHotReloadPreservesStableState", TestAnimationControllerHotReloadPreservesStableState },
		{ "LocalPoseSamplingAndComposition", TestLocalPoseSamplingAndComposition }
	};

	for (const auto& test : tests)
	{
		try
		{
			test.second();
			std::cout << "[PASS] " << test.first << std::endl;
		}
		catch (const std::exception& error)
		{
			std::cerr << "[FAIL] " << test.first << ": "
				<< error.what() << std::endl;
			return 1;
		}
	}

	return 0;
}
