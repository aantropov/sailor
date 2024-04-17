#pragma once
#include "Memory/ObjectPtr.hpp"
#include "AssetRegistry/FileId.h"

namespace Sailor
{
	class IAssetFactory
	{
	public:

		virtual bool LoadAsset(FileId uid, TObjectPtr<Object>& out, bool bImmediate = true) = 0;
	};
}
