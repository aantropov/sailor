#pragma once
#include "Core/Defines.h"
#include "Tasks/Tasks.h"
#include "Core/Submodule.h"
#include "AssetRegistry/AssetFactory.h"
#include "AnimationAssetInfo.h"
#include "Engine/Object.h"
#include "Math/Transform.h"
#include <glm/mat4x4.hpp>
#include "Containers/Vector.h"
#include "Containers/ConcurrentMap.h"
#include "Memory/ObjectAllocator.hpp"

using namespace Sailor::Memory;

namespace Sailor
{
	class Animation : public Object
	{
	public:
		SAILOR_API Animation(FileId uid) : Object(uid) {}

		TVector<Math::Transform> m_frames;
		uint32_t m_numFrames = 0;
		uint32_t m_numBones = 0;
		float m_fps = 30.0f;
	};

	using AnimationPtr = TObjectPtr<Animation>;

	class AnimationImporter final : public TSubmodule<AnimationImporter>, public IAssetInfoHandlerListener, public IAssetFactory
	{
	public:
		SAILOR_API AnimationImporter(AnimationAssetInfoHandler* infoHandler);
		SAILOR_API virtual ~AnimationImporter() override;

		SAILOR_API virtual void OnImportAsset(AssetInfoPtr assetInfo) override {}
		SAILOR_API virtual void OnUpdateAssetInfo(AssetInfoPtr assetInfo, bool bWasExpired) override {}

		SAILOR_API bool LoadAsset(FileId uid, TObjectPtr<Object>& out, bool bImmediate = true) override;
		SAILOR_API Tasks::TaskPtr<AnimationPtr> LoadAnimation(FileId uid, AnimationPtr& outAnimation);
		SAILOR_API bool LoadAnimation_Immediate(FileId uid, AnimationPtr& outAnimation);

		SAILOR_API virtual void CollectGarbage() override {}

	protected:
		TConcurrentMap<FileId, Tasks::TaskPtr<AnimationPtr>> m_promises{};
		TConcurrentMap<FileId, AnimationPtr> m_loadedAnimations{};
		ObjectAllocatorPtr m_allocator;

		SAILOR_API bool ImportAnimation(FileId uid, AnimationPtr& outAnimation);
	};
}
