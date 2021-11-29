#pragma once
#include <string>
#include <vector>
#include "Core/Submodule.h"
#include "RHI/Mesh.h"

namespace Sailor
{
	class DefaultAssets final : public TSubmodule<DefaultAssets>
	{
	public:

		// TODO: Change mesh to model
		//ModelPtr GetDefaultSphere() const { return m_defaultSphere; }
		//ModelPtr GetDefaultCube() const { return m_defaultCube; }

		SAILOR_API DefaultAssets();
		virtual SAILOR_API ~DefaultAssets() override;

	protected:

		//ModelPtr m_defaultSphere;
		//ModelPtr m_defaultCube;
	};
}
