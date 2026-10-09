#pragma once
#include "Components/Component.h"

namespace Sailor
{
	// Preserves missing workspace component data until its module is rebuilt.
	class UnknownComponent final : public Component
	{
	public:
		ReflectedData GetReflectedData() const override
		{
			auto document = YAML::Clone(m_data.Serialize());
			document["overrideProperties"]["instanceId"] = GetInstanceId();
			ReflectedData result;
			result.Deserialize(document);
			return result;
		}

		void ApplyReflection(const ReflectedData& data) override { m_data = data; }
		bool ResolveRefs(const ReflectedData&, const TMap<InstanceId, ObjectPtr>&, bool) override { return true; }

	private:
		ReflectedData m_data;
	};
}
