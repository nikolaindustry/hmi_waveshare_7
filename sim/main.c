/* =====================================================================
 *  main.c — SDL2 host for the Hyperwisor HMI UI.
 *
 *  Runs the REAL LVGL engine and the REAL ui_* sources in a desktop
 *  window: same 800x480 geometry, same 16-bit colour depth, same fonts
 *  as the panel. Mouse stands in for touch. What you see here is what
 *  the display renders, modulo the panel's own colour/backlight.
 *
 *  Build & run:  ./sim/run.sh
 * ===================================================================== */

#include <SDL2/SDL.h>
#include "lvgl.h"
#include "ui_shell.h"
#include "splash.h"
#include "ui_theme.h"
#include "owner_name.h"
#include "tile_names.h"
#include "display_brightness.h"
#include "hmi_role.h"
#include "bus_config.h"
#include "ctrl_state.h"

#include <stdbool.h>
#include <stdio.h>

#define DISP_HOR   800
#define DISP_VER   480
#define TICK_MS    5

/* One tenth of the screen is plenty for the partial-render buffer and
 * matches the order of magnitude of the device's bounce buffer. */
static lv_color_t          s_buf[DISP_HOR * 48];
static lv_disp_draw_buf_t  s_draw_buf;
static lv_disp_drv_t       s_disp_drv;
static lv_indev_drv_t      s_indev_drv;

static SDL_Window   *s_win;
static SDL_Renderer *s_ren;
static SDL_Texture  *s_tex;

static void flush_cb(lv_disp_drv_t *drv, const lv_area_t *area, lv_color_t *px)
{
    SDL_Rect r = {
        .x = area->x1, .y = area->y1,
        .w = area->x2 - area->x1 + 1,
        .h = area->y2 - area->y1 + 1,
    };
    /* LVGL hands us a tightly packed buffer for exactly this area, so
     * the pitch is the area width, not the screen width. */
    SDL_UpdateTexture(s_tex, &r, px, r.w * sizeof(lv_color_t));
    SDL_RenderClear(s_ren);
    SDL_RenderCopy(s_ren, s_tex, NULL, NULL);
    SDL_RenderPresent(s_ren);
    lv_disp_flush_ready(drv);
}

#define SPLASH_MS 3000

/* splash_show() hands us a fresh screen to paint the main UI onto. */
static void on_splash_done(lv_obj_t *scr)
{
    ui_shell_build(scr);
}

static void mouse_cb(lv_indev_drv_t *drv, lv_indev_data_t *data)
{
    (void)drv;
    int x, y;
    Uint32 btn = SDL_GetMouseState(&x, &y);
    data->point.x = x;
    data->point.y = y;
    data->state = (btn & SDL_BUTTON(SDL_BUTTON_LEFT)) ? LV_INDEV_STATE_PRESSED
                                                      : LV_INDEV_STATE_RELEASED;
}

int main(int argc, char **argv)
{
    (void)argc; (void)argv;

    /* Line-buffer stdout so ESP_LOGx output appears live when the sim's
     * output is piped to a file, not only when the process exits. */
    setvbuf(stdout, NULL, _IOLBF, 0);

    if (SDL_Init(SDL_INIT_VIDEO) != 0) {
        fprintf(stderr, "SDL_Init: %s\n", SDL_GetError());
        return 1;
    }
    s_win = SDL_CreateWindow("Hyperwisor HMI — simulator",
                             SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
                             DISP_HOR, DISP_VER, SDL_WINDOW_SHOWN);
    s_ren = SDL_CreateRenderer(s_win, -1, SDL_RENDERER_ACCELERATED);
    /* RGB565 matches LV_COLOR_DEPTH 16 so no per-pixel conversion. */
    s_tex = SDL_CreateTexture(s_ren, SDL_PIXELFORMAT_RGB565,
                              SDL_TEXTUREACCESS_STREAMING, DISP_HOR, DISP_VER);
    if (!s_win || !s_ren || !s_tex) {
        fprintf(stderr, "SDL setup failed: %s\n", SDL_GetError());
        return 1;
    }

    lv_init();

    lv_disp_draw_buf_init(&s_draw_buf, s_buf, NULL, DISP_HOR * 48);
    lv_disp_drv_init(&s_disp_drv);
    s_disp_drv.draw_buf = &s_draw_buf;
    s_disp_drv.flush_cb = flush_cb;
    s_disp_drv.hor_res  = DISP_HOR;
    s_disp_drv.ver_res  = DISP_VER;
    lv_disp_drv_register(&s_disp_drv);

    lv_indev_drv_init(&s_indev_drv);
    s_indev_drv.type    = LV_INDEV_TYPE_POINTER;
    s_indev_drv.read_cb = mouse_cb;
    lv_indev_drv_register(&s_indev_drv);

    /* Same boot order as main.c on the device: settings out of NVS
     * first (so the first paint uses the right theme/names), then the
     * shell. bsp_init()/LVGL port are replaced by the SDL setup above. */
    owner_name_init();
    hmi_role_init();
    bus_config_init();
    ui_theme_init();
    tile_names_init();
    ctrl_state_init();
    display_brightness_init();
    ctrl_state_start_autosave();  /* LVGL is already up here */

    /* Splash first, exactly as app.c does on the device, so the boot
     * screen can be previewed here too. Press R to replay it. */
    splash_show(SPLASH_MS, on_splash_done);
    printf("--- simulator up: 800x480, click to 'touch', R replays splash ---\n");

    bool running = true;
    while (running) {
        SDL_Event e;
        while (SDL_PollEvent(&e)) {
            if (e.type == SDL_QUIT) running = false;
            if (e.type == SDL_KEYDOWN && e.key.keysym.sym == SDLK_ESCAPE)
                running = false;
            /* R: wipe to a blank screen and replay the splash, so the
             * boot sequence can be reviewed without relaunching. */
            if (e.type == SDL_KEYDOWN && e.key.keysym.sym == SDLK_r) {
                lv_obj_t *fresh = lv_obj_create(NULL);
                lv_scr_load_anim(fresh, LV_SCR_LOAD_ANIM_NONE, 0, 0, true);
                splash_show(SPLASH_MS, on_splash_done);
            }
        }
        lv_timer_handler();
        SDL_Delay(TICK_MS);
        lv_tick_inc(TICK_MS);
    }

    SDL_DestroyTexture(s_tex);
    SDL_DestroyRenderer(s_ren);
    SDL_DestroyWindow(s_win);
    SDL_Quit();
    return 0;
}
