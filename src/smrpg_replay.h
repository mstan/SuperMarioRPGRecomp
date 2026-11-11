#pragma once
#include <stdbool.h>
#include <stdint.h>
void SmrpgReplayEnable(bool enabled);
void SmrpgReplayRom(const uint8_t *rom, unsigned size);
bool SmrpgReplayPlayback(const char *path);
bool SmrpgReplayBegin(uint32_t *input, int *width, bool custom);
void SmrpgReplayEnd(void);
void SmrpgReplayShutdown(void);
bool SmrpgReplayFailed(void);
bool SmrpgReplayPlaying(void);
