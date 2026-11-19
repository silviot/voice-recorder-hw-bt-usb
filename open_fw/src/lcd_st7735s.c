/*
 * Smartlink SL6801 Bare-Metal 8080 8-bit Parallel ST7735S LCD (80x160 RGB565)
 * and PWM3 Backlight Driver (0x40095000 + 0x40084080)
 *
 * SPDX-License-Identifier: MIT
 */
#include <stdint.h>
#include "sl6801_regs.h"
#include "sl6801_rom.h"
#include "openfw.h"

#define ST7735S_X_OFFSET 26u
#define ST7735S_Y_OFFSET 0u

static inline void lcdc_wait_idle(void)
{
    while ((LCDC_STATUS & 0x60000000u) != 0u) {
    }
}

static inline void lcdc_wait_done(void)
{
    while ((int32_t)LCDC_STATUS >= 0) {
    }
    LCDC_STATUS = 0x80000000u; /* W1C DONE */
}

static void st7735s_write_cmd(uint8_t cmd)
{
    LCDC_CMD_REG = (uint32_t)cmd;
    lcdc_wait_idle();
    LCDC_XFER_CTRL = (LCDC_XFER_CTRL & ~0x30Eu) | 0x07u;
    lcdc_wait_done();
}

static void st7735s_write_data(uint32_t data, uint8_t len)
{
    LCDC_TX_DATA  = data;
    LCDC_DATA_LEN = (uint32_t)(len - 1u);
    lcdc_wait_idle();
    LCDC_XFER_CTRL = (LCDC_XFER_CTRL & ~0x0Eu) | 0x0Bu;
    lcdc_wait_done();
}

static void st7735s_write_u8(uint8_t val)
{
    st7735s_write_data((uint32_t)val, 1u);
}

static void st7735s_write_bytes(const uint8_t *buf, uint32_t n)
{
    uint32_t i = 0u;
    while ((i + 4u) <= n) {
        uint32_t w = (uint32_t)buf[i]
                   | ((uint32_t)buf[i + 1u] << 8)
                   | ((uint32_t)buf[i + 2u] << 16)
                   | ((uint32_t)buf[i + 3u] << 24);
        st7735s_write_data(w, 4u);
        i += 4u;
    }
    while (i < n) {
        st7735s_write_u8(buf[i++]);
    }
}

static inline uint32_t pack_be16_pair(uint16_t p0, uint16_t p1)
{
    uint32_t b0 = ((uint32_t)(p0 >> 8)) | (((uint32_t)(p0 & 0xFFu)) << 8);
    uint32_t b1 = ((uint32_t)(p1 >> 8)) | (((uint32_t)(p1 & 0xFFu)) << 8);
    return b0 | (b1 << 16);
}

void lcd_backlight_set(uint8_t percent)
{
    if (percent > 100u) {
        percent = 100u;
    }

    if (!rom_rst_is_released(CRU_RST_PWM)) {
        rom_rst_release(CRU_RST_PWM);
    }
    rom_clk_set_src(CRU_CLK_PWM, CRU_CLK_SRC_HOSC24M);
    rom_clk_enable(CRU_CLK_PWM);

    /* PB17 = ALT5 (pwm_ch3) */
    gpio_set_mux(GPIO_PORT_B, 17u, 5u);

    PWM3_CTRL = 0x40u;
    PWM3_CTRL |= 0x3Fu;
    while ((PWM3_CTRL & (1u << 28)) != 0u) {
    }
    PWM3_PERIOD_DUTY = (48000u << 16) | ((uint32_t)percent * 480u);
}

void lcd_init(void)
{
    static const uint8_t init_b1[] = { 0x05u, 0x3Au, 0x3Au };
    static const uint8_t init_b3[] = { 0x05u, 0x3Au, 0x3Au, 0x05u, 0x3Au, 0x3Au };
    static const uint8_t init_c0[] = { 0x62u, 0x02u, 0x04u };
    static const uint8_t init_c2[] = { 0x0Du, 0x00u };
    static const uint8_t init_c3[] = { 0x8Du, 0x2Au };
    static const uint8_t init_c4[] = { 0x8Du, 0xEEu };
    static const uint8_t init_e0[] = {
        0x10u, 0x0Eu, 0x02u, 0x03u, 0x0Eu, 0x07u, 0x02u, 0x07u,
        0x0Au, 0x12u, 0x27u, 0x37u, 0x00u, 0x0Du, 0x0Eu, 0x10u,
    };
    static const uint8_t init_e1[] = {
        0x10u, 0x0Eu, 0x03u, 0x03u, 0x0Fu, 0x06u, 0x02u, 0x08u,
        0x0Au, 0x13u, 0x26u, 0x36u, 0x00u, 0x0Du, 0x0Eu, 0x10u,
    };

    /* Release LCDC reset & clock at 24 MHz HOSC */
    if (!rom_rst_is_released(CRU_RST_LCDC)) {
        rom_rst_release(CRU_RST_LCDC);
    }
    rom_clk_set_src(CRU_CLK_LCDC, CRU_CLK_SRC_HOSC24M);
    rom_clk_div(CRU_CLK_LCDC, 1u);
    rom_clk_enable(CRU_CLK_LCDC);

    /* Configure all 11 LCDC 8080 bus pads (mux=12): PB0..PB5, PB18, PD1..PD3, PD10 */
    for (uint32_t pin = 0u; pin <= 5u; pin++) {
        gpio_set_mux(GPIO_PORT_B, pin, 12u);
    }
    gpio_set_mux(GPIO_PORT_B, 18u, 12u);
    for (uint32_t pin = 1u; pin <= 3u; pin++) {
        gpio_set_mux(GPIO_PORT_D, pin, 12u);
    }
    gpio_set_mux(GPIO_PORT_D, 10u, 12u);

    /* Mandatory pad enable & LCDC crossbar routing (D0..D7, CS#, DC/RS, WR#) */
    GPIO_LCD_PAD_EN |= 0x0000FFD0u;
    LCDC_XBAR0       = 0xABF1FFFFu;
    LCDC_XBAR1       = 0x58764029u;
    LCDC_MCU_TIMING  = 0u;
    LCDC_IF_CTRL    &= ~1u; /* 8080 8-bit parallel mode */

    /* Hardware reset LCD via PB10 */
    gpio_set_mux(GPIO_PORT_B, 10u, 1u);
    gpio_write(GPIO_PORT_B, 10u, 1);
    mdelay(10u);
    gpio_write(GPIO_PORT_B, 10u, 0);
    mdelay(10u);
    gpio_write(GPIO_PORT_B, 10u, 1);
    mdelay(120u);

    /* ST7735S 80x160 IPS panel initialization */
    st7735s_write_cmd(0x11u); /* SLPOUT */
    mdelay(120u);

    st7735s_write_cmd(0xB1u);
    st7735s_write_bytes(init_b1, sizeof(init_b1));
    st7735s_write_cmd(0xB2u);
    st7735s_write_bytes(init_b1, sizeof(init_b1));
    st7735s_write_cmd(0xB3u);
    st7735s_write_bytes(init_b3, sizeof(init_b3));
    st7735s_write_cmd(0xB4u);
    st7735s_write_u8(0x03u);

    st7735s_write_cmd(0xC0u);
    st7735s_write_bytes(init_c0, sizeof(init_c0));
    st7735s_write_cmd(0xC1u);
    st7735s_write_u8(0xC0u);
    st7735s_write_cmd(0xC2u);
    st7735s_write_bytes(init_c2, sizeof(init_c2));
    st7735s_write_cmd(0xC3u);
    st7735s_write_bytes(init_c3, sizeof(init_c3));
    st7735s_write_cmd(0xC4u);
    st7735s_write_bytes(init_c4, sizeof(init_c4));
    st7735s_write_cmd(0xC5u);
    st7735s_write_u8(0x0Eu);

    st7735s_write_cmd(0x21u); /* INVON */

    st7735s_write_cmd(0xE0u);
    st7735s_write_bytes(init_e0, sizeof(init_e0));
    st7735s_write_cmd(0xE1u);
    st7735s_write_bytes(init_e1, sizeof(init_e1));

    st7735s_write_cmd(0x3Au); /* COLMOD: 16-bit RGB565 */
    st7735s_write_u8(0x05u);
    st7735s_write_cmd(0x36u); /* MADCTL: MY=1, MX=1, BGR=1 */
    st7735s_write_u8(0xC8u);
    st7735s_write_cmd(0x29u); /* DISPON */
    mdelay(10u);
}

void lcd_set_window(uint16_t x0, uint16_t y0, uint16_t x1, uint16_t y1)
{
    uint16_t xs = (uint16_t)(x0 + ST7735S_X_OFFSET);
    uint16_t xe = (uint16_t)(x1 + ST7735S_X_OFFSET);
    uint16_t ys = (uint16_t)(y0 + ST7735S_Y_OFFSET);
    uint16_t ye = (uint16_t)(y1 + ST7735S_Y_OFFSET);

    st7735s_write_cmd(0x2Au); /* CASET */
    st7735s_write_data(pack_be16_pair(xs, xe), 4u);

    st7735s_write_cmd(0x2Bu); /* RASET */
    st7735s_write_data(pack_be16_pair(ys, ye), 4u);

    st7735s_write_cmd(0x2Cu); /* RAMWR */
}

void lcd_fill_rect(uint16_t x, uint16_t y, uint16_t w, uint16_t h, uint16_t color)
{
    if (w == 0u || h == 0u || x >= LCD_WIDTH || y >= LCD_HEIGHT) {
        return;
    }
    uint16_t x1 = (uint16_t)(x + w - 1u);
    uint16_t y1 = (uint16_t)(y + h - 1u);
    if (x1 >= LCD_WIDTH)  x1 = LCD_WIDTH - 1u;
    if (y1 >= LCD_HEIGHT) y1 = LCD_HEIGHT - 1u;

    lcd_set_window(x, y, x1, y1);
    uint32_t total = (uint32_t)(x1 - x + 1u) * (uint32_t)(y1 - y + 1u);
    uint32_t pair  = pack_be16_pair(color, color);
    while (total >= 2u) {
        st7735s_write_data(pair, 4u);
        total -= 2u;
    }
    if (total != 0u) {
        st7735s_write_data(pair & 0xFFFFu, 2u);
    }
}

void lcd_blit_rgb565(uint16_t x, uint16_t y, uint16_t w, uint16_t h, const uint16_t *pixels)
{
    if (w == 0u || h == 0u || !pixels) {
        return;
    }
    lcd_set_window(x, y, (uint16_t)(x + w - 1u), (uint16_t)(y + h - 1u));
    uint32_t total = (uint32_t)w * (uint32_t)h;
    uint32_t i = 0u;
    while ((i + 2u) <= total) {
        st7735s_write_data(pack_be16_pair(pixels[i], pixels[i + 1u]), 4u);
        i += 2u;
    }
    if (i < total) {
        uint16_t p = pixels[i];
        uint32_t be = ((uint32_t)(p >> 8)) | (((uint32_t)(p & 0xFFu)) << 8);
        st7735s_write_data(be, 2u);
    }
}

/* Compact 5x7 ASCII bitmap font (0x20..0x7E): bit0=top row, bit6=bottom row */
static const uint8_t FONT5X7[95][5] = {
    {0x00, 0x00, 0x00, 0x00, 0x00}, /* ' ' */
    {0x00, 0x00, 0x5F, 0x00, 0x00}, /* '!' */
    {0x00, 0x07, 0x00, 0x07, 0x00}, /* '"' */
    {0x14, 0x7F, 0x14, 0x7F, 0x14}, /* '#' */
    {0x24, 0x2A, 0x7F, 0x2A, 0x12}, /* '$' */
    {0x23, 0x13, 0x08, 0x64, 0x62}, /* '%' */
    {0x36, 0x49, 0x55, 0x22, 0x50}, /* '&' */
    {0x00, 0x05, 0x03, 0x00, 0x00}, /* ''' */
    {0x00, 0x1C, 0x22, 0x41, 0x00}, /* '(' */
    {0x00, 0x41, 0x22, 0x1C, 0x00}, /* ')' */
    {0x08, 0x2A, 0x1C, 0x2A, 0x08}, /* '*' */
    {0x08, 0x08, 0x3E, 0x08, 0x08}, /* '+' */
    {0x00, 0x50, 0x30, 0x00, 0x00}, /* ',' */
    {0x08, 0x08, 0x08, 0x08, 0x08}, /* '-' */
    {0x00, 0x60, 0x60, 0x00, 0x00}, /* '.' */
    {0x20, 0x10, 0x08, 0x04, 0x02}, /* '/' */
    {0x3E, 0x51, 0x49, 0x45, 0x3E}, /* '0' */
    {0x00, 0x42, 0x7F, 0x40, 0x00}, /* '1' */
    {0x42, 0x61, 0x51, 0x49, 0x46}, /* '2' */
    {0x21, 0x41, 0x45, 0x4B, 0x31}, /* '3' */
    {0x18, 0x14, 0x12, 0x7F, 0x10}, /* '4' */
    {0x27, 0x45, 0x45, 0x45, 0x39}, /* '5' */
    {0x3C, 0x4A, 0x49, 0x49, 0x30}, /* '6' */
    {0x01, 0x71, 0x09, 0x05, 0x03}, /* '7' */
    {0x36, 0x49, 0x49, 0x49, 0x36}, /* '8' */
    {0x06, 0x49, 0x49, 0x29, 0x1E}, /* '9' */
    {0x00, 0x36, 0x36, 0x00, 0x00}, /* ':' */
    {0x00, 0x56, 0x36, 0x00, 0x00}, /* ';' */
    {0x00, 0x08, 0x14, 0x22, 0x41}, /* '<' */
    {0x14, 0x14, 0x14, 0x14, 0x14}, /* '=' */
    {0x41, 0x22, 0x14, 0x08, 0x00}, /* '>' */
    {0x02, 0x01, 0x51, 0x09, 0x06}, /* '?' */
    {0x32, 0x49, 0x79, 0x41, 0x3E}, /* '@' */
    {0x7E, 0x11, 0x11, 0x11, 0x7E}, /* 'A' */
    {0x7F, 0x49, 0x49, 0x49, 0x36}, /* 'B' */
    {0x3E, 0x41, 0x41, 0x41, 0x22}, /* 'C' */
    {0x7F, 0x41, 0x41, 0x22, 0x1C}, /* 'D' */
    {0x7F, 0x49, 0x49, 0x49, 0x41}, /* 'E' */
    {0x7F, 0x09, 0x09, 0x01, 0x01}, /* 'F' */
    {0x3E, 0x41, 0x41, 0x51, 0x32}, /* 'G' */
    {0x7F, 0x08, 0x08, 0x08, 0x7F}, /* 'H' */
    {0x00, 0x41, 0x7F, 0x41, 0x00}, /* 'I' */
    {0x20, 0x40, 0x41, 0x3F, 0x01}, /* 'J' */
    {0x7F, 0x08, 0x14, 0x22, 0x41}, /* 'K' */
    {0x7F, 0x40, 0x40, 0x40, 0x40}, /* 'L' */
    {0x7F, 0x02, 0x04, 0x02, 0x7F}, /* 'M' */
    {0x7F, 0x04, 0x08, 0x10, 0x7F}, /* 'N' */
    {0x3E, 0x41, 0x41, 0x41, 0x3E}, /* 'O' */
    {0x7F, 0x09, 0x09, 0x09, 0x06}, /* 'P' */
    {0x3E, 0x41, 0x51, 0x21, 0x5E}, /* 'Q' */
    {0x7F, 0x09, 0x19, 0x29, 0x46}, /* 'R' */
    {0x46, 0x49, 0x49, 0x49, 0x31}, /* 'S' */
    {0x01, 0x01, 0x7F, 0x01, 0x01}, /* 'T' */
    {0x3F, 0x40, 0x40, 0x40, 0x3F}, /* 'U' */
    {0x1F, 0x20, 0x40, 0x20, 0x1F}, /* 'V' */
    {0x7F, 0x20, 0x18, 0x20, 0x7F}, /* 'W' */
    {0x63, 0x14, 0x08, 0x14, 0x63}, /* 'X' */
    {0x03, 0x04, 0x78, 0x04, 0x03}, /* 'Y' */
    {0x61, 0x51, 0x49, 0x45, 0x43}, /* 'Z' */
    {0x00, 0x00, 0x7F, 0x41, 0x41}, /* '[' */
    {0x02, 0x04, 0x08, 0x10, 0x20}, /* '\' */
    {0x41, 0x41, 0x7F, 0x00, 0x00}, /* ']' */
    {0x04, 0x02, 0x01, 0x02, 0x04}, /* '^' */
    {0x40, 0x40, 0x40, 0x40, 0x40}, /* '_' */
    {0x00, 0x01, 0x02, 0x04, 0x00}, /* '`' */
    {0x20, 0x54, 0x54, 0x54, 0x78}, /* 'a' */
    {0x7F, 0x48, 0x44, 0x44, 0x38}, /* 'b' */
    {0x38, 0x44, 0x44, 0x44, 0x20}, /* 'c' */
    {0x38, 0x44, 0x44, 0x48, 0x7F}, /* 'd' */
    {0x38, 0x54, 0x54, 0x54, 0x18}, /* 'e' */
    {0x08, 0x7E, 0x09, 0x01, 0x02}, /* 'f' */
    {0x08, 0x14, 0x54, 0x54, 0x3C}, /* 'g' */
    {0x7F, 0x08, 0x04, 0x04, 0x78}, /* 'h' */
    {0x00, 0x44, 0x7D, 0x40, 0x00}, /* 'i' */
    {0x20, 0x40, 0x44, 0x3D, 0x00}, /* 'j' */
    {0x00, 0x7F, 0x10, 0x28, 0x44}, /* 'k' */
    {0x00, 0x41, 0x7F, 0x40, 0x00}, /* 'l' */
    {0x7C, 0x04, 0x18, 0x04, 0x78}, /* 'm' */
    {0x7C, 0x08, 0x04, 0x04, 0x78}, /* 'n' */
    {0x38, 0x44, 0x44, 0x44, 0x38}, /* 'o' */
    {0x7C, 0x14, 0x14, 0x14, 0x08}, /* 'p' */
    {0x08, 0x14, 0x14, 0x18, 0x7C}, /* 'q' */
    {0x7C, 0x08, 0x04, 0x04, 0x08}, /* 'r' */
    {0x48, 0x54, 0x54, 0x54, 0x20}, /* 's' */
    {0x04, 0x3F, 0x44, 0x40, 0x20}, /* 't' */
    {0x3C, 0x40, 0x40, 0x20, 0x7C}, /* 'u' */
    {0x1C, 0x20, 0x40, 0x20, 0x1C}, /* 'v' */
    {0x3C, 0x40, 0x30, 0x40, 0x3C}, /* 'w' */
    {0x44, 0x28, 0x10, 0x28, 0x44}, /* 'x' */
    {0x0C, 0x50, 0x50, 0x50, 0x3C}, /* 'y' */
    {0x44, 0x64, 0x54, 0x4C, 0x44}, /* 'z' */
    {0x00, 0x08, 0x36, 0x41, 0x00}, /* '{' */
    {0x00, 0x00, 0x7F, 0x00, 0x00}, /* '|' */
    {0x00, 0x41, 0x36, 0x08, 0x00}, /* '}' */
    {0x08, 0x08, 0x2A, 0x1C, 0x08}, /* '~' -> arrow */
};

void lcd_draw_char(uint16_t x, uint16_t y, char c, uint16_t fg, uint16_t bg)
{
    if ((uint16_t)(x + 6u) > LCD_WIDTH || (uint16_t)(y + 8u) > LCD_HEIGHT) {
        return;
    }
    uint8_t uc = (uint8_t)c;
    if (uc < 0x20u || uc > 0x7Eu) {
        uc = '?';
    }
    const uint8_t *cols = FONT5X7[uc - 0x20u];

    lcd_set_window(x, y, (uint16_t)(x + 5u), (uint16_t)(y + 7u));
    for (uint16_t row = 0u; row < 8u; row++) {
        uint16_t row_px[6];
        for (uint16_t col = 0u; col < 5u; col++) {
            uint8_t on = (row < 7u) ? (uint8_t)((cols[col] >> row) & 1u) : 0u;
            row_px[col] = on ? fg : bg;
        }
        row_px[5] = bg;
        st7735s_write_data(pack_be16_pair(row_px[0], row_px[1]), 4u);
        st7735s_write_data(pack_be16_pair(row_px[2], row_px[3]), 4u);
        st7735s_write_data(pack_be16_pair(row_px[4], row_px[5]), 4u);
    }
}

void lcd_draw_str(uint16_t x, uint16_t y, const char *s, uint16_t fg, uint16_t bg)
{
    uint16_t cx = x;
    while (*s != '\0' && (uint16_t)(cx + 6u) <= LCD_WIDTH) {
        lcd_draw_char(cx, y, *s++, fg, bg);
        cx += 6u;
    }
}

void lcd_draw_u32(uint16_t x, uint16_t y, uint32_t v, uint16_t fg, uint16_t bg)
{
    char buf[12];
    int i = 0;
    if (v == 0u) {
        lcd_draw_char(x, y, '0', fg, bg);
        return;
    }
    while (v > 0u && i < 11) {
        buf[i++] = (char)('0' + (v % 10u));
        v /= 10u;
    }
    uint16_t cx = x;
    while (i > 0 && (uint16_t)(cx + 6u) <= LCD_WIDTH) {
        lcd_draw_char(cx, y, buf[--i], fg, bg);
        cx += 6u;
    }
}

void lcd_draw_hex(uint16_t x, uint16_t y, uint32_t v, uint8_t digits, uint16_t fg, uint16_t bg)
{
    static const char hex[] = "0123456789ABCDEF";
    uint16_t cx = x;
    for (int d = (int)digits - 1; d >= 0 && (uint16_t)(cx + 6u) <= LCD_WIDTH; d--) {
        lcd_draw_char(cx, y, hex[(v >> (d * 4)) & 0xFu], fg, bg);
        cx += 6u;
    }
}
