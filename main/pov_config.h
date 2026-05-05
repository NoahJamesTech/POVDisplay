#ifndef POV_CONFIG_H
#define POV_CONFIG_H

#include <stdint.h>

#define POV_GLOBAL_COLS 720
#define POV_GLOBAL_LEDS 36
#define POV_GLOBAL_ROTATION_PERIOD_US 120000
#define POV_PIXEL_BYTES 4

#define DOTSTAR_DATA_GPIO  1 
#define DOTSTAR_CLK_GPIO   2 
#define BLADE_LEDS          (DOTSTAR_NUM_LEDS / 2)   // 36

#endif
