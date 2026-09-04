#pragma once
// Host shim for the tiny slice of the Arduino core the ARGUS screen code uses.
// SIM ONLY. Nothing here is compiled into firmware.
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <math.h>

uint32_t millis(void);
uint32_t micros(void);
void     delay(uint32_t ms);

#ifndef LOW
#define LOW  0
#define HIGH 1
#endif
