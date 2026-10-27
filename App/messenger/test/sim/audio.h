/* Host stand-in for App/audio.h (the real one pulls in the MCU GPIO driver). */
#ifndef AUDIO_H
#define AUDIO_H

#include <stdbool.h>
#include <stdint.h>

typedef enum BEEP_Type_t {
    BEEP_NONE = 0,
    BEEP_1KHZ_60MS_OPTIONAL,
    BEEP_500HZ_60MS_DOUBLE_BEEP_OPTIONAL,
} BEEP_Type_t;

extern BEEP_Type_t gBeepToPlay;

#endif
