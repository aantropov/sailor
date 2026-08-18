#pragma once
#include "Sailor.h"
#include "Components/Component.h"

namespace Sailor
{
	class LocalShadowAtlasStressTestSetupComponent : public Component
	{
		SAILOR_REFLECTABLE(LocalShadowAtlasStressTestSetupComponent)

	public:
		SAILOR_API virtual void BeginPlay() override;
		SAILOR_API virtual void Tick(float deltaTime) override;

	private:
		void SpawnBox(const char* name, const glm::vec3& position, const glm::vec3& scale);
		void SpawnGeometryField(const char* namePrefix, float xOffset, uint32_t fieldIndex);
		void SpawnLightField(const char* namePrefix, float xOffset, uint32_t fieldIndex);
		void SpawnGeometry();
		void SpawnLights();
		void EnsureSky();
		void EnsureCamera();

		GameObjectPtr m_camera;
		float m_elapsedTime = 0.0f;
	};
}

using namespace Sailor::Attributes;

REFL_AUTO(
	type(Sailor::LocalShadowAtlasStressTestSetupComponent, bases<Sailor::Component>)
)
