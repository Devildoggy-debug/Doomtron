// ILI9341 (320x240, SPI) driver for RP2040 - tuned for Doom's 320x200 8-bit paletted output.
// Uses hardware SPI + DMA. Pixel data is converted palette->RGB565 in small chunks while the
// previous chunk is still being sent, so CPU conversion overlaps with the SPI transfer.
#pragma once
#include <stdint.h>
#include <stdbool.h>

// ---- Pin / bus configuration (override with -D on the compiler command line) ----
#ifndef ILI9341_SPI
#define ILI9341_SPI      spi0
#endif
#ifndef ILI9341_PIN_SCK
#define ILI9341_PIN_SCK  18
#endif
#ifndef ILI9341_PIN_MOSI
#define ILI9341_PIN_MOSI 19
#endif
#ifndef ILI9341_PIN_CS
#define ILI9341_PIN_CS   17
#endif
#ifndef ILI9341_PIN_DC
#define ILI9341_PIN_DC   20
#endif
#ifndef ILI9341_PIN_RST
#define ILI9341_PIN_RST  21
#endif
#ifndef ILI9341_PIN_BL            // backlight; set to -1 if wired straight to 3V3
#define ILI9341_PIN_BL   22
#endif
#ifndef ILI9341_BAUD_HZ           // 40 MHz is a safe start; many panels run at 62.5 MHz
#define ILI9341_BAUD_HZ  (40u * 1000u * 1000u)
#endif

#define ILI9341_WIDTH    320
#define ILI9341_HEIGHT   240
#define DOOM_WIDTH       320
#define DOOM_HEIGHT      200
#define DOOM_Y_OFFSET    ((ILI9341_HEIGHT - DOOM_HEIGHT) / 2)   // letterbox: 20px bars top/bottom

// Initialise SPI, GPIOs, DMA and the panel. Returns the actual SPI baud rate achieved.
uint32_t ili9341_init(void);

// Set the 256-entry palette from Doom's PLAYPAL format: 256 * (R,G,B), 8 bits each.
void ili9341_set_palette(const uint8_t rgb[256 * 3]);

// Draw `rows` rows of 8-bit indexed pixels (DOOM_WIDTH bytes per row) starting at Doom row `y`
// (0..199). Returns quickly: the last chunk is left transferring via DMA.
void ili9341_write_rows(int y, int rows, const uint8_t *indexed);

// Convenience: draw a full 320x200 frame.
static inline void ili9341_write_frame(const uint8_t *indexed) {
    ili9341_write_rows(0, DOOM_HEIGHT, indexed);
}

// Fill the entire panel with one RGB565 colour (blocking).
void ili9341_fill(uint16_t rgb565);

// Block until any in-flight DMA transfer has fully left the SPI FIFO.
void ili9341_wait_idle(void);
