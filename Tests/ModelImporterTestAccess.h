#pragma once
#include "AssetRegistry/Model/ModelImporter.h"

namespace Sailor
{
	class ModelImporterTestAccess
	{
	public:
		static void BeforeCpuPreparation(ModelImporter& importer, std::function<void()> callback)
		{
			importer.m_beforeCpuPreparationForTests = std::move(callback);
		}
	};
}
