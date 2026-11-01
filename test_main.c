// Standalone wiring/driver test: draws a palette gradient + moving bar at 320x200.
// Build this first; once you see a clean picture, integrate the driver into rp2040-doom.
#include <stdio.h>
#include "pico/stdlib.h"
#include "ili9341.h"

static uint8_t frame[DOOM_WIDTH * DOOM_HEIGHT];

int main(void) {
    stdio_init_all();
    uint32_t baud = ili9341_init();
    printf("ILI9341 SPI baud: %u Hz\n", baud);

    uint8_t pal[256 * 3];
    for (int i = 0; i < 256; i++) {            // simple blue->red->yellow ramp
        pal[i*3+0] = i;
        pal[i*3+1] = i > 128 ? (i - 128) * 2 : 0;
        pal[i*3+2] = 255 - i;
    }
    ili9341_set_palette(pal);

    uint32_t t = 0;
    while (true) {
        for (int y = 0; y < DOOM_HEIGHT; y++)
            for (int x = 0; x < DOOM_WIDTH; x++)
                frame[y * DOOM_WIDTH + x] = (x * 255 / (DOOM_WIDTH - 1));
        int bar = (t * 4) % DOOM_WIDTH;
        for (int y = 0; y < DOOM_HEIGHT; y++)
            for (int x = bar; x < bar + 8 && x < DOOM_WIDTH; x++) frame[y * DOOM_WIDTH + x] = 255;

        absolute_time_t t0 = get_absolute_time();
        ili9341_write_frame(frame);
        ili9341_wait_idle();
        if ((t++ % 60) == 0)
            printf("frame push: %lld us\n", absolute_time_diff_us(t0, get_absolute_time()));
    }
}
