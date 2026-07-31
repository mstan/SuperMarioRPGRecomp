#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "common_cpu_infra.h"

const RtlGameInfo *SmrpgGameInfo(void);
void SmrpgBeginDrawing(uint8_t *pixels, size_t pitch);
void SmrpgDrawPpuFrame(void);
void SmrpgSetWidescreenExtra(int extra);
void SmrpgSetWidescreenHud(bool enabled);
int SmrpgWidescreenWidth(void);
uint32_t SmrpgResumePc(void);
int SmrpgLastLleResult(void);
