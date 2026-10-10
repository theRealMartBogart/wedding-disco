// lvgl_port.h — LVGL <-> BSP glue: ST7796 async-DMA display flush, FT6336
// touch input device, and the millisecond tick source.
//
// Call once after board_init(), st7796_init() and ft6336_init(). After that
// the app just calls lv_timer_handler() from its loop.
#ifndef WEDDING_LVGL_PORT_H
#define WEDDING_LVGL_PORT_H

#include <stdbool.h>
#include <stdint.h>

// lv_init() + display/indev/tick registration.
void lvgl_port_init(void);

// True while a DMA flush is in flight. The audio loop uses this to avoid
// touching SPI1 (shared with the CC1101) mid-transfer.
bool lvgl_port_flush_busy(void);

#endif // WEDDING_LVGL_PORT_H
