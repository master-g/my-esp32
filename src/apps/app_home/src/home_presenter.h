#ifndef APP_HOME_HOME_PRESENTER_H
#define APP_HOME_HOME_PRESENTER_H

#include <stdbool.h>
#include <stdint.h>

#include "service_home.h"

typedef enum {
    SPRITE_STATE_IDLE = 0,
    SPRITE_STATE_WORKING,
    SPRITE_STATE_WAITING,
    SPRITE_STATE_COMPACTING,
    SPRITE_STATE_SLEEPING,
    SPRITE_STATE_COUNT,
} sprite_state_t;

typedef enum {
    SPRITE_EMOTION_NEUTRAL = 0,
    SPRITE_EMOTION_HAPPY,
    SPRITE_EMOTION_SAD,
    SPRITE_EMOTION_SOB,
    SPRITE_EMOTION_COUNT,
} sprite_emotion_t;

typedef struct {
    char time_text[9];
    char date_line[48];
    const char *wifi_symbol;
    uint32_t wifi_color;
    bool wifi_dot_visible;
    uint32_t claude_color;
    bool claude_dot_visible;
    const char *weather_symbol;
    uint32_t weather_color;
    char weather_text[64];
    sprite_state_t sprite_state;
    sprite_emotion_t sprite_emotion;
    bool bubble_visible;
    char bubble_text[96];
    char screensaver_time_text[6];
} home_present_model_t;

void home_presenter_build(home_present_model_t *out, const home_snapshot_t *snapshot);

#endif
