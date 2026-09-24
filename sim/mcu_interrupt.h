#pragma once
enum { INTERRUPT_SOURCE_IRQ0 = 0 };
void MCU_Interrupt_SetRequest(int source, int value);
