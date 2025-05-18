#pragma once

#ifdef _WIN32
#include "Platform/Win32/Input.h"
namespace Sailor::Platform
{
    using InputState = Sailor::Win32::InputState;
    using GlobalInput = Sailor::Win32::GlobalInput;
}
#else
namespace Sailor::Platform
{
    struct InputState {};
    class GlobalInput
    {
    public:
        static const InputState& GetInputState()
        {
            static InputState state{};
            return state;
        }
    };
}
#endif
