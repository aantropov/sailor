#include "Components/PathTracerProxyComponent.h"
#include "Engine/GameObject.h"
#include "AssetRegistry/Material/MaterialImporter.h"
#include "Raytracing/PathTracer.h"

using namespace Sailor;

void PathTracerProxyComponent::Initialize()
{
	auto* pEcs = GetOwner()->GetWorld()->GetECS<PathTracerECS>();
	m_handle = pEcs->RegisterComponent();

	auto& data = GetData();
	data.SetOwner(GetOwner());
	data.MarkDirty();
}

void PathTracerProxyComponent::EndPlay()
{
	GetOwner()->GetWorld()->GetECS<PathTracerECS>()->UnregisterComponent(m_handle);
}

PathTracerProxyData& PathTracerProxyComponent::GetData()
{
	auto* pEcs = GetOwner()->GetWorld()->GetECS<PathTracerECS>();
	return pEcs->GetComponentData(m_handle);
}

const PathTracerProxyData& PathTracerProxyComponent::GetData() const
{
	auto* pEcs = GetOwner()->GetWorld()->GetECS<PathTracerECS>();
	return pEcs->GetComponentData(m_handle);
}

void PathTracerProxyComponent::SetEnabled(bool bEnabled)
{
	auto& options = GetData().GetOptions();
	if (options.m_bEnabled != bEnabled)
	{
		options.m_bEnabled = bEnabled;
		GetData().MarkDirty();
	}
}

void PathTracerProxyComponent::SetRebuildEveryFrame(bool bValue)
{
	auto& data = GetData();
	auto& options = data.GetOptions();
	if (options.m_bRebuildEveryFrame != bValue)
	{
		options.m_bRebuildEveryFrame = bValue;
		data.MarkDirty();
	}
}

