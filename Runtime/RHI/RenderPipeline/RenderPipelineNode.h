#pragma once
#include "Core/Defines.h"
#include "Memory/RefPtr.hpp"
#include "Engine/Object.h"
#include "RHI/Types.h"
#include "BaseRenderPipelineNode.h"
#include "RHI/RenderPipeline/RenderPipeline.h"
#include "AssetRegistry/RenderPipeline/RenderPipelineImporter.h"

namespace Sailor::RHI
{
	template<typename TRenderNode>
	class TRHIRenderPipelineNode : public IBaseRenderPipelineNode
	{
	public:

		TRHIRenderPipelineNode() { TRHIRenderPipelineNode::s_registrationFactoryMethod; }
		static const std::string& GetName() { return TRenderNode::GetName(); }

	protected:

		class SAILOR_API RegistrationFactoryMethod
		{
		public:

			RegistrationFactoryMethod()
			{
				if (!s_bRegistered)
				{
					RenderPipelineImporter::RegisterRenderPipelineNode(TRenderNode::GetName(), []() { return TRefPtr<TRenderNode>::Make(); });
					s_bRegistered = true;
				}
			}

		protected:

			static bool s_bRegistered;
		};

		static volatile RegistrationFactoryMethod s_registrationFactoryMethod;
	};

#ifndef _SAILOR_IMPORT_
	template<typename T>
	TRHIRenderPipelineNode<T>::RegistrationFactoryMethod volatile TRHIRenderPipelineNode<T>::s_registrationFactoryMethod;

	template<typename T>
	bool TRHIRenderPipelineNode<T>::RegistrationFactoryMethod::s_bRegistered = false;
#endif

	class RHINodeDefault : public TRHIRenderPipelineNode<RHINodeDefault>
	{
	public:
		static const char* GetName() { return "untitled"; }

		virtual void Initialize(RHIRenderPipelinePtr renderPipeline) {}
		virtual void Process() {}
		virtual void Clear() {}
	};
};
