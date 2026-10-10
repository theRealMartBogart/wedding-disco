// lvgl_port.c — see lvgl_port.h.
#include "lvgl_port.h"
#include "lvgl.h"
#include "fw2.h"
#include "pico/stdlib.h"   // time_us_64

// Partial-render draw buffers, double-buffered, in SRAM.
//
// Sizing note: wilicankit measured LVGL's software rendering into a PSRAM
// buffer at roughly 2x slower than into SRAM (PSRAM's per-access QSPI latency
// dominates), so these stay in SRAM. 10 rows x 480 px x 2 bytes = 9.4 KB each,
// 18.8 KB for the pair.
//
// This is the RAM knob to turn first, and deliberately so: the whole firmware
// is copy_to_ram inside 512 KB, and LVGL's code alone is ~300 KB of that.
// Shrinking these buffers only costs speed (more, smaller DMA chunks per
// redraw) and can never fail at runtime, whereas shrinking LV_MEM_SIZE risks
// a silent allocation failure with LV_USE_LOG off. If a future UI needs more
// room, take it from here before touching the heap.
#define PORT_BUF_ROWS  10
#define PORT_BUF_PX    (ST7796_W * PORT_BUF_ROWS)
#define PORT_BUF_BYTES ((size_t)PORT_BUF_PX * sizeof(uint16_t))
static uint16_t s_buf1[PORT_BUF_PX];
static uint16_t s_buf2[PORT_BUF_PX];

static lv_display_t *s_disp;
static volatile bool s_busy;

static uint32_t tick_get_cb(void) {
    return (uint32_t)(time_us_64() / 1000);
}

static void flush_done_cb(void) {
    s_busy = false;
    lv_display_flush_ready(s_disp);
}

static void disp_flush_cb(lv_display_t *disp, const lv_area_t *area, uint8_t *px_map) {
    (void)disp;
    // LVGL renders RGB565 little-endian; the ST7796 driver wants big-endian
    // wire order.
    int32_t px_count = lv_area_get_width(area) * lv_area_get_height(area);
    lv_draw_sw_rgb565_swap(px_map, (uint32_t)px_count);

    s_busy = true;
    st7796_flush_async((uint16_t)area->x1, (uint16_t)area->y1,
                       (uint16_t)area->x2, (uint16_t)area->y2,
                       (const uint16_t *)px_map, flush_done_cb);
}

static void touch_read_cb(lv_indev_t *indev, lv_indev_data_t *data) {
    (void)indev;
    uint16_t x, y;
    if (ft6336_poll(&x, &y)) {
        data->point.x = x;
        data->point.y = y;
        data->state = LV_INDEV_STATE_PRESSED;
    } else {
        data->state = LV_INDEV_STATE_RELEASED;
    }
}

bool lvgl_port_flush_busy(void) { return s_busy || st7796_flush_busy(); }

void lvgl_port_init(void) {
    lv_init();
    lv_tick_set_cb(tick_get_cb);

    s_disp = lv_display_create(ST7796_W, ST7796_H);
    lv_display_set_color_format(s_disp, LV_COLOR_FORMAT_RGB565);
    lv_display_set_flush_cb(s_disp, disp_flush_cb);
    lv_display_set_buffers(s_disp, s_buf1, s_buf2, PORT_BUF_BYTES,
                           LV_DISPLAY_RENDER_MODE_PARTIAL);

    lv_indev_t *indev = lv_indev_create();
    lv_indev_set_type(indev, LV_INDEV_TYPE_POINTER);
    lv_indev_set_read_cb(indev, touch_read_cb);
}
