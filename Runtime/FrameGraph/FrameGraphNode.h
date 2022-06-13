#pragma once
#include "Core/Defines.h"
#include "Memory/RefPtr.hpp"
#include "Engine/Object.h"
#include "RHI/Types.h"
#include "BaseFrameGraphNode.h"
#include "FrameGraph/RHIFrameGraph.h"
#include "AssetRegistry/FrameGraph/FrameGraphImporter.h"

namespace Sailor
{
	template<typename TRenderNode>
	class TFrameGraphNode : public BaseFrameGraphNode
	{
	public:

		TFrameGraphNode() { TFrameGraphNode::s_registrationFactoryMethod; }
		static const std::string& GetName() { return TRenderNode::GetName(); }

	protected:

		class SAILOR_API RegistrationFactoryMethod
		{
		public:

			RegistrationFactoryMethod()
			{
				if (!s_bRegistered)
				{
					FrameGraphImporter::RegisterFrameGraphNode(TRenderNode::GetName(), []() { return TRefPtr<TRenderNode>::Make(); });
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
		static const char* GetName() { return "untitled"; }

		virtual void Initialize(FrameGraphPtr FrameGraph) {}
		virtual void Process() {}
		virtual void Clear() {}
	};
};
