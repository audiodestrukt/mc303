// Shim for Nuked-SC55's mcu.h: only what pcm.cpp needs. The MC-303 board supplies these.
#pragma once
#include <stdint.h>
extern int mcu_mk1;
extern int mcu_jv880;
void MCU_PostSample(int *sample);
void MCU_GA_SetGAInt(int line, int value);
