#include "Components/Tests/ShadowBindingPublicationTestComponent.h"
#include "ECS/CameraECS.h"
#include "ECS/LightingECS.h"
#include "ECS/TransformECS.h"
#include "Engine/GameObject.h"
#include "Engine/World.h"
#include "FrameGraph/ShadowPrepassNode.h"
#include "GraphicsDriver/Vulkan/VulkanDescriptors.h"
#include "GraphicsDriver/Vulkan/VulkanImage.h"
#include "GraphicsDriver/Vulkan/VulkanImageView.h"
#include "RHI/Material.h"
#include "RHI/RenderTarget.h"
#include "RHI/SceneView.h"
#include "RHI/Shader.h"
#include <array>
#include <format>

using namespace Sailor;
using namespace Sailor::RHI;
using namespace Sailor::GraphicsDriver::Vulkan;

namespace
{
	class ShadowPublicationWorld final : public World
	{
	public:
		ShadowPublicationWorld() : World("Shadow binding publication fixture", 0, CreateEcs()) { BeginPlayEcs(); }
		~ShadowPublicationWorld() override { Clear(); }

	private:
		static TVector<ECS::TBaseSystemPtr> CreateEcs()
		{
			TVector<ECS::TBaseSystemPtr> systems;
			systems.Add(TUniquePtr<TransformECS>::Make());
			systems.Add(TUniquePtr<LightingECS>::Make());
			return systems;
		}
	};

	struct RestoreShadowView
	{
		RHIRenderTargetPtr m_texture;
		VulkanImageViewPtr m_view;
		~RestoreShadowView() { m_texture->m_vulkan.m_imageView = m_view; }
	};

	std::string ValidateDirectionalShadowOwner()
	{
		ShadowPublicationWorld world;
		auto* lighting = world.GetECS<LightingECS>();
		const auto addLight = [&](std::string_view name, ELightType type, glm::vec3 rotation, float intensity)
		{
			auto owner = world.Instantiate(name);
			owner->GetTransformComponent().SetRotation(glm::quat(glm::radians(rotation)));
			const size_t index = lighting->RegisterComponent();
			auto& light = lighting->GetComponentData(index);
			light.SetOwner(owner);
			light.m_type = type;
			light.m_shadowType = EShadowType::PCF;
			light.m_shadowQuality = ELightShadowQuality::VeryLow;
			light.m_radius = 80.0f;
			light.m_intensity = glm::vec3(intensity);
			light.MarkDirty();
			return index;
		};
		const std::array suns{
			addLight("First shadow sun", ELightType::Directional, { -35, 20, 0 }, 3.0f),
			addLight("Second shadow sun", ELightType::Directional, { -15, -70, 0 }, 7.0f) };
		const std::array localLights{
			addLight("Local point shadow", ELightType::Point, {}, 1.0f),
			addLight("Local spot shadow", ELightType::Spot, { -35, 20, 0 }, 1.0f) };
		world.GetECS<TransformECS>()->Tick(0.0f);
		const auto makeScene = [&](uint64_t frame, uint32_t flight)
		{
			auto scene = RHISceneViewPtr::Make();
			scene->m_world = &world;
			CameraData camera;
			camera.SetAspect(16.0f / 9.0f);
			camera.SetFov(60.0f);
			camera.SetZNear(0.1f);
			camera.SetZFar(80.0f);
			for (float x : { 0.0f, 8.0f })
			{
				scene->m_cameras.Add(camera);
				scene->m_cameraTransforms.Add(Math::Transform(glm::vec4(x, 2.0f, 12.0f, 1.0f)));
			}
			auto context = RHIRenderSubmissionContextPtr::Make();
			context->BeginSubmission(frame, flight);
			scene->SetSubmissionContext(context);
			return scene;
		};
		const auto fill = [&](RHISceneViewPtr& scene)
		{
			lighting->Tick(0.0f);
			scene->m_rhiLightsDataPerCamera.Clear(false);
			lighting->FillLightingData(scene, scene->m_submissionContext->GetFlightSlot());
		};
		const auto validateScene = [&](const RHISceneViewPtr& scene, uint32_t shadowOwner, bool bExpectUpdates) -> std::string
		{
			if (!scene->m_cpuLightsData || scene->m_cpuLightsData->Num() != 4u ||
				scene->m_rhiLightsDataPerCamera.Num() != 2u)
				return "directional ownership fixture did not publish four light slots and two cameras";
			for (const size_t sun : suns)
			{
				const auto expectedType = sun == shadowOwner ? EShadowType::PCF : EShadowType::None;
				if ((*scene->m_cpuLightsData)[sun].m_shadowType != static_cast<uint32_t>(expectedType))
					return "CPU light publication did not select exactly one directional shadow owner";
			}
			for (size_t view = 0; view < 2; ++view)
			{
				TVector<glm::mat4> projections;
				glm::mat4 lightView(1.0f);
				if (shadowOwner != LightingECS::InvalidShadowMapIndex)
				{
					auto* owner = static_cast<GameObject*>(lighting->GetComponentData(shadowOwner).GetOwner().GetRawPtr());
					lightView = glm::inverse(owner->GetTransformComponent().GetCachedWorldMatrix());
					const auto& camera = scene->m_cameras[view];
					ShadowPrepassNode::CalculateLightProjectionForCascades(lightView,
						scene->m_cameraTransforms[view].Matrix(), camera.GetAspect(), camera.GetFov(),
						camera.GetZNear(), camera.GetZFar(), projections);
				}
				const auto bindings = scene->m_rhiLightsDataPerCamera[view];
				RHIShaderBindingPtr maps;
				if (!bindings || !bindings->m_vulkan.m_descriptorSet ||
					!bindings->GetShaderBindings().TryGet("shadowMaps"_h, maps))
					return "a camera is missing its native shadow map binding";
				for (uint32_t cascade = 0; cascade < LightingECS::NumCascades; ++cascade)
				{
					const auto expected = cascade < projections.Num() ? projections[cascade] * lightView : glm::mat4(1.0f);
					if (!Math::AreExactlyEqual(scene->m_shadowMatrices[view][cascade], expected))
						return "a camera copied CSM matrices from a different owner, camera or inactive cascade";
				}
				size_t numCsmUpdates = 0;
				for (const auto& update : scene->m_shadowMapsToUpdate[view])
				{
					const uint32_t cascade = update.m_lighMatrixIndex;
					if (cascade >= LightingECS::NumCascades) continue;
					++numCsmUpdates;
					if (cascade >= projections.Num() || !update.m_shadowMap ||
						!Math::AreExactlyEqual(update.m_lightMatrix, projections[cascade] * lightView) ||
						maps->GetTextureBinding(cascade) != update.m_shadowMap ||
						!bindings->m_vulkan.m_descriptorSet->ReferencesImageView(9u, cascade, update.m_shadowMap->m_vulkan.m_imageView))
						return "CSM matrix, texture and native descriptor refer to different directional owners";
				}
				if (numCsmUpdates != (bExpectUpdates ? projections.Num() : 0u))
					return "directional ownership generated duplicate or unexpected CSM work";
				for (const size_t local : localLights)
				{
					if ((*scene->m_cpuLightsData)[local].m_shadowType != static_cast<uint32_t>(EShadowType::PCF) ||
						scene->m_shadowIndices[view][local] == LightingECS::InvalidShadowMapIndex)
						return "directional shadow selection removed a local point/spot shadow";
				}
			}
			if (lighting->GetLocalShadowsOccupiedMemoryMb() <= 0.0f ||
				lighting->GetShadowsOccupiedMemoryMb() > lighting->GetShadowsMemoryBudgetMb())
				return "the two-camera directional/local shadow fixture exceeded its memory budget";
			return {};
		};

		auto sceneA = makeScene(90u, 0u);
		fill(sceneA);
		if (auto error = validateScene(sceneA, static_cast<uint32_t>(suns[0]), true); !error.empty()) return error;
		if ((*sceneA->m_cpuLightsData)[suns[1]].m_type != static_cast<uint32_t>(ELightType::Directional) ||
			(*sceneA->m_cpuLightsData)[suns[1]].m_intensity != glm::vec3(7.0f))
			return "the unshadowed directional light lost its illumination";
		auto retainedLights = sceneA->m_cpuLightsData;
		const auto retainedBindings = sceneA->m_rhiLightsDataPerCamera;
		const auto retainedMatrices = sceneA->m_shadowMatrices;
		std::array<RHIShaderBindingPtr, 2> retainedMaps;
		for (size_t view = 0; view < 2; ++view)
			retainedBindings[view]->GetShaderBindings().TryGet("shadowMaps"_h, retainedMaps[view]);
		if (retainedMaps[0]->GetTextureBinding(0) == retainedMaps[1]->GetTextureBinding(0))
			return "different cameras shared one writable CSM target";

		lighting->GetComponentData(suns[0]).m_shadowType = EShadowType::None;
		lighting->GetComponentData(suns[0]).MarkDirty();
		auto sceneB = makeScene(91u, 1u);
		fill(sceneB);
		if (auto error = validateScene(sceneB, static_cast<uint32_t>(suns[1]), true); !error.empty()) return error;
		if ((*sceneB->m_cpuLightsData)[suns[0]].m_intensity != glm::vec3(3.0f) ||
			lighting->GetComponentData(suns[1]).m_shadowType != EShadowType::PCF)
			return "owner transfer changed light energy or an authored shadow setting";
		fill(sceneB);
		if (auto error = validateScene(sceneB, static_cast<uint32_t>(suns[1]), false); !error.empty()) return error;

		auto& first = lighting->GetComponentData(suns[0]);
		first.m_shadowType = EShadowType::PCF;
		first.m_globalIlluminationMode = ELightGlobalIlluminationMode::BakedOnly;
		first.MarkDirty();
		const LightData secondLight = lighting->GetComponentData(suns[1]);
		lighting->UnregisterComponent(suns[1]);
		fill(sceneB);
		if (auto error = validateScene(sceneB, LightingECS::InvalidShadowMapIndex, false); !error.empty()) return error;
		first.m_globalIlluminationMode = ELightGlobalIlluminationMode::Realtime;
		first.MarkDirty();
		fill(sceneB);
		if (auto error = validateScene(sceneB, static_cast<uint32_t>(suns[0]), true); !error.empty()) return error;
		const size_t restored = lighting->RegisterComponent();
		if (restored != suns[1]) return "re-registering the second sun did not reuse its free slot";
		lighting->GetComponentData(restored) = secondLight;
		lighting->GetComponentData(restored).MarkDirty();
		lighting->UnregisterComponent(suns[0]);
		fill(sceneB);
		if (auto error = validateScene(sceneB, static_cast<uint32_t>(suns[1]), true); !error.empty()) return error;

		if (sceneA->m_cpuLightsData != retainedLights ||
			(*retainedLights)[suns[0]].m_shadowType != static_cast<uint32_t>(EShadowType::PCF) ||
			(*retainedLights)[suns[1]].m_shadowType != static_cast<uint32_t>(EShadowType::None))
			return "a later flight rewrote the retained directional-light publication";
		for (size_t view = 0; view < 2; ++view)
		{
			if (sceneA->m_rhiLightsDataPerCamera[view] != retainedBindings[view])
				return "a later flight replaced the retained camera's shadow binding";
			for (const auto& update : sceneA->m_shadowMapsToUpdate[view])
			{
				if (update.m_lighMatrixIndex >= LightingECS::NumCascades) continue;
				const auto cascade = update.m_lighMatrixIndex;
				if (retainedMaps[view]->GetTextureBinding(cascade) != update.m_shadowMap ||
					!Math::AreExactlyEqual(sceneA->m_shadowMatrices[view][cascade], retainedMatrices[view][cascade]) ||
					!retainedBindings[view]->m_vulkan.m_descriptorSet->ReferencesImageView(9u, cascade, update.m_shadowMap->m_vulkan.m_imageView))
					return "a later owner changed a prior flight's CSM matrix, image or native descriptor";
			}
		}
		return {};
	}

	std::string ValidateShadowPublication()
	{
		ShadowPublicationWorld world;
		auto owner = world.Instantiate("Fixture directional light");
		if (!owner) return "could not instantiate the private directional-light owner";
		owner->GetTransformComponent().SetRotation(glm::quat(glm::radians(glm::vec3(-35.0f, 20.0f, 0.0f))));
		auto* transforms = world.GetECS<TransformECS>();
		transforms->Tick(0.0f);
		transforms->PostTick();
		auto* lighting = world.GetECS<LightingECS>();
		auto& light = lighting->GetComponentData(lighting->RegisterComponent());
		light.SetOwner(owner);
		light.m_type = ELightType::Directional;
		light.m_shadowType = EShadowType::PCF;
		light.MarkDirty();
		lighting->Tick(0.0f);

		auto scene = RHISceneViewPtr::Make();
		scene->m_world = &world;
		CameraData camera;
		camera.SetAspect(16.0f / 9.0f);
		camera.SetFov(60.0f);
		camera.SetZNear(0.1f);
		camera.SetZFar(80.0f);
		scene->m_cameras.Add(camera);
		scene->m_cameraTransforms.Add(Math::Transform(glm::vec4(0.0f, 2.0f, 12.0f, 1.0f)));
		auto context = RHIRenderSubmissionContextPtr::Make();
		context->BeginSubmission(77u, 0u);
		scene->SetSubmissionContext(context);
		auto token = scene->GetOrCreateSubmissionCompletionToken();
		const auto fill = [&]()
		{
			// Fill appends these entries; reuse the camera/context/token, not the previous output vector.
			scene->m_rhiLightsDataPerCamera.Clear(false);
			lighting->FillLightingData(scene, scene->m_submissionContext->GetFlightSlot());
		};

		fill();
		auto publishedA = scene->m_rhiLightsData;
		if (!publishedA || !publishedA->m_vulkan.m_descriptorSet || !publishedA->m_vulkan.m_descriptorSet->IsCompiled() ||
			scene->m_rhiLightsDataPerCamera.Num() != 1u || scene->m_rhiLightsDataPerCamera[0] != publishedA ||
			scene->m_shadowMapsToUpdate.Num() != 1u || scene->m_shadowMapsToUpdate[0].IsEmpty())
			return "A did not publish a compiled global/per-camera shadow template with real CSM updates";
		const uint32_t cascade = scene->m_shadowMapsToUpdate[0][0].m_lighMatrixIndex;
		auto shadowMap = scene->m_shadowMapsToUpdate[0][0].m_shadowMap;
		RHIShaderBindingPtr bindingA;
		if (!shadowMap || shadowMap->GetExtent().x <= 1 || shadowMap->GetExtent().y <= 1 ||
			!shadowMap->m_vulkan.m_imageView || static_cast<VkImageView>(*shadowMap->m_vulkan.m_imageView) == VK_NULL_HANDLE ||
			!publishedA->GetShaderBindings().TryGet("shadowMaps"_h, bindingA) || !bindingA ||
			bindingA->GetTextureBinding(cascade) != shadowMap)
			return "A did not expose its own compiled CSM texture in the shadowMaps binding";
		auto originalView = shadowMap->m_vulkan.m_imageView;
		auto nativeA = publishedA->m_vulkan.m_descriptorSet;
		const VkDescriptorSet handleA = *nativeA;
		const uint64_t revisionA = publishedA->GetDescriptorRevision();
		if (!nativeA->ReferencesImageView(9u, cascade, originalView)) return "A's native descriptor did not retain the CSM view";

		// Only this private world's CSM wrapper is changed. Native A still owns its original compiled view.
		RestoreShadowView restore{ shadowMap, originalView };
		auto unavailable = VulkanImageViewPtr::Make(shadowMap->m_vulkan.m_image->GetDevice(), shadowMap->m_vulkan.m_image);
		if (static_cast<VkImageView>(*unavailable) != VK_NULL_HANDLE) return "failure fixture unexpectedly compiled its image view";
		shadowMap->m_vulkan.m_imageView = unavailable;
		scene->m_cameraTransforms[0].m_position.x += 32.0f;
		fill();
		if (scene->m_shadowMapsToUpdate.Num() != 1u || scene->m_shadowMapsToUpdate[0].IsEmpty() ||
			!scene->m_shadowMapsToUpdate[0].ContainsIf([&](const RHIUpdateShadowMapCommand& update)
				{ return update.m_lighMatrixIndex == cascade && update.m_shadowMap == shadowMap; }))
			return "camera move B did not update the same owned CSM texture";
		const bool globalRetained = scene->m_rhiLightsData == publishedA;
		const bool cameraRetained = scene->m_rhiLightsDataPerCamera.Num() == 1u && scene->m_rhiLightsDataPerCamera[0] == publishedA;
		RHIShaderBindingPtr afterFailure;
		const bool nativeRetained = publishedA->GetShaderBindings().TryGet("shadowMaps"_h, afterFailure) && afterFailure == bindingA &&
			publishedA->m_vulkan.m_descriptorSet == nativeA && nativeA->IsCompiled() && static_cast<VkDescriptorSet>(*nativeA) == handleA &&
			publishedA->GetDescriptorRevision() == revisionA && nativeA->ReferencesImageView(9u, cascade, originalView);
		if (!globalRetained || !cameraRetained || !nativeRetained)
			return std::format("failed B replaced shadow publication A: global retained={}, per-camera retained={}, native retained={}",
				globalRetained, cameraRetained, nativeRetained);

		shadowMap->m_vulkan.m_imageView = originalView;
		fill();
		if (scene->m_submissionContext != context || scene->GetOrCreateSubmissionCompletionToken() != token || !token->IsPending() ||
			scene->m_shadowMapsToUpdate.Num() != 1u || !scene->m_shadowMapsToUpdate[0].IsEmpty())
			return "retry C changed the pending submission or generated fresh CSM updates";
		auto publishedC = scene->m_rhiLightsData;
		RHIShaderBindingPtr bindingC;
		if (!publishedC || publishedC == publishedA || scene->m_rhiLightsDataPerCamera.Num() != 1u ||
			scene->m_rhiLightsDataPerCamera[0] != publishedC || !publishedC->m_vulkan.m_descriptorSet ||
			publishedC->m_vulkan.m_descriptorSet == nativeA || !publishedC->m_vulkan.m_descriptorSet->IsCompiled() ||
			!publishedC->GetShaderBindings().TryGet("shadowMaps"_h, bindingC) || !bindingC || bindingC == bindingA ||
			bindingC->GetTextureBinding(cascade) != shadowMap ||
			!publishedC->m_vulkan.m_descriptorSet->ReferencesImageView(9u, cascade, originalView))
			return "same-input retry C did not publish a distinct valid shadow template after restoring the view";
		auto nativeC = publishedC->m_vulkan.m_descriptorSet;
		const uint64_t revisionC = publishedC->GetDescriptorRevision();
		fill();
		RHIShaderBindingPtr stableBinding;
		if (scene->m_shadowMapsToUpdate.Num() != 1u || !scene->m_shadowMapsToUpdate[0].IsEmpty() ||
			scene->m_rhiLightsData != publishedC || scene->m_rhiLightsDataPerCamera.Num() != 1u ||
			scene->m_rhiLightsDataPerCamera[0] != publishedC || publishedC->m_vulkan.m_descriptorSet != nativeC ||
			publishedC->GetDescriptorRevision() != revisionC || !publishedC->GetShaderBindings().TryGet("shadowMaps"_h, stableBinding) ||
			stableBinding != bindingC || scene->GetOrCreateSubmissionCompletionToken() != token || !token->IsPending())
			return "unchanged fourth Fill republished C or regenerated shadow work";
		return {};
	}
}

void ShadowBindingPublicationTestComponent::Tick(float)
{
	if (IsFinished()) return;
	if (!m_validation)
	{
		m_validation = Tasks::CreateTaskWithResult<std::string>("Shadow binding publication validation"_h,
			[]()
			{
				if (auto error = ValidateShadowPublication(); !error.empty()) return error;
				return ValidateDirectionalShadowOwner();
			}, EThreadType::RHI);
		m_validation->Run();
		return;
	}
	if (!m_validation->IsFinished()) return;
	const auto& error = m_validation->GetResult();
	if (!error.empty()) { MarkFailed(error); return; }
	AddJournalEvent("ShadowBindingPublicationEvidence",
		"Owned CSM native A survived failed B globally/per-camera; restored view published distinct C with zero CSM updates on the same pending submission, and fourth Fill retained C");
	AddJournalEvent("ShadowBindingPublicationScope", "Descriptor publication only: no shadow pixels, atlas/matrix transaction or forced allocation failure");
	AddJournalEvent("DirectionalShadowOwnerEvidence",
		"Two cameras share one explicit sun selection; matrices, native maps and effective light types agree through disable, GI-mode and removal transitions; prior flight and local point/spot shadows remain intact");
	MarkPassed();
}
