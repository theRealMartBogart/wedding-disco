/* lv_conf.h — minimal LVGL v9.3 config for wedding_disco_lvgl.
 *
 * Only overrides that differ from LVGL's built-in defaults are listed; the
 * rest fall back to lv_conf_internal.h (which mirrors lv_conf_template.h).
 *
 * This app is a real-time audio-reactive display: the mic loop must keep
 * pulling PDM blocks every 16 ms or the capture ring overruns, so the config
 * is tuned for a small, predictable LVGL footprint rather than a rich UI. */

#ifndef LV_CONF_H
#define LV_CONF_H

/* RGB565, matching the ST7796 panel. LVGL renders little-endian native; the
 * panel wants big-endian on the wire, fixed up per-flush with
 * lv_draw_sw_rgb565_swap() in lvgl_port.c rather than here. */
#define LV_COLOR_DEPTH 16

/* Object heap in SRAM. Unlike wilicankit (which needed a 1 MB PSRAM pool for
 * nested modals, file explorers and loaded configs), this UI is a fixed set
 * of ~40 widgets built once at startup and only updated in place — nothing
 * is created or destroyed per frame, so the heap never churns. 48 KB is
 * ample and avoids pulling PSRAM into the real-time path. */
#define LV_USE_STDLIB_MALLOC LV_STDLIB_BUILTIN
#define LV_MEM_SIZE (48U * 1024U)

/* No RTOS — bare super-loop shared with the audio/render loop. */
#define LV_USE_OS LV_OS_NONE

/* Diagnostics go out over RTT via DIAG(); skip LVGL's own logging. */
#define LV_USE_LOG 0
#define LV_BUILD_EXAMPLES 0

/* Redraw at the frame rate the audio loop already runs at (~62 Hz); there is
 * no point refreshing faster than we produce new spectrum data. */
#define LV_DEF_REFR_PERIOD 16

/* Fonts: 14 for body text and button labels, 28 for the big BPM readout. */
#define LV_FONT_MONTSERRAT_14 1
#define LV_FONT_MONTSERRAT_28 1
#define LV_FONT_DEFAULT &lv_font_montserrat_14

/* Widgets this app never creates — disabled to free copy_to_ram code space.
 * Each widget's .c is wrapped in `#if LV_USE_<X> != 0`, so switching one off
 * that turns out to be needed is a build error, not a runtime surprise.
 * Left ON (used here): label, button, bar (spectrum columns + VU), slider's
 * dependency chain is not needed but bar is, obj/style basics. */
#define LV_USE_ANIMIMG     0
#define LV_USE_ARC         0
#define LV_USE_CALENDAR    0
#define LV_USE_CANVAS      0
#define LV_USE_CHART       0
#define LV_USE_CHECKBOX    0
#define LV_USE_DROPDOWN    0
#define LV_USE_IMAGEBUTTON 0
#define LV_USE_KEYBOARD    0
#define LV_USE_LED         0
#define LV_USE_LINE        0
#define LV_USE_LIST        0
#define LV_USE_MENU        0
#define LV_USE_MSGBOX      0
#define LV_USE_ROLLER      0
#define LV_USE_SCALE       0
#define LV_USE_SPAN        0
#define LV_USE_SPINBOX     0
#define LV_USE_SPINNER     0
#define LV_USE_TABLE       0
#define LV_USE_TABVIEW     0
#define LV_USE_TEXTAREA    0
#define LV_USE_TILEVIEW    0
#define LV_USE_WIN         0

#endif /*LV_CONF_H*/
