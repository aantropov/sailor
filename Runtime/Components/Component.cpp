#include "Components/Component.h"
#include "Engine/GameObject.h"

using namespace Sailor;
using namespace Sailor::Tasks;

WorldPtr Component::GetWorld() const 
{
	return m_owner->GetWorld(); 
}