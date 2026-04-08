#ifndef WEATHER_MAPPER_H
#define WEATHER_MAPPER_H

#include <stdbool.h>
#include <stdint.h>

const char *weather_mapper_text_for_code(uint16_t weather_code, bool is_day);
uint8_t weather_mapper_icon_for_code(uint16_t weather_code, bool is_day);

#endif
