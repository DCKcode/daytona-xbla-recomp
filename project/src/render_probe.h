#pragma once

#include <memory>
#include <rex/system/interfaces/graphics.h>

namespace rex::input {
class InputSystem;
}

std::unique_ptr<rex::system::IGraphicsSystem> CreateDaytonaSdkGraphics();
void InstallDaytonaProbeInput(rex::input::InputSystem &input);
