#pragma once
#include "Core/Defines.h"
#include "Memory/RefPtr.hpp"
#include "Engine/Object.h"
#include "RHI/Types.h"
#include "BaseFrameGraphNode.h"
#include "FrameGraph/RHIFrameGraph.h"
#include "Tasks/Tasks.h"
#include "AssetRegistry/FrameGraph/FrameGraphImporter.h"

namespace Sailor::Framegraph
{
	class SAILOR_API FrameGraphBuilder : public TSubmodule<FrameGraphBuilder>
	{
	public:

		static void RegisterFrameGraphNode(StringHash nodeName, std::function<FrameGraphNodePtr(void)> factoryMethod);

		FrameGraphNodePtr CreateNode(StringHash nodeName) const;
	};

	template<typename TRenderNode>
	class TFrameGraphNode : public BaseFrameGraphNode
	{
	public:

		SAILOR_API TFrameGraphNode() 
		{ 
			auto res = TFrameGraphNode::s_registrationFactoryMethod; 
			res.DoWork();
		}
		SAILOR_API static StringHash GetName() { return TRenderNode::GetName(); }
		SAILOR_API virtual std::string_view GetDebugName() const { return TRenderNode::GetName().ToString(); }

	protected:

		class SAILOR_SHARED_API RegistrationFactoryMethod
		{
		public:

			RegistrationFactoryMethod()
			{
				if (!s_bRegistered)
				{
					FrameGraphBuilder::RegisterFrameGraphNode(TRenderNode::GetName(), []() { return TRefPtr<TRenderNode>::Make(); });
					s_bRegistered = true;
				}
			}

			// We meed that to supress warning and hint compiler not to omit the code
			void DoWork() {}

		protected:

			static bool s_bRegistered;
		};

		SAILOR_SHARED_API static RegistrationFactoryMethod s_registrationFactoryMethod;
	};

#ifndef _SAILOR_IMPORT_
	template<typename T>
	typename TFrameGraphNode<T>::RegistrationFactoryMethod TFrameGraphNode<T>::s_registrationFactoryMethod;

	template<typename T>
	bool TFrameGraphNode<T>::RegistrationFactoryMethod::s_bRegistered = false;
#endif

	class RHINodeDefault : public TFrameGraphNode<RHINodeDefault>
	{
	public:
		SAILOR_API static StringHash GetName() { return "untitled"_h; }

		SAILOR_API virtual void Process(RHI::RHIFrameGraphPtr frameGraph, RHI::RHICommandListPtr transferCommandList, RHI::RHICommandListPtr commandList, const RHI::RHISceneViewSnapshot& sceneView) override {}
		SAILOR_API virtual void Clear() override {}
	};

#ifndef _SAILOR_IMPORT_
	template class TFrameGraphNode<RHINodeDefault>;
#else
	extern template class TFrameGraphNode<RHINodeDefault>;
#endif
};
