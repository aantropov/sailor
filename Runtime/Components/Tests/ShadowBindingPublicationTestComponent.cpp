#include "Components/Tests/ShadowBindingPublicationTestComponent.h"
#include "ECS/CameraECS.h"
#include "ECS/LightingECS.h"
#include "ECS/TransformECS.h"
#include "Engine/GameObject.h"
#include "Engine/World.h"
#include "GraphicsDriver/Vulkan/VulkanDescriptors.h"
#include "GraphicsDriver/Vulkan/VulkanImage.h"
#include "GraphicsDriver/Vulkan/VulkanImageView.h"
#include "RHI/Material.h"
#include "RHI/RenderTarget.h"
#include "RHI/SceneView.h"
#include "RHI/Shader.h"
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
			lighting->FillLightingData(scene);
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
			!publishedA->GetShaderBindings().TryGet("shadowMaps", bindingA) || !bindingA ||
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
		const bool nativeRetained = publishedA->GetShaderBindings().TryGet("shadowMaps", afterFailure) && afterFailure == bindingA &&
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
			!publishedC->GetShaderBindings().TryGet("shadowMaps", bindingC) || !bindingC || bindingC == bindingA ||
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
			publishedC->GetDescriptorRevision() != revisionC || !publishedC->GetShaderBindings().TryGet("shadowMaps", stableBinding) ||
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
		m_validation = Tasks::CreateTaskWithResult<std::string>("Shadow binding publication validation",
			[]() { return ValidateShadowPublication(); }, EThreadType::RHI);
		m_validation->Run();
		return;
	}
	if (!m_validation->IsFinished()) return;
	const auto& error = m_validation->GetResult();
	if (!error.empty()) { MarkFailed(error); return; }
	AddJournalEvent("ShadowBindingPublicationEvidence",
		"Owned CSM native A survived failed B globally/per-camera; restored view published distinct C with zero CSM updates on the same pending submission, and fourth Fill retained C");
	AddJournalEvent("ShadowBindingPublicationScope", "Descriptor publication only: no shadow pixels, atlas/matrix transaction or forced allocation failure");
	MarkPassed();
}
