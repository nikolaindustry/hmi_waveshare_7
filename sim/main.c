/* =====================================================================
 *  main.c — SDL2 host for the Hyperwisor HMI UI.
 *
 *  Runs the REAL LVGL engine and the REAL ui_* sources in a desktop
 *  window: same 800x480 geometry, same 16-bit colour depth, same fonts
 *  as the panel. Mouse stands in for touch. What you see here is what
 *  the display renders, modulo the panel's own colour/backlight.
 *
 *  Build & run:  ./sim/run.sh
 *
 *  Two modes:
 *    (default)        the hand-built ui_* shell, as it ships
 *    --dashboard [p]  render a transcoded Hyperwisor dashboard spec
 *                     instead, so a JSON-authored dashboard can be
 *                     reviewed at 800x480 before anything is flashed.
 *                     Defaults to assets/dashboard/dashboard.json.
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
#include "hds.h"

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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

/* =====================================================================
 *  Dashboard mode
 * ===================================================================== */

static const char *s_spec_path  = "assets/dashboard/dashboard.json";
static const char *s_asset_dir  = "assets/dashboard/assets";
static bool        s_dashboard;
static const char *s_shot_path;   /* --shot: save a BMP and exit */
static int         s_shot_page;
static int         s_tap_x = -1, s_tap_y = -1;   /* --tap: synthetic touch */
static bool        s_tap_down;

/* Stands in for the firmware's hyperwisor_emit(). On the device this is
 * where the local-first resolver goes: a RELAY_CONTROL for a coil this
 * board owns should drive modbus_task directly instead of round-tripping
 * through the cloud. Here we just show what would go out. */
static void sim_emit(const char *target, const cJSON *payload, void *user)
{
    (void)user;
    /* Print the whole payload, placeholders already filled, so what you
     * see here is byte-for-byte what the web dashboard would send. */
    char *txt = cJSON_PrintUnformatted((cJSON *)payload);
    printf("  -> emit  target=%s\n     %s\n",
           target ? target : "(none)", txt ? txt : "{}");
    free(txt);
}

static void dashboard_show(void)
{
    if (hds_init(s_spec_path, s_asset_dir) != ESP_OK) {
        printf("!! could not load %s\n", s_spec_path);
        printf("   run: python3 tools/hds_transcode.py <export.json> "
               "-o assets/dashboard\n");
        return;
    }
    hds_set_emit(sim_emit, NULL);

    lv_obj_t *scr = lv_obj_create(NULL);
    lv_obj_remove_style_all(scr);
    lv_scr_load(scr);
    hds_build(scr);
}

/* Read the rendered frame back off the SDL renderer and save it. Gives
 * an exact 800x480 capture with no window-manager involvement, so a
 * layout can be reviewed (or diffed in CI) headlessly. */
static int save_shot(const char *path)
{
    SDL_Surface *surf = SDL_CreateRGBSurfaceWithFormat(
        0, DISP_HOR, DISP_VER, 24, SDL_PIXELFORMAT_RGB24);
    if (!surf) {
        fprintf(stderr, "shot: %s\n", SDL_GetError());
        return 1;
    }

    /* Re-compose the full texture into the back buffer and read that,
     * without presenting. Reading straight after a Present returns
     * whichever buffer the driver swapped in, which on a partial-render
     * flush is only the last band LVGL touched. */
    SDL_RenderClear(s_ren);
    SDL_RenderCopy(s_ren, s_tex, NULL, NULL);
    if (SDL_RenderReadPixels(s_ren, NULL, SDL_PIXELFORMAT_RGB24,
                             surf->pixels, surf->pitch) != 0) {
        fprintf(stderr, "shot: %s\n", SDL_GetError());
        SDL_FreeSurface(surf);
        return 1;
    }
    int rc = SDL_SaveBMP(surf, path);
    SDL_FreeSurface(surf);
    if (rc != 0) {
        fprintf(stderr, "shot: %s\n", SDL_GetError());
        return 1;
    }
    printf("wrote %s (%dx%d)\n", path, DISP_HOR, DISP_VER);
    return 0;
}

static void mouse_cb(lv_indev_drv_t *drv, lv_indev_data_t *data)
{
    (void)drv;
    /* --tap drives the real input device rather than calling a widget's
     * callback directly, so what gets exercised is the whole chain:
     * hit-testing, LVGL's event dispatch, then the emit sink. */
    if (s_tap_x >= 0) {
        data->point.x = s_tap_x;
        data->point.y = s_tap_y;
        data->state = s_tap_down ? LV_INDEV_STATE_PRESSED
                                 : LV_INDEV_STATE_RELEASED;
        return;
    }

    int x, y;
    Uint32 btn = SDL_GetMouseState(&x, &y);
    data->point.x = x;
    data->point.y = y;
    data->state = (btn & SDL_BUTTON(SDL_BUTTON_LEFT)) ? LV_INDEV_STATE_PRESSED
                                                      : LV_INDEV_STATE_RELEASED;
}

int main(int argc, char **argv)
{
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--dashboard") == 0) {
            s_dashboard = true;
            /* An optional path may follow, as long as it is not the
             * next flag. */
            if (i + 1 < argc && argv[i + 1][0] != '-') {
                s_spec_path = argv[++i];
            }
        } else if (strcmp(argv[i], "--assets") == 0 && i + 1 < argc) {
            s_asset_dir = argv[++i];
        } else if (strcmp(argv[i], "--shot") == 0 && i + 1 < argc) {
            s_shot_path = argv[++i];
        } else if (strcmp(argv[i], "--page") == 0 && i + 1 < argc) {
            s_shot_page = atoi(argv[++i]);
        } else if (strcmp(argv[i], "--tap") == 0 && i + 1 < argc) {
            if (sscanf(argv[++i], "%d,%d", &s_tap_x, &s_tap_y) != 2) {
                fprintf(stderr, "--tap wants X,Y\n");
                return 2;
            }
        } else {
            fprintf(stderr,
                    "usage: %s [--dashboard [spec.json]] [--assets DIR]\n"
                    "          [--page N] [--tap X,Y] [--shot out.bmp]\n",
                    argv[0]);
            return 2;
        }
    }

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

    if (s_dashboard) {
        /* Straight to the dashboard -- the splash belongs to the shell
         * and would only get in the way of reviewing a layout. */
        dashboard_show();
        printf("--- dashboard mode: %s ---\n", s_spec_path);
        printf("    click to 'touch', H outlines hit areas, "
               "1/2 switch pages, ESC quits\n");
    } else {
        /* Splash first, exactly as app.c does on the device, so the boot
         * screen can be previewed here too. Press R to replay it. */
        splash_show(SPLASH_MS, on_splash_done);
        printf("--- simulator up: 800x480, click to 'touch', "
               "R replays splash ---\n");
    }

    if (s_dashboard && s_shot_page > 0) {
        hds_spec_t *sp = hds_spec();
        if (sp && s_shot_page <= sp->n_pages) {
            hds_goto(sp->pages[s_shot_page - 1].id);
        }
    }

    /* --shot: pump the loop long enough for LVGL to lay out and flush a
     * full frame, save it, and quit. */
    if (s_shot_path) {
        for (int i = 0; i < 60; i++) {
            lv_timer_handler();
            lv_tick_inc(TICK_MS);
            SDL_PumpEvents();
        }

        if (s_tap_x >= 0) {
            printf("--- tap %d,%d ---\n", s_tap_x, s_tap_y);
            s_tap_down = true;
            for (int i = 0; i < 12; i++) {
                lv_timer_handler(); lv_tick_inc(TICK_MS);
            }
            s_tap_down = false;
            for (int i = 0; i < 24; i++) {
                lv_timer_handler(); lv_tick_inc(TICK_MS);
            }
        }
        int rc = save_shot(s_shot_path);
        SDL_DestroyTexture(s_tex);
        SDL_DestroyRenderer(s_ren);
        SDL_DestroyWindow(s_win);
        SDL_Quit();
        return rc;
    }

    bool running = true;
    while (running) {
        SDL_Event e;
        while (SDL_PollEvent(&e)) {
            if (e.type == SDL_QUIT) running = false;
            if (e.type == SDL_KEYDOWN && e.key.keysym.sym == SDLK_ESCAPE)
                running = false;
            /* R: wipe to a blank screen and replay the splash, so the
             * boot sequence can be reviewed without relaunching. */
            if (e.type == SDL_KEYDOWN && e.key.keysym.sym == SDLK_r
                && !s_dashboard) {
                lv_obj_t *fresh = lv_obj_create(NULL);
                lv_scr_load_anim(fresh, LV_SCR_LOAD_ANIM_NONE, 0, 0, true);
                splash_show(SPLASH_MS, on_splash_done);
            }

            if (s_dashboard && e.type == SDL_KEYDOWN) {
                SDL_Keycode k = e.key.keysym.sym;
                /* H: outline the hit areas. Navigation widgets are often
                 * transparent regions behind artwork, so there is no
                 * other way to see where they are. */
                if (k == SDLK_h) {
                    hds_set_debug_hitboxes(!hds_get_debug_hitboxes());
                }
                /* R: reload the spec from disk, so the transcoder can be
                 * re-run without relaunching the sim. */
                if (k == SDLK_r) {
                    dashboard_show();
                    printf("--- reloaded %s ---\n", s_spec_path);
                }
                /* 1..9: jump straight to a page by index. */
                if (k >= SDLK_1 && k <= SDLK_9) {
                    hds_spec_t *sp = hds_spec();
                    int idx = k - SDLK_1;
                    if (sp && idx < sp->n_pages) {
                        hds_goto(sp->pages[idx].id);
                    }
                }
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
