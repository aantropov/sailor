#pragma once

#include "Components/Tests/TestCaseComponent.h"
#include "FrameGraph/CopyTextureToRamNode.h"
#include "RHI/Reflections.h"

#include <array>

namespace Sailor
{
	class ReflectionProbeComponent;
	class ReflectionProbeRenderState;

	class ReflectionProbeTestComponent final : public TestCaseComponent
	{
		SAILOR_REFLECTABLE(ReflectionProbeTestComponent)

	public:
		SAILOR_API ~ReflectionProbeTestComponent() override;
		SAILOR_API void BeginPlay() override;
		SAILOR_API void Tick(float deltaTime) override;
		SAILOR_API void EndPlay() override;

	private:
		enum class Stage
		{
			Prepare, Initial, InitialPixels, Dynamic, Freeze, Move, Remove, RemovedPixels
		};
		GameObjectPtr AddBox(const char* name, glm::vec3 position, glm::vec3 dimensions);
		bool PrepareMaterials();
		bool HasProbeIndex(uint32_t object, uint32_t expected) const;
		bool RequestPixels();
		bool CheckPixels(bool removed);

		Memory::ObjectAllocatorPtr m_allocator = Memory::ObjectAllocatorPtr::Make();
		TVector<MaterialPtr> m_materials;
		ShaderSetPtr m_mirrorShader;
		ShaderSetPtr m_markerShader;
		GameObjectPtr m_camera;
		std::array<GameObjectPtr, 2> m_mirrors;
		std::array<GameObjectPtr, 2> m_markers;
		std::array<GameObjectPtr, 2> m_probes;
		std::array<TObjectPtr<ReflectionProbeComponent>, 2> m_components;
		TSharedPtr<ReflectionProbeRenderState> m_renderState;
		RHI::RHISceneReflectionsSnapshotPtr m_retained;
		Framegraph::CopyTextureToRamNode::CaptureTask m_capture;
		std::array<uint32_t, 2> m_slots{};
		std::array<uint64_t, 2> m_initialGenerations{};
		std::array<uint64_t, 2> m_initialCaptures{};
		std::array<glm::vec3, 2> m_initialPixels{};
		uint64_t m_stageFrame = 0u;
		Stage m_stage = Stage::Prepare;
	};
}

REFL_AUTO(
	type(Sailor::ReflectionProbeTestComponent, bases<Sailor::TestCaseComponent>)
)
