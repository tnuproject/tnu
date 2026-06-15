/**
 * @file font.h
 * @brief Bitmap font definitions
 */

#ifndef TNU_FONT_H
#define TNU_FONT_H

#include <tnu/types.h>

/**
 * Built-in PC Screen Font (PSF2-like)
 * 8x16 bitmap font for 256 characters
 * Each character is 8 pixels wide, 16 pixels tall
 */

#define FONT_WIDTH  8
#define FONT_HEIGHT 16
#define FONT_CHARS  256

/* Font data - 8x16 bitmap font */
extern const uint8_t font_data[FONT_CHARS * FONT_HEIGHT];

/**
 * font_get_glyph - Get bitmap for character
 * 
 * Returns pointer to FONT_HEIGHT bytes, each byte is one row
 * of the glyph (LSB = rightmost pixel)
 */
static inline const uint8_t *font_get_glyph(char c)
{
    return &font_data[(unsigned char)c * FONT_HEIGHT];
}

/**
 * font_draw_char - Draw character at position
 * 
 * x, y: Top-left pixel position
 * c: Character to draw
 * fg: Foreground color (RGB)
 * bg: Background color (RGB), 0xFFFFFFFF for transparent
 */
void font_draw_char(int32_t x, int32_t y, char c, uint32_t fg, uint32_t bg);

/**
 * font_draw_string - Draw null-terminated string
 */
void font_draw_string(int32_t x, int32_t y, const char *s, uint32_t fg, uint32_t bg);

/**
 * font_measure_string - Get pixel width of string
 */
uint32_t font_measure_string(const char *s);

/**
 * font_get_width - Get character width
 */
static inline uint32_t font_get_width(void) { return FONT_WIDTH; }

/**
 * font_get_height - Get character height
 */
static inline uint32_t font_get_height(void) { return FONT_HEIGHT; }

#endif /* TNU_FONT_H */