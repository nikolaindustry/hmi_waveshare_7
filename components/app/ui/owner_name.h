#pragma once
#include <stdbool.h>
#include "esp_err.h"

/* ------------------------------------------------------------------
 * Owner name, persisted to NVS.
 *
 * Call owner_name_init() once during app startup (it also takes
 * care of initialising NVS itself). After that:
 *   - owner_name_get() always returns a pointer to a NUL-terminated
 *     buffer; it is never NULL.
 *   - owner_name_set() updates the in-memory copy and persists it
 *     to NVS under namespace "cfg" / key "owner".
 *
 * Labels in the UI can subscribe to changes via owner_name_listen();
 * the stored callback is invoked after every successful set.
 * ------------------------------------------------------------------ */

#define OWNER_NAME_MAX 32

typedef void (*owner_name_cb_t)(const char *new_name);

esp_err_t   owner_name_init(void);                 /* call once, early */
const char *owner_name_get(void);                  /* never NULL */
esp_err_t   owner_name_set(const char *new_name);  /* persists + notifies */
void        owner_name_listen(owner_name_cb_t cb); /* latest callback wins */
