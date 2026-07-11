#include "Components/Component.h"
#include "Engine/GameObject.h"

using namespace Sailor;
using namespace Sailor::Tasks;

template<>
SAILOR_SHARED_API const TypeInfo& TypeInfo::Get<Component>()
{
	static const TypeInfo ti(refl::reflect<Component>());
	return ti;
}

WorldPtr Component::GetWorld() const 
{
	return m_owner->GetWorld(); 
}
