#include "hds_assets.h"
#include "esp_log.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(ESP_PLATFORM)
#include "esp_heap_caps.h"
#endif

static const char *TAG = "hds_assets";

#define HDS_ASSET_HDR     12
#define HDS_ASSET_MAX     16       /* distinct images per dashboard */
#define HDS_ASSET_KEY_MAX 20

typedef struct {
    char         key[HDS_ASSET_KEY_MAX];
    lv_img_dsc_t dsc;
    uint8_t     *px;
} entry_t;

static entry_t s_cache[HDS_ASSET_MAX];
static int     s_count;
static char    s_dir[160];

void hds_assets_init(const char *dir)
{
    hds_assets_reset();
    snprintf(s_dir, sizeof(s_dir), "%s", dir ? dir : ".");
    ESP_LOGI(TAG, "asset dir: %s", s_dir);
}

void hds_assets_reset(void)
{
    for (int i = 0; i < s_count; i++) {
        free(s_cache[i].px);
    }
    memset(s_cache, 0, sizeof(s_cache));
    s_count = 0;
}

/* Pixel buffers are the only sizeable allocation the renderer makes, so
 * on the device they go to PSRAM and leave internal RAM for LVGL. */
static uint8_t *px_alloc(size_t n)
{
#if defined(ESP_PLATFORM)
    uint8_t *p = heap_caps_malloc(n, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    return p ? p : malloc(n);
#else
    return malloc(n);
#endif
}

static const lv_img_dsc_t *load(const char *key)
{
    char path[256];
    snprintf(path, sizeof(path), "%s/%s.bin", s_dir, key);

    FILE *f = fopen(path, "rb");
    if (!f) {
        ESP_LOGW(TAG, "missing asset %s", path);
        return NULL;
    }

    uint8_t hdr[HDS_ASSET_HDR];
    if (fread(hdr, 1, sizeof(hdr), f) != sizeof(hdr) ||
        memcmp(hdr, "HDSI", 4) != 0) {
        ESP_LOGW(TAG, "%s is not an HDSI blob", path);
        fclose(f);
        return NULL;
    }

    uint32_t w  = (uint32_t)hdr[4] | ((uint32_t)hdr[5] << 8);
    uint32_t h  = (uint32_t)hdr[6] | ((uint32_t)hdr[7] << 8);
    uint8_t  cf = hdr[8];

    if (!w || !h || (cf != LV_IMG_CF_TRUE_COLOR &&
                     cf != LV_IMG_CF_TRUE_COLOR_ALPHA)) {
        ESP_LOGW(TAG, "%s: bad geometry %ux%u cf=%u", path,
                 (unsigned)w, (unsigned)h, cf);
        fclose(f);
        return NULL;
    }

    size_t bpp  = (cf == LV_IMG_CF_TRUE_COLOR_ALPHA) ? 3 : 2;
    size_t need = (size_t)w * h * bpp;

    uint8_t *px = px_alloc(need);
    if (!px) {
        ESP_LOGE(TAG, "%s: out of memory for %u bytes",
                 path, (unsigned)need);
        fclose(f);
        return NULL;
    }

    size_t got = fread(px, 1, need, f);
    fclose(f);

    if (got != need) {
        ESP_LOGW(TAG, "%s: truncated (%u of %u bytes)",
                 path, (unsigned)got, (unsigned)need);
        free(px);
        return NULL;
    }

    if (s_count >= HDS_ASSET_MAX) {
        ESP_LOGE(TAG, "asset cache full (%d); %s not loaded",
                 HDS_ASSET_MAX, key);
        free(px);
        return NULL;
    }

    entry_t *e = &s_cache[s_count++];
    snprintf(e->key, sizeof(e->key), "%s", key);
    e->px = px;
    e->dsc.header.always_zero = 0;
    e->dsc.header.cf = cf;
    e->dsc.header.w  = w;
    e->dsc.header.h  = h;
    e->dsc.data      = px;
    e->dsc.data_size = need;

    ESP_LOGI(TAG, "loaded %s  %ux%u  %s  %.1f KB", key,
             (unsigned)w, (unsigned)h,
             bpp == 3 ? "RGB565+A8" : "RGB565", need / 1024.0);
    return &e->dsc;
}

const lv_img_dsc_t *hds_assets_get(const char *key)
{
    if (!key || !key[0]) {
        return NULL;
    }
    for (int i = 0; i < s_count; i++) {
        if (strcmp(s_cache[i].key, key) == 0) {
            return &s_cache[i].dsc;
        }
    }
    return load(key);
}
