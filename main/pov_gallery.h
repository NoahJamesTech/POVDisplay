#ifndef POV_GALLERY_H
#define POV_GALLERY_H

#include <stdint.h>
#include "pov_config.h"

typedef struct {
    const char *name;
    const uint8_t (*data)[POV_GLOBAL_LEDS][4];
} pov_image_entry_t;

#define POV_IMAGE_COUNT 0

static const pov_image_entry_t pov_images[1] = {
    {"", 0}
};

#endif
