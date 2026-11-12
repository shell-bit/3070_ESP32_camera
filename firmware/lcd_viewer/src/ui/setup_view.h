#pragma once

#include "../clock/clock_state.h"
#include <cstdint>

namespace lcd {

enum class SetupExit { Home, StartPreview };
using SetupExitHandler = void (*)(SetupExit);

void setupViewBegin(SetupExitHandler handler);
void setupViewOpen(uint32_t now, bool configured);
void setupViewLoop(uint32_t now, const ClockSnapshot& clock);
void setupViewBack(uint32_t now);
bool setupViewVisible();
bool setupViewOwnsDraft();
bool setupViewScanBusy();

}
