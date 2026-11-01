#include "ili9341.h"
#include <string.h>
#include "pico/stdlib.h"
#include "hardware/spi.h"
#include "hardware/gpio.h"
#include "hardware/dma.h"

#define CHUNK_ROWS 8                                   // rows converted+sent per DMA transfer
#define CHUNK_PIXELS (DOOM_WIDTH * CHUNK_ROWS)

// Palette stored already byte-swapped (big-endian RGB565) so DMA can stream bytes directly.
static uint16_t palette_be[256];
// Two chunk buffers for ping-pong (2 * 5120 bytes).
static uint16_t chunk_buf[2][CHUNK_PIXELS];
static int cur_buf;
static int dma_ch = -1;
static bool cs_asserted;

static inline void cs_low(void)  { gpio_put(ILI9341_PIN_CS, 0); cs_asserted = true; }
static inline void cs_high(void) { gpio_put(ILI9341_PIN_CS, 1); cs_asserted = false; }

void ili9341_wait_idle(void) {
    if (dma_ch >= 0) dma_channel_wait_for_finish_blocking(dma_ch);
    // DMA done != SPI done: wait for the TX FIFO / shift register to drain.
    while (spi_get_hw(ILI9341_SPI)->sr & SPI_SSPSR_BSY_BITS) tight_loop_contents();
    if (cs_asserted) cs_high();
}

static void write_cmd(uint8_t cmd, const uint8_t *data, size_t len) {
    ili9341_wait_idle();
    gpio_put(ILI9341_PIN_DC, 0);
    cs_low();
    spi_write_blocking(ILI9341_SPI, &cmd, 1);
    if (len) {
        gpio_put(ILI9341_PIN_DC, 1);
        spi_write_blocking(ILI9341_SPI, data, len);
    }
    cs_high();
}

#define CMD(c, ...) do { const uint8_t d_[] = { __VA_ARGS__ }; write_cmd((c), d_, sizeof d_); } while (0)

static void set_window(int x0, int y0, int x1, int y1) {
    CMD(0x2A, x0 >> 8, x0 & 0xFF, x1 >> 8, x1 & 0xFF);   // CASET
    CMD(0x2B, y0 >> 8, y0 & 0xFF, y1 >> 8, y1 & 0xFF);   // PASET
}

// Begin RAMWR and stream `bytes` bytes from `src` by DMA. Does not wait for completion.
static void start_pixel_dma(const void *src, size_t bytes) {
    uint8_t ramwr = 0x2C;
    gpio_put(ILI9341_PIN_DC, 0);
    cs_low();
    spi_write_blocking(ILI9341_SPI, &ramwr, 1);
    gpio_put(ILI9341_PIN_DC, 1);

    dma_channel_config c = dma_channel_get_default_config(dma_ch);
    channel_config_set_transfer_data_size(&c, DMA_SIZE_8);
    channel_config_set_dreq(&c, spi_get_dreq(ILI9341_SPI, true));
    channel_config_set_read_increment(&c, true);
    channel_config_set_write_increment(&c, false);
    dma_channel_configure(dma_ch, &c, &spi_get_hw(ILI9341_SPI)->dr, src, bytes, true);
}

uint32_t ili9341_init(void) {
    uint32_t baud = spi_init(ILI9341_SPI, ILI9341_BAUD_HZ);
    spi_set_format(ILI9341_SPI, 8, SPI_CPOL_0, SPI_CPHA_0, SPI_MSB_FIRST);
    gpio_set_function(ILI9341_PIN_SCK,  GPIO_FUNC_SPI);
    gpio_set_function(ILI9341_PIN_MOSI, GPIO_FUNC_SPI);

    gpio_init(ILI9341_PIN_CS);  gpio_set_dir(ILI9341_PIN_CS,  GPIO_OUT); cs_high();
    gpio_init(ILI9341_PIN_DC);  gpio_set_dir(ILI9341_PIN_DC,  GPIO_OUT);
    gpio_init(ILI9341_PIN_RST); gpio_set_dir(ILI9341_PIN_RST, GPIO_OUT);
#if ILI9341_PIN_BL >= 0
    gpio_init(ILI9341_PIN_BL);  gpio_set_dir(ILI9341_PIN_BL,  GPIO_OUT); gpio_put(ILI9341_PIN_BL, 0);
#endif

    // Claim a DMA channel without clashing with others (Doom's video code may use some too).
    dma_ch = dma_claim_unused_channel(true);

    // Hardware reset
    gpio_put(ILI9341_PIN_RST, 1); sleep_ms(5);
    gpio_put(ILI9341_PIN_RST, 0); sleep_ms(20);
    gpio_put(ILI9341_PIN_RST, 1); sleep_ms(150);

    write_cmd(0x01, NULL, 0); sleep_ms(150);                         // software reset
    CMD(0xCF, 0x00, 0xC1, 0x30);                                     // power control B
    CMD(0xED, 0x64, 0x03, 0x12, 0x81);                               // power on sequence
    CMD(0xE8, 0x85, 0x00, 0x78);                                     // driver timing A
    CMD(0xCB, 0x39, 0x2C, 0x00, 0x34, 0x02);                         // power control A
    CMD(0xF7, 0x20);                                                 // pump ratio
    CMD(0xEA, 0x00, 0x00);                                           // driver timing B
    CMD(0xC0, 0x23);                                                 // power control 1
    CMD(0xC1, 0x10);                                                 // power control 2
    CMD(0xC5, 0x3E, 0x28);                                           // VCOM 1
    CMD(0xC7, 0x86);                                                 // VCOM 2
    CMD(0x36, 0x28);                                                 // MADCTL: landscape, BGR  (see README if flipped/mirrored)
    CMD(0x3A, 0x55);                                                 // 16 bpp
    CMD(0xB1, 0x00, 0x18);                                           // frame rate ~79 Hz
    CMD(0xB6, 0x08, 0x82, 0x27);                                     // display function control
    CMD(0xF2, 0x00);                                                 // 3-gamma off
    CMD(0x26, 0x01);                                                 // gamma curve 1
    CMD(0xE0, 0x0F,0x31,0x2B,0x0C,0x0E,0x08,0x4E,0xF1,0x37,0x07,0x10,0x03,0x0E,0x09,0x00);
    CMD(0xE1, 0x00,0x0E,0x14,0x03,0x11,0x07,0x31,0xC1,0x48,0x08,0x0F,0x0C,0x31,0x36,0x0F);
    write_cmd(0x11, NULL, 0); sleep_ms(120);                         // sleep out
    write_cmd(0x29, NULL, 0); sleep_ms(20);                          // display on

    ili9341_fill(0x0000);
#if ILI9341_PIN_BL >= 0
    gpio_put(ILI9341_PIN_BL, 1);
#endif
    return baud;
}

void ili9341_set_palette(const uint8_t rgb[256 * 3]) {
    for (int i = 0; i < 256; i++) {
        uint16_t r = rgb[i * 3 + 0], g = rgb[i * 3 + 1], b = rgb[i * 3 + 2];
        uint16_t v = ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3);
        palette_be[i] = (uint16_t)((v >> 8) | (v << 8));
    }
}

void ili9341_write_rows(int y, int rows, const uint8_t *indexed) {
    while (rows > 0) {
        int n = rows > CHUNK_ROWS ? CHUNK_ROWS : rows;
        int px = n * DOOM_WIDTH;
        uint16_t *dst = chunk_buf[cur_buf];

        // Convert while the previous chunk is still being DMA'd out of the other buffer.
        for (int i = 0; i < px; i++) dst[i] = palette_be[indexed[i]];

        int top = DOOM_Y_OFFSET + y;
        set_window(0, top, DOOM_WIDTH - 1, top + n - 1);   // waits for previous DMA to finish
        start_pixel_dma(dst, (size_t)px * 2);

        cur_buf ^= 1;
        indexed += px;
        y += n;
        rows -= n;
    }
}

void ili9341_fill(uint16_t rgb565) {
    uint16_t be = (uint16_t)((rgb565 >> 8) | (rgb565 << 8));
    for (int i = 0; i < CHUNK_PIXELS; i++) chunk_buf[0][i] = be;
    for (int y = 0; y < ILI9341_HEIGHT; y += CHUNK_ROWS) {
        set_window(0, y, ILI9341_WIDTH - 1, y + CHUNK_ROWS - 1);
        start_pixel_dma(chunk_buf[0], sizeof chunk_buf[0]);
    }
    ili9341_wait_idle();
}
