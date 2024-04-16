#pragma once
#include "Memory/UniquePtr.hpp"
#include "Memory/ObjectPtr.hpp"
#include "Memory/SharedPtr.hpp"

namespace Sailor
{
	using ModelPtr = TObjectPtr<class Model>;
	using MaterialPtr = TObjectPtr<class Material>;
	using TexturePtr = TObjectPtr<class Texture>;
	using ShaderSetPtr = TObjectPtr<class ShaderSet>;
	using FrameGraphPtr = TObjectPtr<class FrameGraph>;
	using PrefabPtr = TObjectPtr<class Prefab>;

	using GameObjectPtr = TObjectPtr<class GameObject>;
	using WorldPtr = class World*;
	using ObjectPtr = TObjectPtr<class Object>;
	using FrameGraphPtr = TObjectPtr<class FrameGraph>;

	using ComponentPtr = TObjectPtr<class Component>;
	using CameraComponentPtr = TObjectPtr<class CameraComponent>;
	using MeshRendererComponentPtr = TObjectPtr<class MeshRendererComponent>;

	enum class EMobilityType : uint8_t
	{
		Static = 0,
		Stationary = 1,
		Dynamic = 2
	};

	enum class ELightType : uint8_t
	{
		Directional = 0,
		Point,
		Spot,
		Area
	};
}
