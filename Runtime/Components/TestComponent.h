#pragma once
#include "Sailor.h"
#include "Memory/SharedPtr.hpp"
#include "Memory/ObjectPtr.hpp"
#include "JobSystem/JobSystem.h"
#include "Engine/Object.h"
#include "Components/Component.h"
#include "AssetRegistry/Model/ModelImporter.h"
#include "ECS/StaticMeshRendererECS.h"

namespace Sailor
{
	using WorldPtr = class World*;
	using GameObjectPtr = TObjectPtr<class GameObject>;
	using TestComponentPtr = TObjectPtr<class TestComponent>;

	class TestComponent : public Component
	{
	public:

		SAILOR_API virtual void BeginPlay() override;
		SAILOR_API virtual void Tick(float deltaTime) override;
		SAILOR_API virtual void EndPlay() override;

		SAILOR_API RHI::ShaderBindingSetPtr& GetPerInstanceBinding() { return m_perInstanceData; }
		SAILOR_API RHI::ShaderBindingSetPtr& GetFrameBinding() { return m_frameDataBinding; }

	protected:

		RHI::ShaderBindingSetPtr m_perInstanceData;
		RHI::UboFrameData m_frameData;
		RHI::ShaderBindingSetPtr m_frameDataBinding;

		TexturePtr defaultTexture;

		glm::ivec2 m_lastCursorPos;
	};
}
