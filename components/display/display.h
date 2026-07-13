#pragma once

#include <stdbool.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define DISPLAY_WIDTH  72
#define DISPLAY_HEIGHT 40
#define DISPLAY_PAGES  (DISPLAY_HEIGHT / 8)

/* Initialize the I2C bus and the SSD1306 controller. */
esp_err_t display_init(void);

/* Clear the local framebuffer (call display_flush to update the screen). */
void display_clear(void);

/* Draw a text string into the framebuffer using a 5x7 font.
 * x is in pixels (0..71), page is the 8-pixel-tall row (0..4).
 * Each character is 6 pixels wide.
 */
void display_draw_text(int x, int page, const char *text);

/* Set or clear one pixel in the local framebuffer. */
void display_draw_pixel(int x, int y, bool on);

/* Push the framebuffer to the OLED. */
esp_err_t display_flush(void);

/* Serialize clear/draw/flush sequences between tasks. Hold the lock
 * for the whole sequence, not per call. */
void display_lock(void);
void display_unlock(void);

#ifdef __cplusplus
}
#endif
