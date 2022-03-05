#include "Components/Component.h"
#include "Engine/GameObject.h"

using namespace Sailor;
using namespace Sailor::JobSystem;

WorldPtr Component::GetWorld() const 
{
	return m_owner->GetWorld(); 
}