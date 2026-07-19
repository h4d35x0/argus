#pragma once
#include <lvgl.h>

void lora_screen_create();
void lora_screen_restore_power();
void lora_screen_show();
bool lora_screen_is_active();
bool lora_screen_is_powered();
