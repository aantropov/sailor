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
	using AudioClipPtr = TObjectPtr<class AudioClip>;

	using GameObjectPtr = TObjectPtr<class GameObject>;
	using WorldPtr = class World*;
	using ObjectPtr = TObjectPtr<class Object>;
	using FrameGraphPtr = TObjectPtr<class FrameGraph>;

	using ComponentPtr = TObjectPtr<class Component>;
	using CameraComponentPtr = TObjectPtr<class CameraComponent>;
	using MeshRendererComponentPtr = TObjectPtr<class MeshRendererComponent>;
	using PathTracerProxyComponentPtr = TObjectPtr<class PathTracerProxyComponent>;

	enum class EMobilityType : uint8_t
	{
		Static = 0,
		Stationary = 1,
		Dynamic = 2
	};

	constexpr bool IsMobilityHierarchyValid(
		EMobilityType parentMobility,
		EMobilityType childMobility) noexcept
	{
		return static_cast<uint8_t>(childMobility) >=
			static_cast<uint8_t>(parentMobility);
	}

	enum class ELightType : uint8_t
	{
		Directional = 0,
		Point,
		Spot,
		Area
	};

	enum class ELightGlobalIlluminationMode : uint8_t
	{
		Realtime = 0,
		RealtimeAndBaked,
		BakedOnly
	};

	constexpr bool ContributesToRealtimeLighting(
		ELightGlobalIlluminationMode mode) noexcept
	{
		return mode == ELightGlobalIlluminationMode::Realtime ||
			mode == ELightGlobalIlluminationMode::RealtimeAndBaked;
	}

	constexpr bool ContributesToBakedGlobalIllumination(
		ELightGlobalIlluminationMode mode) noexcept
	{
		return mode == ELightGlobalIlluminationMode::RealtimeAndBaked ||
			mode == ELightGlobalIlluminationMode::BakedOnly;
	}

	enum class ELightShadowQuality : uint8_t
	{
		VeryLow = 0,
		Low,
		Medium,
		High
	};

	enum class ELightShadowFilter : uint8_t
	{
		Hard = 0,
		Soft
	};

	enum class EAnimationPlayMode : uint8_t
	{
		Repeat = 0,
		Once,
		PingPong
	};
}
