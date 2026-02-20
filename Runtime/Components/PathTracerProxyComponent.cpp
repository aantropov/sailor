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

ModelPtr PathTracerProxyComponent::GetModel() const
{
	if (auto pMeshRenderer = GetOwner()->GetComponent<MeshRendererComponent>())
	{
		return pMeshRenderer->GetModel();
	}

	return ModelPtr();
}

void PathTracerProxyComponent::SetModel(const ModelPtr& pModel)
{
	auto& data = GetData();
	auto pMeshRenderer = GetOwner()->GetComponent<MeshRendererComponent>();
	if (!pMeshRenderer)
	{
		pMeshRenderer = GetOwner()->AddComponent<MeshRendererComponent>();
	}

	if (pMeshRenderer)
	{
		pMeshRenderer->SetModel(pModel);
	}

	TVector<MaterialPtr> materials;
	TVector<TMap<std::string, TexturePtr>> textureBindings;

	if (pMeshRenderer)
	{
		materials = pMeshRenderer->GetMaterials();
	}

	textureBindings.Resize(materials.Num());
	for (size_t i = 0; i < materials.Num(); i++)
	{
		const MaterialPtr& pMaterial = materials[i];
		if (!pMaterial)
		{
			continue;
		}

		auto& bindings = textureBindings[i];
		for (const auto& sampler : pMaterial->GetSamplers())
		{
			bindings.Add(sampler.m_first, sampler.m_second);
		}
	}

	data.SetMaterials(std::move(materials), std::move(textureBindings));
}

void PathTracerProxyComponent::LoadModel(const std::string& path)
{
	auto pMeshRenderer = GetOwner()->GetComponent<MeshRendererComponent>();
	if (!pMeshRenderer)
	{
		pMeshRenderer = GetOwner()->AddComponent<MeshRendererComponent>();
	}

	if (pMeshRenderer)
	{
		pMeshRenderer->LoadModel(path);
		SetModel(pMeshRenderer->GetModel());
	}
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

void PathTracerProxyComponent::SetMaxBounces(uint32_t value)
{
	auto& data = GetData();
	auto& options = data.GetOptions();
	if (options.m_maxBounces != value)
	{
		options.m_maxBounces = value;
		data.MarkDirty();
	}
}

void PathTracerProxyComponent::SetSamplesPerPixel(uint32_t value)
{
	auto& data = GetData();
	auto& options = data.GetOptions();
	if (options.m_samplesPerPixel != value)
	{
		options.m_samplesPerPixel = value;
		data.MarkDirty();
	}
}

bool PathTracerProxyComponent::InitializePathTracer(Raytracing::PathTracer& outPathTracer)
{
	auto* pEcs = GetOwner()->GetWorld()->GetECS<PathTracerECS>();
	return pEcs ? pEcs->InitializePathTracer(outPathTracer, m_handle) : false;
}

bool PathTracerProxyComponent::RenderScene(const Raytracing::PathTracer::Params& params)
{
	auto* pEcs = GetOwner()->GetWorld()->GetECS<PathTracerECS>();
	return pEcs ? pEcs->RenderScene(params, m_handle) : false;
}

void PathTracerProxyComponent::SetPathTracingEnabled(bool bEnabled)
{
	if (auto* pEcs = GetOwner()->GetWorld()->GetECS<PathTracerECS>())
	{
		pEcs->SetPathTracingEnabled(bEnabled);
	}
}

bool PathTracerProxyComponent::IsPathTracingEnabled() const
{
	auto* pEcs = GetOwner()->GetWorld()->GetECS<PathTracerECS>();
	return pEcs ? pEcs->IsPathTracingEnabled() : false;
}

