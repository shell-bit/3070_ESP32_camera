#pragma once
#include "clock_state.h"

namespace lcd {
void viewerClockBegin();
void viewerClockLoop(bool wifi);
ClockSnapshot viewerClockSnapshot();
}
