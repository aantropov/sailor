#include "Reflection.h"
#include "Core/YamlUtils.h"
#include "Engine/InstanceId.h"
#include "Containers/Containers.h"
#include "Components/Component.h"
#include "Containers/ConcurrentMap.h"
#include "RHI/Types.h"
#include "Engine/GameObject.h"
#include "AssetRegistry/AssetRegistry.h"
#include "RHI/SceneView.h"
#include "Physics/PhysicsTypes.h"
#include <algorithm>
#include <condition_variable>
#include <iterator>
#include <mutex>
#include <shared_mutex>
#include <unordered_map>
#include <unordered_set>

using namespace Sailor;

YAML::Node TypeInfo::SerializeAssetType() const
{
	return m_serializeAssetType ? m_serializeAssetType() : YAML::Node(YAML::NodeType::Undefined);
}

namespace Sailor::Internal
{
	thread_local bool g_bSuppressEngineAutoRegistration = false;

	struct WorkspaceTypeInvocationState
	{
		std::mutex m_mutex;
		std::condition_variable m_conditionVariable;
		size_t m_numActiveCalls = 0;
		bool m_bUnloading = false;
	};

	class WorkspaceTypeInvocationLease final
	{
	public:
		WorkspaceTypeInvocationLease() = default;
		WorkspaceTypeInvocationLease(const WorkspaceTypeInvocationLease&) = delete;
		WorkspaceTypeInvocationLease& operator=(const WorkspaceTypeInvocationLease&) = delete;

		~WorkspaceTypeInvocationLease()
		{
			if (!m_state)
			{
				return;
			}

			bool bNotify = false;
			{
				std::lock_guard lock(m_state->m_mutex);
				check(m_state->m_numActiveCalls > 0);
				--m_state->m_numActiveCalls;
				bNotify = m_state->m_bUnloading && m_state->m_numActiveCalls == 0;
			}

			if (bNotify)
			{
				m_state->m_conditionVariable.notify_all();
			}
		}

		bool Acquire(const TSharedPtr<WorkspaceTypeInvocationState>& state)
		{
			if (!state)
			{
				return false;
			}

			std::lock_guard lock(state->m_mutex);
			if (state->m_bUnloading)
			{
				return false;
			}

			++state->m_numActiveCalls;
			m_state = state;
			return true;
		}

	private:
		TSharedPtr<WorkspaceTypeInvocationState> m_state;
	};

	struct WorkspaceTypeEntry
	{
		std::string m_owner;
		const TypeInfo* m_typeInfo{};
		TSharedPtr<WorkspaceTypeInvocationState> m_invocationState;
		Reflection::TPlacementFactoryMethod m_placementFactory;
		ReflectedData m_defaultObject;
		size_t m_alignment = 8;
	};
	using WorkspaceTypeEntryPtr = TUniquePtr<WorkspaceTypeEntry>;

	TUniquePtr<TConcurrentMap<std::string, ReflectedData, 32u, ERehashPolicy::Never>> g_pCdos;
	TUniquePtr<TConcurrentMap<std::string, Reflection::TPlacementFactoryMethod>> g_pPlacementFactoryMethods;
	TUniquePtr<TConcurrentMap<std::string, const TypeInfo*>> g_pReflectionTypes;
	Memory::ObjectAllocatorPtr g_cdoAllocator;

	// Engine reflection registration can run during static initialization.
	// Process-lifetime storage avoids cross-translation-unit initialization and teardown ordering.
	TMap<std::string, WorkspaceTypeEntryPtr>& GetWorkspaceTypes()
	{
		static auto* workspaceTypes = new TMap<std::string, WorkspaceTypeEntryPtr>();
		return *workspaceTypes;
	}

	std::shared_mutex& GetWorkspaceTypesMutex()
	{
		static auto* workspaceTypesMutex = new std::shared_mutex();
		return *workspaceTypesMutex;
	}
}

ComponentPtr Reflection::CreateCDO(const TypeInfo& pType)
{
	std::string typeName = pType.Name();

	static std::once_flag s_once{};

	std::call_once(s_once, [&]() {
		if (!Internal::g_pCdos)
		{
			Internal::g_pCdos = TUniquePtr<TConcurrentMap<std::string, ReflectedData, 32u, ERehashPolicy::Never>>::Make(128);
			Internal::g_cdoAllocator = Memory::ObjectAllocatorPtr::Make(Memory::EAllocationPolicy::SharedMemory_MultiThreaded);
		}});

	check(Internal::g_pCdos && !Internal::g_pCdos->ContainsKey(typeName));

	auto cdo = CreateObject<Component>(pType, Internal::g_cdoAllocator);

	return cdo;
}

void Reflection::StoreCDO(const std::string& typeName, ReflectedData&& reflectedCdo)
{
	auto& cdoInfo = Internal::g_pCdos->At_Lock(typeName);
	cdoInfo = std::move(reflectedCdo);
	Internal::g_pCdos->Unlock(typeName);
}

void Reflection::RegisterFactoryMethod(const TypeInfo& type, TPlacementFactoryMethod placementNew)
{
	if (IsEngineAutoRegistrationSuppressed())
	{
		return;
	}

	static std::once_flag s_once{};

	std::call_once(s_once, [&]() {
		if (!Internal::g_pPlacementFactoryMethods)
		{
			Internal::g_pPlacementFactoryMethods = TUniquePtr<TConcurrentMap<std::string, Reflection::TPlacementFactoryMethod>>::Make();
		}});

	check(Internal::g_pPlacementFactoryMethods && !Internal::g_pPlacementFactoryMethods->ContainsKey(type.Name()));

		auto& method = Internal::g_pPlacementFactoryMethods->At_Lock(type.Name());
		method = placementNew;
		Internal::g_pPlacementFactoryMethods->Unlock(type.Name());
}

void Reflection::RegisterType(const std::string& typeName, const TypeInfo* pType)
{
	if (IsEngineAutoRegistrationSuppressed())
	{
		return;
	}

	static std::once_flag s_once{};

	std::call_once(s_once, [&]() {
		if (!Internal::g_pReflectionTypes)
		{
			Internal::g_pReflectionTypes = TUniquePtr<TConcurrentMap<std::string, const TypeInfo*>>::Make();
		}});

	check(Internal::g_pReflectionTypes && !Internal::g_pReflectionTypes->ContainsKey(typeName));

	auto& type = Internal::g_pReflectionTypes->At_Lock(typeName);
	type = pType;
	Internal::g_pReflectionTypes->Unlock(typeName);
}

void Reflection::SetEngineAutoRegistrationSuppressed(bool suppressed)
{
	Internal::g_bSuppressEngineAutoRegistration = suppressed;
}

bool Reflection::IsEngineAutoRegistrationSuppressed()
{
	return Internal::g_bSuppressEngineAutoRegistration;
}

bool Reflection::RegisterWorkspaceTypes(
	const std::string& owner,
	TVector<WorkspaceTypeRegistration>&& registrations,
	std::string& outError)
{
	outError.clear();
	if (owner.empty())
	{
		outError = "Workspace type owner must not be empty.";
		return false;
	}

	std::unique_lock lock(Internal::GetWorkspaceTypesMutex());
	for (const auto& pair : Internal::GetWorkspaceTypes())
	{
		if ((*pair.m_second)->m_owner == owner)
		{
			outError = "Workspace owner '" + owner + "' is already registered.";
			return false;
		}
	}

	TSet<std::string> incomingTypeNames;
	for (const WorkspaceTypeRegistration& registration : registrations)
	{
		if (registration.m_typeInfo == nullptr || registration.m_typeInfo->Name().empty())
		{
			outError = "Workspace type descriptor is missing TypeInfo.";
			return false;
		}

		const std::string& typeName = registration.m_typeInfo->Name();
		if (!incomingTypeNames.Insert(typeName))
		{
			outError = "Workspace module contains duplicate type '" + typeName + "'.";
			return false;
		}

		if ((Internal::g_pReflectionTypes && Internal::g_pReflectionTypes->ContainsKey(typeName)) ||
			Internal::GetWorkspaceTypes().ContainsKey(typeName))
		{
			outError = "Workspace type '" + typeName + "' conflicts with an existing reflected type.";
			return false;
		}

		if (!registration.m_placementFactory)
		{
			outError = "Workspace type '" + typeName + "' is missing a placement factory.";
			return false;
		}

		if (registration.m_alignment == 0 ||
			(registration.m_alignment & (registration.m_alignment - 1)) != 0)
		{
			outError = "Workspace type '" + typeName + "' has invalid alignment.";
			return false;
		}

		if (!registration.m_defaultObject.IsValid() ||
			&registration.m_defaultObject.GetTypeInfo() != registration.m_typeInfo)
		{
			outError = "Workspace type '" + typeName + "' has invalid default reflection data.";
			return false;
		}
	}

	const auto invocationState = TSharedPtr<Internal::WorkspaceTypeInvocationState>::Make();
	for (WorkspaceTypeRegistration& registration : registrations)
	{
		const std::string typeName = registration.m_typeInfo->Name();
		auto entry = Internal::WorkspaceTypeEntryPtr::Make();
		entry->m_owner = owner;
		entry->m_typeInfo = registration.m_typeInfo;
		entry->m_invocationState = invocationState;
		entry->m_placementFactory = std::move(registration.m_placementFactory);
		entry->m_defaultObject = std::move(registration.m_defaultObject);
		entry->m_alignment = registration.m_alignment;
		Internal::GetWorkspaceTypes().Insert(typeName, std::move(entry));
	}

	return true;
}

size_t Reflection::UnregisterWorkspaceTypes(const std::string& owner)
{
	TSharedPtr<Internal::WorkspaceTypeInvocationState> invocationState;
	size_t numRemoved = 0;
	{
		std::unique_lock lock(Internal::GetWorkspaceTypesMutex());
		for (auto it = Internal::GetWorkspaceTypes().begin(); it != Internal::GetWorkspaceTypes().end();)
		{
			if (it.Value()->m_owner == owner)
			{
				if (!invocationState)
				{
					invocationState = it.Value()->m_invocationState;
					std::lock_guard invocationLock(invocationState->m_mutex);
					invocationState->m_bUnloading = true;
				}
				else
				{
					check(invocationState == it.Value()->m_invocationState);
				}

				const std::string typeName = it.Key();
				++it;
				Internal::GetWorkspaceTypes().Remove(typeName);
				++numRemoved;
			}
			else
			{
				++it;
			}
		}
	}

	if (invocationState)
	{
		std::unique_lock invocationLock(invocationState->m_mutex);
		invocationState->m_conditionVariable.wait(invocationLock, [&invocationState]()
			{
				return invocationState->m_numActiveCalls == 0;
			});
	}

	return numRemoved;
}

bool Reflection::IsWorkspaceTypeRegistered(const std::string& typeName)
{
	std::shared_lock lock(Internal::GetWorkspaceTypesMutex());
	return Internal::GetWorkspaceTypes().ContainsKey(typeName);
}

size_t Reflection::GetNumWorkspaceTypes(const std::string& owner)
{
	std::shared_lock lock(Internal::GetWorkspaceTypesMutex());
	return static_cast<size_t>(std::count_if(
		Internal::GetWorkspaceTypes().begin(),
		Internal::GetWorkspaceTypes().end(),
		[&owner](const auto& pair) { return (*pair.m_second)->m_owner == owner; }));
}

const TypeInfo* Reflection::TryGetTypeByName(const std::string& typeName)
{
	{
		std::shared_lock lock(Internal::GetWorkspaceTypesMutex());
		const auto workspaceType = Internal::GetWorkspaceTypes().Find(typeName);
		if (workspaceType != Internal::GetWorkspaceTypes().end())
		{
			return workspaceType.Value()->m_typeInfo;
		}
	}

	if (Internal::g_pReflectionTypes && Internal::g_pReflectionTypes->ContainsKey(typeName))
	{
		return (*Internal::g_pReflectionTypes)[typeName];
	}

	return nullptr;
}

ReflectedData Reflection::CreateReflectedData(const TypeInfo& type, const YAML::Node& properties)
{
	ReflectedData result;
	result.m_typeInfo = &type;
	if (properties.IsMap())
	{
		for (const auto& property : properties)
		{
			result.m_properties[property.first.as<std::string>()] = property.second;
		}
	}

	return result;
}

const TypeInfo& Reflection::GetTypeByName(const std::string& typeName)
{
	const TypeInfo* type = TryGetTypeByName(typeName);
	check(type != nullptr);
	return *type;
}

const ReflectedData& Reflection::GetCDO(const std::string& typeName)
{
	{
		std::shared_lock lock(Internal::GetWorkspaceTypesMutex());
		const auto workspaceType = Internal::GetWorkspaceTypes().Find(typeName);
		if (workspaceType != Internal::GetWorkspaceTypes().end())
		{
			return workspaceType.Value()->m_defaultObject;
		}
	}

	return (*Internal::g_pCdos)[typeName];
}

size_t Reflection::GetObjectAlignment(const std::string& typeName)
{
	std::shared_lock lock(Internal::GetWorkspaceTypesMutex());
	const auto workspaceType = Internal::GetWorkspaceTypes().Find(typeName);
	return workspaceType != Internal::GetWorkspaceTypes().end() ? workspaceType.Value()->m_alignment : 8;
}

bool Reflection::ConstructObject(const std::string& typeName, void* destination)
{
	// Keep the lease alive until after the copied callable has been destroyed.
	Internal::WorkspaceTypeInvocationLease workspaceInvocation;
	TPlacementFactoryMethod workspacePlacementFactory;
	{
		std::shared_lock lock(Internal::GetWorkspaceTypesMutex());
		const auto workspaceType = Internal::GetWorkspaceTypes().Find(typeName);
		if (workspaceType != Internal::GetWorkspaceTypes().end())
		{
			if (!workspaceInvocation.Acquire(workspaceType.Value()->m_invocationState))
			{
				return false;
			}

			workspacePlacementFactory = workspaceType.Value()->m_placementFactory;
		}
	}

	if (workspacePlacementFactory)
	{
		return workspacePlacementFactory(destination) != nullptr;
	}

	if (Internal::g_pPlacementFactoryMethods && Internal::g_pPlacementFactoryMethods->ContainsKey(typeName))
	{
		return (*Internal::g_pPlacementFactoryMethods)[typeName](destination) != nullptr;
	}

	return false;
}

YAML::Node TypeInfo::Serialize() const
{
	YAML::Node res{};

	::Serialize(res, "typename", m_name);
	::Serialize(res, "base", m_base);
	::Serialize(res, "properties", m_props);

	YAML::Node propertyRanges(YAML::NodeType::Map);
	for (const auto& propertyRange : m_propertyRanges)
	{
		YAML::Node range(YAML::NodeType::Map);
		range["min"] = propertyRange.m_second->m_min;
		range["max"] = propertyRange.m_second->m_max;
		propertyRanges[propertyRange.m_first] = std::move(range);
	}
	res["propertyRanges"] = std::move(propertyRanges);

	return res;
};

void TypeInfo::Deserialize(const YAML::Node& inData)
{
	::Deserialize(inData, "typename", m_name);
	::Deserialize(inData, "base", m_base);
	::Deserialize(inData, "properties", m_props);

	m_propertyRanges.Clear();
	YAML::Node propertyRanges(YAML::NodeType::Undefined);
	for (const auto& field : inData)
	{
		if (field.first.IsScalar() && field.first.as<std::string>() == "propertyRanges")
		{
			propertyRanges = field.second;
			break;
		}
	}
	if (propertyRanges.IsMap())
	{
		for (const auto& propertyRange : propertyRanges)
		{
			const YAML::Node range = propertyRange.second;
			if (!propertyRange.first.IsScalar() || !range.IsMap())
			{
				continue;
			}

			YAML::Node min(YAML::NodeType::Undefined);
			YAML::Node max(YAML::NodeType::Undefined);
			for (const auto& field : range)
			{
				if (!field.first.IsScalar())
				{
					continue;
				}

				const std::string fieldName = field.first.as<std::string>();
				if (fieldName == "min")
				{
					min = field.second;
				}
				else if (fieldName == "max")
				{
					max = field.second;
				}
			}

			if (min.IsScalar() && max.IsScalar())
			{
				m_propertyRanges[propertyRange.first.as<std::string>()] =
					PropertyRange{ min.as<double>(), max.as<double>() };
			}
		}
	}
}

YAML::Node ReflectedData::Serialize() const
{
	assert(m_typeInfo);

	YAML::Node res{};

	::Serialize(res, "typename", m_typeInfo->Name());
	::Serialize(res, "overrideProperties", m_properties);

	return res;
};

void ReflectedData::Deserialize(const YAML::Node& inData)
{
	std::string typeName;

	::Deserialize(inData, "typename", typeName);
	::Deserialize(inData, "overrideProperties", m_properties);

	m_typeInfo = Reflection::TryGetTypeByName(typeName);
}

bool ReflectedData::operator==(const ReflectedData& rhs) const
{
	if (m_typeInfo != rhs.m_typeInfo ||
		m_properties.Num() != rhs.m_properties.Num())
	{
		return false;
	}

	for (const auto& property : m_properties)
	{
		if (!rhs.m_properties.ContainsKey(property.m_first) ||
			!Utils::AreYamlNodesEqual(
				*property.m_second,
				rhs.m_properties[property.m_first]))
		{
			return false;
		}
	}

	return true;
}

TMap<std::string, YAML::Node> ReflectedData::GetOverrideProperties() const
{
	const auto& cdo = Reflection::GetCDO(m_typeInfo->Name());
	return DiffTo(cdo).m_properties;
}

ReflectedData ReflectedData::DiffTo(const ReflectedData& rhs) const
{
	ReflectedData res;

	check(rhs.GetTypeInfo() == GetTypeInfo());

	res.m_typeInfo = &rhs.GetTypeInfo();

	for (const auto& prop : GetProperties())
	{
		if (!rhs.m_properties.ContainsKey(prop.m_first) ||
			!Utils::AreYamlNodesEqual(
				*prop.m_second,
				rhs.m_properties[prop.m_first]))
		{
			res.m_properties[prop.m_first] = *prop.m_second;
		}
	}

	return res;
}

YAML::Node Reflection::ExportEngineTypes()
{
	// Write types
	auto types = Internal::g_pReflectionTypes->GetValues();

	YAML::Node yamlTypes;
	yamlTypes["timeStamp"] = std::time(nullptr);

	TVector<YAML::Node> nodes;
	for (auto& type : types)
	{
		nodes.Add(type->Serialize());
	}

	yamlTypes["engineTypes"] = nodes;

	nodes.Clear();
	for (auto& cdo : Internal::g_pCdos->GetValues())
	{
		YAML::Node yamlCdo;
		yamlCdo["typename"] = cdo.m_typeInfo->Name();
		yamlCdo["defaultValues"] = cdo.GetProperties();

		nodes.Add(yamlCdo);
	}

	yamlTypes["cdos"] = nodes;

	nodes.Clear();
	nodes.Add(ReflectEnumValues<EMobilityType>());
	nodes.Add(ReflectEnumValues<ELightType>());
	nodes.Add(ReflectEnumValues<ELightGlobalIlluminationMode>());
	nodes.Add(ReflectEnumValues<ELightShadowQuality>());
	nodes.Add(ReflectEnumValues<ELightShadowFilter>());
	nodes.Add(ReflectEnumValues<EAnimationPlayMode>());
	nodes.Add(ReflectEnumValues<Physics::ERigidBodyMotionType>());
	nodes.Add(ReflectEnumValues<Physics::ECollisionShapeType>());
	nodes.Add(ReflectEnumValues<RHI::EFormat>());
	nodes.Add(ReflectEnumValues<RHI::ETextureFiltration>());
	nodes.Add(ReflectEnumValues<RHI::ETextureClamping>());
	nodes.Add(ReflectEnumValues<RHI::ESamplerReductionMode>());
	nodes.Add(ReflectEnumValues<RHI::EFillMode>());
	nodes.Add(ReflectEnumValues<RHI::ECullMode>());
	nodes.Add(ReflectEnumValues<RHI::EBlendMode>());
	nodes.Add(ReflectEnumValues<RHI::EDepthCompare>());
	nodes.Add(ReflectEnumValues<RHI::EShadowType>());

	yamlTypes["enums"] = nodes;
	YAML::Node assetTypes(YAML::NodeType::Sequence);
	for (const TypeInfo* type : types)
	{
		YAML::Node asset = type->SerializeAssetType();
		if (asset.IsDefined())
		{
			assetTypes.push_back(asset);
		}
	}
	yamlTypes["assetTypes"] = assetTypes;

	return yamlTypes;
}

bool Utils::TryGetComponentInstanceId(
	const ReflectedData& reflection,
	InstanceId& outInstanceId,
	std::string& outDiagnostic)
{
	outInstanceId = InstanceId::Invalid;
	outDiagnostic.clear();

	if (!reflection.IsValid())
	{
		outDiagnostic = "the reflected component is invalid";
		return false;
	}

	const auto& properties = reflection.GetProperties();
	if (!properties.ContainsKey("instanceId"))
	{
		outDiagnostic = "the reflected component has no instanceId";
		return false;
	}

	const auto& instanceIdNode = properties["instanceId"];
	if (!instanceIdNode.IsScalar())
	{
		outDiagnostic = "the reflected component has an invalid instanceId: expected a scalar value";
		return false;
	}

	InstanceId instanceId;
	std::string conversionDiagnostic;
	if (!External::TryConvertYaml(
			instanceIdNode,
			instanceId,
			conversionDiagnostic))
	{
		outDiagnostic = "the reflected component has an invalid instanceId";
		if (!conversionDiagnostic.empty())
		{
			outDiagnostic += ": " + conversionDiagnostic;
		}
		return false;
	}

	if (instanceId.ComponentId() == InstanceId::Invalid ||
		instanceId.GameObjectId() == InstanceId::Invalid)
	{
		outDiagnostic =
			"the reflected component has an invalid instanceId: "
			"both component and game-object IDs must be valid";
		return false;
	}

	outInstanceId = instanceId;
	return true;
}

ObjectPtr IReflectable::ResolveAssetDependency(const FileId& fileId, bool bImmediate)
{
	return App::GetSubmodule<AssetRegistry>()->LoadAssetFromFile<Object>(fileId, bImmediate);
}

ObjectPtr IReflectable::ResolveExternalDependency(const InstanceId& componentInstanceId, const TMap<InstanceId, ObjectPtr>& resolveContext)
{
	if (auto goInstanceId = componentInstanceId.GameObjectId())
	{
		if (resolveContext.ContainsKey(goInstanceId))
		{
			auto go = resolveContext[goInstanceId].DynamicCast<GameObject>();

			const size_t index = go->GetComponents().FindIf([&](const auto& comp) { return comp->GetInstanceId().ComponentId() == componentInstanceId.ComponentId(); });
			if (index != -1)
			{
				return go->GetComponents()[index];
			}
		}
	}

	return ObjectPtr();
}
