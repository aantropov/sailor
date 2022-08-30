#pragma once
#include "Core/Defines.h"
#include "Memory/RefPtr.hpp"
#include "Engine/Object.h"
#include "RHI/Types.h"
#include "BaseFrameGraphNode.h"
#include "FrameGraph/RHIFrameGraph.h"
#include "AssetRegistry/FrameGraph/FrameGraphImporter.h"

namespace Sailor::Framegraph
{
	class SAILOR_API FrameGraphBuilder : public TSubmodule<FrameGraphBuilder>
	{
	public:

		static void RegisterFrameGraphNode(const std::string& nodeName, std::function<FrameGraphNodePtr(void)> factoryMethod);

		FrameGraphNodePtr CreateNode(const std::string& nodeName) const;
	};

	template<typename TRenderNode>
	class TFrameGraphNode : public BaseFrameGraphNode
	{
	public:

		SAILOR_API TFrameGraphNode() { TFrameGraphNode::s_registrationFactoryMethod; }
		SAILOR_API static const char* GetName() { return TRenderNode::GetName(); }

	protected:

		class SAILOR_API RegistrationFactoryMethod
		{
		public:

			RegistrationFactoryMethod()
			{
				if (!s_bRegistered)
				{
					FrameGraphBuilder::RegisterFrameGraphNode(std::string(TRenderNode::GetName()), []() { return TRefPtr<TRenderNode>::Make(); });
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
	TFrameGraphNode<T>::RegistrationFactoryMethod volatile TFrameGraphNode<T>::s_registrationFactoryMethod;

	template<typename T>
	bool TFrameGraphNode<T>::RegistrationFactoryMethod::s_bRegistered = false;
#endif

	class RHINodeDefault : public TFrameGraphNode<RHINodeDefault>
	{
	public:
		SAILOR_API static const char* GetName() { return m_name; }

		SAILOR_API virtual void Process(RHI::RHIFrameGraph* frameGraph, RHI::RHICommandListPtr transferCommandList, RHI::RHICommandListPtr commandList, const RHI::RHISceneViewSnapshot& sceneView) override {}
		SAILOR_API virtual void Clear() override {}
	
	protected:

		static const char* m_name;
	};

	template class TFrameGraphNode<RHINodeDefault>;
};
