/*
 * In-memory stand-in for Zephyr's settings subsystem (NVS flash on the real
 * board).  Saved values live only as long as the simulation, so every run
 * starts like a freshly flashed board with cleared settings.
 */
#include <zephyr/kernel.h>
#include <zephyr/settings/settings.h>
#include "sim_internal.h"

SIM_SECTION_MARKERS(struct settings_handler_static, settings_handler_static);

#define MAX_ITEMS 64
static struct {
    char key[64];
    uint8_t val[64];
    size_t len;
    bool used;
} store[MAX_ITEMS];

int settings_subsys_init(void) { return 0; }

int settings_save_one(const char *name, const void *value, size_t val_len) {
    for (int i = 0; i < MAX_ITEMS; i++) {
        if (!store[i].used || strcmp(store[i].key, name) == 0) {
            snprintf(store[i].key, sizeof(store[i].key), "%s", name);
            store[i].len = MIN(val_len, sizeof(store[i].val));
            memcpy(store[i].val, value, store[i].len);
            store[i].used = true;
            sim_log(SIM_LOG_DBG, "settings: saved %s (%u bytes)", name, (unsigned)val_len);
            return 0;
        }
    }
    return -ENOMEM;
}

int settings_delete(const char *name) {
    for (int i = 0; i < MAX_ITEMS; i++) {
        if (store[i].used && strcmp(store[i].key, name) == 0) {
            store[i].used = false;
        }
    }
    return 0;
}

int settings_name_steq(const char *name, const char *key, const char **next) {
    if (next) {
        *next = NULL;
    }
    if (!name || !key) {
        return 0;
    }
    while (*key && *name && *name == *key) {
        name++;
        key++;
    }
    if (*key) {
        return 0;
    }
    if (*name == '/') {
        if (next) {
            *next = name + 1;
        }
        return 1;
    }
    return *name == '=' || *name == '\0';
}

int settings_name_next(const char *name, const char **next) {
    int rc = 0;
    if (next) {
        *next = NULL;
    }
    if (!name) {
        return 0;
    }
    while (*name != '\0' && *name != '=' && *name != '/') {
        rc++;
        name++;
    }
    if (*name == '/' && next) {
        *next = name + 1;
    }
    return rc;
}

/* The store starts empty, so loading only runs the handlers' commit hooks. */
int settings_load_subtree(const char *subtree) {
    STRUCT_SECTION_FOREACH(settings_handler_static, h) {
        if (subtree && !settings_name_steq(h->name, subtree, NULL)) {
            continue;
        }
        if (h->h_commit) {
            h->h_commit();
        }
    }
    return 0;
}
int settings_load(void) { return settings_load_subtree(NULL); }
int settings_load_subtree_direct(const char *subtree, settings_load_direct_cb cb, void *param) {
    return 0;
}
int settings_commit(void) { return 0; }
int settings_commit_subtree(const char *subtree) { return 0; }
