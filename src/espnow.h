#pragma once

#include "app_state.h"

void requestStatus(StatusCode code);
void applyPendingStatus();

void sendWledTouchButton(uint8_t button_code);
void sendPower(bool power);
void sendBrightnessDelta(int delta);
void sendPreset(uint8_t preset);

void initEspNow();
