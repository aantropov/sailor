#pragma once

#include "Engine/InstanceId.h"
#include "Math/Transform.h"

#include <string>
#include <variant>

namespace Sailor::EditorViewport
{
	enum class ETransformOperation : uint8_t
	{
		Select = 0,
		Translate,
		Rotate,
		Scale
	};

	enum class ETransformSpace : uint8_t
	{
		World = 0,
		Local
	};

	struct SelectionEvent
	{
		InstanceId m_instanceId;
	};

	struct TransformEvent
	{
		InstanceId m_instanceId;
		Math::Transform m_before;
		Math::Transform m_after;
		ETransformOperation m_operation = ETransformOperation::Translate;
		ETransformSpace m_space = ETransformSpace::World;
	};

	struct AssetDropEvent
	{
		std::string m_fileId;
		glm::vec2 m_position{};
	};

	struct ToolShortcutEvent
	{
		uint32_t m_keyCode = 0;
	};

	struct Event
	{
		uint64_t m_revision = 0;
		uint64_t m_managedMutationRevision = 0;
		std::variant<SelectionEvent, TransformEvent, AssetDropEvent, ToolShortcutEvent> m_payload;
	};
}
