#pragma once
#include <memory>
#include <rex/system/interfaces/graphics.h>
std::unique_ptr<rex::system::IGraphicsSystem> CreateDaytonaNativeGraphics();
void WaitForDaytonaNativeInputs();
