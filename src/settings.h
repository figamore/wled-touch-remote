#pragma once

#include "app_state.h"

const char* idleModeName(IdleMode mode);
IdleMode nextIdleMode(IdleMode mode);

void loadSettings();
void saveSettings();
void markInfoTabSeen();
