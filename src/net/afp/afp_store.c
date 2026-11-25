/* Livadix — the AFP server's ID and desktop databases (afp_store.h).
 *
 * Sources: Inside AppleTalk, 2nd ed., chapter 13: "Directory IDs" (the root
 * is 2, its parent 1, 0 is no ID), "Desktop database" and the FPAddIcon,
 * FPGetIcon, FPGetIconInfo, FPAddAPPL, FPGetAPPL, FPRemoveAPPL,
 * FPAddComment, FPGetComment and FPRemoveComment calls (what the database
 * holds and how its entries are found).
 *
 * Livadix's own code. */
#include "afp_store.h"
#include "macroman.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---- an open-addressing table: 64-bit keys to slots ---------------------- */

typedef struct table {
    uint64_t *keys;
    uint32_t *slots;            /* slot + 1; 0 empty, UINT32_MAX removed */
    size_t size, used;
} table;

static uint64_t mix(uint64_t x) {
    x ^= x >> 33;
    x *= 0xFF51AFD7ED558CCDULL;
    x ^= x >> 33;
    x *= 0xC4CEB9FE1A85EC53ULL;
    return x ^ (x >> 33);
}

static int table_grow(table *t) {
    table n;
    n.size = t->size ? t->size * 2 : 1024;
    n.used = 0;
    n.keys = calloc(n.size, sizeof *n.keys);
    n.slots = calloc(n.size, sizeof *n.slots);
    if (!n.keys || !n.slots) {
        free(n.keys);
        free(n.slots);
        return 0;
    }
    for (size_t i = 0; i < t->size; i++) {
        if (!t->slots[i] || t->slots[i] == UINT32_MAX) continue;
        size_t at = mix(t->keys[i]) & (n.size - 1);
        while (n.slots[at]) at = (at + 1) & (n.size - 1);
        n.keys[at] = t->keys[i];
        n.slots[at] = t->slots[i];
        n.used++;
    }
    free(t->keys);
    free(t->slots);
    *t = n;
    return 1;
}

static void table_put(table *t, uint64_t key, uint32_t slot) {
    if ((t->used + 1) * 2 > t->size && !table_grow(t)) return;
    size_t at = mix(key) & (t->size - 1);
    while (t->slots[at] && t->slots[at] != UINT32_MAX) at = (at + 1) & (t->size - 1);
    t->keys[at] = key;
    t->slots[at] = slot + 1;
    t->used++;
}

static void table_remove(table *t, uint64_t key, uint32_t slot) {
    if (!t->size) return;
    for (size_t at = mix(key) & (t->size - 1); t->slots[at]; at = (at + 1) & (t->size - 1))
        if (t->slots[at] == slot + 1 && t->keys[at] == key) {
            t->slots[at] = UINT32_MAX;
            return;
        }
}

/* Calls each slot stored under key until visit returns nonzero. */
static uint32_t table_find(const table *t, uint64_t key, int (*visit)(void *ctx, uint32_t slot), void *ctx) {
    if (!t->size) return UINT32_MAX;
    for (size_t at = mix(key) & (t->size - 1); t->slots[at]; at = (at + 1) & (t->size - 1))
        if (t->slots[at] != UINT32_MAX && t->keys[at] == key && visit(ctx, t->slots[at] - 1)) return t->slots[at] - 1;
    return UINT32_MAX;
}

static void table_free(table *t) {
    free(t->keys);
    free(t->slots);
}

/* ---- IDs ------------------------------------------------------------------- */

typedef struct record {
    uint32_t id, parent;
    uint64_t index;
    uint16_t *name;
} record;

struct mc_afp_ids {
    record *r;
    size_t count, capacity;
    table by_id, by_name, by_index;
    uint32_t next;
    mc_host_file *log;
    uint64_t log_end;
    uint16_t path[MC_HOST_PATH];
};

/* Windows compares names without case: so does the key. */
static uint64_t name_key(uint32_t parent, const uint16_t *name) {
    uint64_t h = 0xCBF29CE484222325ULL ^ parent;
    for (; *name; name++) {
        uint16_t c = *name;
        if (c >= 'a' && c <= 'z') c = (uint16_t)(c - 32);
        else if (c >= 0xE0 && c <= 0xFE && c != 0xF7) c = (uint16_t)(c - 32);
        h = (h ^ c) * 0x100000001B3ULL;
    }
    return h;
}

static uint16_t fold(uint16_t c) {
    if (c >= 'a' && c <= 'z') return (uint16_t)(c - 32);
    if (c >= 0xE0 && c <= 0xFE && c != 0xF7) return (uint16_t)(c - 32);
    return c;
}

static int same_name(const uint16_t *a, const uint16_t *b) {
    for (; *a && *b; a++, b++)
        if (fold(*a) != fold(*b)) return 0;
    return *a == *b;
}

typedef struct probe {
    mc_afp_ids *ids;
    uint32_t id, parent;
    const uint16_t *name;
    uint64_t index;
} probe;

static int match_id(void *ctx, uint32_t slot) {
    const probe *p = ctx;
    return p->ids->r[slot].id == p->id;
}
static int match_name(void *ctx, uint32_t slot) {
    const probe *p = ctx;
    const record *r = &p->ids->r[slot];
    return r->id && r->parent == p->parent && same_name(r->name, p->name);
}
static int match_index(void *ctx, uint32_t slot) {
    const probe *p = ctx;
    return p->ids->r[slot].id && p->ids->r[slot].index == p->index;
}

static uint32_t slot_of_id(mc_afp_ids *ids, uint32_t id) {
    probe p = {ids, id, 0, NULL, 0};
    return table_find(&ids->by_id, id, match_id, &p);
}

static void log_line(mc_afp_ids *ids, const char *line) {
    if (!ids->log) return;
    const size_t n = strlen(line);
    if (mc_host_write(ids->log, ids->log_end, line, n) == MC_AFP_OK) ids->log_end += n;
}

static void log_record(mc_afp_ids *ids, const record *r) {
    char line[MC_HOST_NAME * 3 + 64], name[MC_HOST_NAME * 3 + 1];
    mc_host_to_utf8(r->name, name, sizeof name);
    snprintf(line, sizeof line, "S\t%lu\t%lu\t%llu\t%s\n", (unsigned long)r->id, (unsigned long)r->parent, (unsigned long long)r->index,
             name);
    log_line(ids, line);
}

static uint16_t *dup16(const uint16_t *s) {
    const size_t n = mc_host_len(s);
    uint16_t *d = malloc((n + 1) * 2);
    if (d) memcpy(d, s, (n + 1) * 2);
    return d;
}

static void unlink_record(mc_afp_ids *ids, uint32_t slot) {
    record *r = &ids->r[slot];
    table_remove(&ids->by_name, name_key(r->parent, r->name), slot);
    if (r->index) table_remove(&ids->by_index, r->index, slot);
}

static void link_record(mc_afp_ids *ids, uint32_t slot) {
    record *r = &ids->r[slot];
    table_put(&ids->by_name, name_key(r->parent, r->name), slot);
    if (r->index) table_put(&ids->by_index, r->index, slot);
}

/* Sets a record (without logging). */
static void set_record(mc_afp_ids *ids, uint32_t id, uint32_t parent, const uint16_t *name, uint64_t index) {
    uint32_t slot = slot_of_id(ids, id);
    if (slot != UINT32_MAX) {
        unlink_record(ids, slot);
        uint16_t *n = dup16(name);
        if (!n) return;
        free(ids->r[slot].name);
        ids->r[slot].name = n;
        ids->r[slot].parent = parent;
        if (index) ids->r[slot].index = index;
        link_record(ids, slot);
    } else {
        if (ids->count == ids->capacity) {
            const size_t cap = ids->capacity ? ids->capacity * 2 : 1024;
            record *r = realloc(ids->r, cap * sizeof *r);
            if (!r) return;
            ids->r = r;
            ids->capacity = cap;
        }
        slot = (uint32_t)ids->count;
        record *r = &ids->r[slot];
        r->name = dup16(name);
        if (!r->name) return;
        r->id = id;
        r->parent = parent;
        r->index = index;
        ids->count++;
        table_put(&ids->by_id, id, slot);
        link_record(ids, slot);
    }
    if (id >= ids->next) ids->next = id + 1;
}

static void remove_record(mc_afp_ids *ids, uint32_t id) {
    const uint32_t slot = slot_of_id(ids, id);
    if (slot == UINT32_MAX) return;
    unlink_record(ids, slot);
    table_remove(&ids->by_id, id, slot);
    free(ids->r[slot].name);
    ids->r[slot].name = NULL;
    ids->r[slot].id = 0;
}

static void load(mc_afp_ids *ids, mc_host_file *f) {
    uint64_t size = 0;
    if (mc_host_size(f, &size) || !size || size > 256u * 1024 * 1024) return;
    char *text = malloc((size_t)size + 1);
    size_t got = 0;
    if (!text || mc_host_read(f, 0, text, (size_t)size, &got)) {
        free(text);
        return;
    }
    text[got] = 0;
    for (char *line = text; *line;) {
        char *end = strchr(line, '\n');
        if (!end) break;               /* a line cut short by a crash is dropped */
        *end = 0;
        unsigned long id, parent;
        unsigned long long index;
        int at = 0;
        if (line[0] == 'S' && sscanf(line, "S\t%lu\t%lu\t%llu\t%n", &id, &parent, &index, &at) == 3 && at && id >= MC_AFP_FIRST_ID) {
            uint16_t name[MC_HOST_NAME];
            mc_host_from_utf8(line + at, name, MC_HOST_NAME);
            if (name[0]) set_record(ids, (uint32_t)id, (uint32_t)parent, name, index);
        } else if (line[0] == 'D' && sscanf(line, "D\t%lu", &id) == 1) remove_record(ids, (uint32_t)id);
        else if (line[0] == 'N' && sscanf(line, "N\t%lu", &id) == 1 && id > ids->next) ids->next = (uint32_t)id;
        line = end + 1;
    }
    free(text);
}

mc_afp_ids *mc_ids_open(const uint16_t *path, uint64_t root_index) {
    mc_afp_ids *ids = calloc(1, sizeof *ids);
    if (!ids) return NULL;
    ids->next = MC_AFP_FIRST_ID;
    (void)root_index;
    if (path) {
        mc_host_copy_str(ids->path, path, MC_HOST_PATH);
        mc_host_file *f;
        if (mc_host_open(path, 0, MC_HOST_OPEN, &f) == MC_AFP_OK) {
            load(ids, f);
            mc_host_close(f);
        }
        /* Compacted: the records as they are, then appended to. */
        if (mc_host_open(path, 1, MC_HOST_OPEN_ALWAYS, &ids->log) == MC_AFP_OK) {
            mc_host_set_attribute(path, MC_HOST_HIDDEN, 1);
            mc_host_truncate(ids->log, 0);
            ids->log_end = 0;
            char head[64];
            snprintf(head, sizeof head, "# Livadix AFP IDs\nN\t%lu\n", (unsigned long)ids->next);
            log_line(ids, head);
            for (size_t k = 0; k < ids->count; k++)
                if (ids->r[k].id) log_record(ids, &ids->r[k]);
        }
    }
    return ids;
}

void mc_ids_close(mc_afp_ids *ids) {
    if (!ids) return;
    for (size_t k = 0; k < ids->count; k++) free(ids->r[k].name);
    free(ids->r);
    table_free(&ids->by_id);
    table_free(&ids->by_name);
    table_free(&ids->by_index);
    if (ids->log) {
        mc_host_flush(ids->log);
        mc_host_close(ids->log);
    }
    free(ids);
}

unsigned mc_ids_count(const mc_afp_ids *ids) {
    unsigned n = 0;
    for (size_t k = 0; k < ids->count; k++) n += ids->r[k].id ? 1u : 0u;
    return n;
}

uint32_t mc_ids_find(mc_afp_ids *ids, uint32_t parent, const uint16_t *name) {
    probe p = {ids, 0, parent, name, 0};
    const uint32_t slot = table_find(&ids->by_name, name_key(parent, name), match_name, &p);
    return slot == UINT32_MAX ? 0 : ids->r[slot].id;
}

uint32_t mc_ids_get(mc_afp_ids *ids, uint32_t parent, const uint16_t *name, uint64_t index) {
    if (index) {
        probe p = {ids, 0, parent, name, index};
        const uint32_t slot = table_find(&ids->by_index, index, match_index, &p);
        if (slot != UINT32_MAX) {
            record *r = &ids->r[slot];
            if (r->parent != parent || !same_name(r->name, name) || mc_host_len(r->name) != mc_host_len(name) ||
                memcmp(r->name, name, mc_host_len(name) * 2)) {
                /* Renamed or moved since: the ID follows the file. A record
                   still holding the new place (a file replaced on the
                   Windows side) gives way. */
                const uint32_t other = mc_ids_find(ids, parent, name);
                if (other && other != r->id) mc_ids_forget(ids, other);
                const uint32_t id = r->id;
                set_record(ids, id, parent, name, index);
                log_record(ids, &ids->r[slot_of_id(ids, id)]);
            }
            return r->id;
        }
    }
    const uint32_t found = mc_ids_find(ids, parent, name);
    if (found) {
        const uint32_t slot = slot_of_id(ids, found);
        if (index && ids->r[slot].index != index) {
            /* same name, another file (replaced): its own ID when the old one had a file ID */
            if (ids->r[slot].index) {
                mc_ids_forget(ids, found);
            } else {
                set_record(ids, found, parent, name, index);
                log_record(ids, &ids->r[slot]);
                return found;
            }
        } else return found;
    }
    const uint32_t id = ids->next;
    set_record(ids, id, parent, name, index);
    const uint32_t slot = slot_of_id(ids, id);
    if (slot != UINT32_MAX) log_record(ids, &ids->r[slot]);
    return id;
}

int mc_ids_lookup(mc_afp_ids *ids, uint32_t id, uint32_t *parent, uint16_t name[MC_HOST_NAME], uint64_t *index) {
    const uint32_t slot = slot_of_id(ids, id);
    if (slot == UINT32_MAX) return 0;
    if (parent) *parent = ids->r[slot].parent;
    if (name) mc_host_copy_str(name, ids->r[slot].name, MC_HOST_NAME);
    if (index) *index = ids->r[slot].index;
    return 1;
}

void mc_ids_move(mc_afp_ids *ids, uint32_t id, uint32_t parent, const uint16_t *name, uint64_t index) {
    const uint32_t other = mc_ids_find(ids, parent, name);
    if (other && other != id) mc_ids_forget(ids, other);
    set_record(ids, id, parent, name, index);
    const uint32_t slot = slot_of_id(ids, id);
    if (slot != UINT32_MAX) log_record(ids, &ids->r[slot]);
}

void mc_ids_forget(mc_afp_ids *ids, uint32_t id) {
    if (slot_of_id(ids, id) == UINT32_MAX) return;
    remove_record(ids, id);
    char line[32];
    snprintf(line, sizeof line, "D\t%lu\n", (unsigned long)id);
    log_line(ids, line);
}

/* ---- the desktop database ------------------------------------------------------ */

typedef struct icon {
    uint8_t creator[4], type[4], itype;
    uint32_t tag;
    size_t length;
    uint8_t *bitmap;
} icon;

typedef struct appl {
    uint8_t creator[4];
    uint32_t dir, tag;
    uint8_t name[32];
    size_t n;
} appl;

typedef struct comment {
    uint32_t id;
    size_t n;
    uint8_t text[199];
} comment;

struct mc_afp_desktop {
    uint16_t path[MC_HOST_PATH];
    icon *icons;
    size_t nicons;
    appl *appls;
    size_t nappls;
    comment *comments;
    size_t ncomments;
    int loading;
};

static void p32(uint8_t *p, uint32_t v) { p[0] = (uint8_t)(v >> 24); p[1] = (uint8_t)(v >> 16); p[2] = (uint8_t)(v >> 8); p[3] = (uint8_t)v; }
static uint32_t g32(const uint8_t *p) { return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3]; }

/* The file: "MDT1", then records: 'I' creator type itype tag length bitmap;
 * 'A' creator dir tag n name; 'C' id n text. Written whole after each change. */
static void save(mc_afp_desktop *dt) {
    if (!dt->path[0] || dt->loading) return;
    size_t size = 4;
    for (size_t k = 0; k < dt->nicons; k++) size += 1 + 4 + 4 + 1 + 4 + 4 + dt->icons[k].length;
    size += dt->nappls * (1 + 4 + 4 + 4 + 1 + 32) + dt->ncomments * (1 + 4 + 1 + 199);
    uint8_t *b = malloc(size), *p = b;
    if (!b) return;
    memcpy(p, "MDT1", 4);
    p += 4;
    for (size_t k = 0; k < dt->nicons; k++) {
        const icon *i = &dt->icons[k];
        *p++ = 'I';
        memcpy(p, i->creator, 4);
        memcpy(p + 4, i->type, 4);
        p[8] = i->itype;
        p32(p + 9, i->tag);
        p32(p + 13, (uint32_t)i->length);
        memcpy(p + 17, i->bitmap, i->length);
        p += 17 + i->length;
    }
    for (size_t k = 0; k < dt->nappls; k++) {
        const appl *a = &dt->appls[k];
        *p++ = 'A';
        memcpy(p, a->creator, 4);
        p32(p + 4, a->dir);
        p32(p + 8, a->tag);
        p[12] = (uint8_t)a->n;
        memcpy(p + 13, a->name, a->n);
        p += 13 + a->n;
    }
    for (size_t k = 0; k < dt->ncomments; k++) {
        const comment *c = &dt->comments[k];
        *p++ = 'C';
        p32(p, c->id);
        p[4] = (uint8_t)c->n;
        memcpy(p + 5, c->text, c->n);
        p += 5 + c->n;
    }
    mc_host_file *f;
    if (mc_host_open(dt->path, 1, MC_HOST_OPEN_ALWAYS, &f) == MC_AFP_OK) {
        mc_host_write(f, 0, b, (size_t)(p - b));
        mc_host_truncate(f, (uint64_t)(p - b));
        mc_host_close(f);
        mc_host_set_attribute(dt->path, MC_HOST_HIDDEN, 1);
    }
    free(b);
}

static void load_desktop(mc_afp_desktop *dt) {
    mc_host_file *f;
    if (mc_host_open(dt->path, 0, MC_HOST_OPEN, &f)) return;
    uint64_t size = 0;
    uint8_t *b = NULL;
    size_t got = 0;
    if (!mc_host_size(f, &size) && size >= 4 && size < 64u * 1024 * 1024 && (b = malloc((size_t)size)) &&
        !mc_host_read(f, 0, b, (size_t)size, &got) && got == size && !memcmp(b, "MDT1", 4)) {
        const uint8_t *p = b + 4, *end = b + size;
        while (p < end) {
            const uint8_t kind = *p++;
            if (kind == 'I' && end - p >= 17) {
                const uint32_t len = g32(p + 13);
                if ((size_t)(end - p) < 17 + (size_t)len) break;
                mc_desktop_add_icon(dt, p, p + 4, p[8], g32(p + 9), p + 17, len);
                p += 17 + len;
            } else if (kind == 'A' && end - p >= 13 && p[12] <= 31 && end - p >= 13 + p[12]) {
                mc_desktop_add_appl(dt, p, g32(p + 4), p + 13, p[12], g32(p + 8));
                p += 13 + p[12];
            } else if (kind == 'C' && end - p >= 5 && p[4] <= 199 && end - p >= 5 + p[4]) {
                mc_desktop_set_comment(dt, g32(p), p + 5, p[4]);
                p += 5 + p[4];
            } else break;
        }
    }
    free(b);
    mc_host_close(f);
}

mc_afp_desktop *mc_desktop_open(const uint16_t *path) {
    mc_afp_desktop *dt = calloc(1, sizeof *dt);
    if (!dt) return NULL;
    if (path) {
        mc_host_copy_str(dt->path, path, MC_HOST_PATH);
        dt->loading = 1;
        load_desktop(dt);
        dt->loading = 0;
    }
    return dt;
}

void mc_desktop_close(mc_afp_desktop *dt) {
    if (!dt) return;
    for (size_t k = 0; k < dt->nicons; k++) free(dt->icons[k].bitmap);
    free(dt->icons);
    free(dt->appls);
    free(dt->comments);
    free(dt);
}

int mc_desktop_add_icon(mc_afp_desktop *dt, const uint8_t creator[4], const uint8_t type[4], uint8_t itype, uint32_t tag,
                        const uint8_t *bitmap, size_t length) {
    icon *i = NULL;
    for (size_t k = 0; k < dt->nicons && !i; k++)
        if (!memcmp(dt->icons[k].creator, creator, 4) && !memcmp(dt->icons[k].type, type, 4) && dt->icons[k].itype == itype)
            i = &dt->icons[k];
    if (i && i->length != length) return MC_AFP_ICON_TYPE_ERR;
    uint8_t *copy = malloc(length ? length : 1);
    if (!copy) return MC_AFP_MISC_ERR;
    memcpy(copy, bitmap, length);
    if (!i) {
        icon *grown = realloc(dt->icons, (dt->nicons + 1) * sizeof *grown);
        if (!grown) { free(copy); return MC_AFP_MISC_ERR; }
        dt->icons = grown;
        i = &dt->icons[dt->nicons++];
        memcpy(i->creator, creator, 4);
        memcpy(i->type, type, 4);
        i->itype = itype;
        i->bitmap = NULL;
    }
    free(i->bitmap);
    i->bitmap = copy;
    i->length = length;
    i->tag = tag;
    save(dt);
    return MC_AFP_OK;
}

const uint8_t *mc_desktop_icon(mc_afp_desktop *dt, const uint8_t creator[4], const uint8_t type[4], uint8_t itype, size_t *length,
                               uint32_t *tag) {
    for (size_t k = 0; k < dt->nicons; k++) {
        const icon *i = &dt->icons[k];
        if (!memcmp(i->creator, creator, 4) && !memcmp(i->type, type, 4) && i->itype == itype) {
            *length = i->length;
            if (tag) *tag = i->tag;
            return i->bitmap;
        }
    }
    return NULL;
}

int mc_desktop_icon_info(mc_afp_desktop *dt, const uint8_t creator[4], unsigned index, uint8_t type[4], uint8_t *itype, uint32_t *tag,
                         size_t *length) {
    unsigned n = 0;
    for (size_t k = 0; k < dt->nicons; k++) {
        const icon *i = &dt->icons[k];
        if (memcmp(i->creator, creator, 4) || ++n != index) continue;
        memcpy(type, i->type, 4);
        *itype = i->itype;
        *tag = i->tag;
        *length = i->length;
        return 1;
    }
    return 0;
}

int mc_desktop_add_appl(mc_afp_desktop *dt, const uint8_t creator[4], uint32_t dir, const uint8_t *name, size_t n, uint32_t tag) {
    if (n > 31) return MC_AFP_PARAM_ERR;
    appl *a = NULL;
    for (size_t k = 0; k < dt->nappls && !a; k++)
        if (!memcmp(dt->appls[k].creator, creator, 4) && dt->appls[k].dir == dir && mc_mac_name_equal(dt->appls[k].name, dt->appls[k].n, name, n))
            a = &dt->appls[k];
    if (!a) {
        appl *grown = realloc(dt->appls, (dt->nappls + 1) * sizeof *grown);
        if (!grown) return MC_AFP_MISC_ERR;
        dt->appls = grown;
        a = &dt->appls[dt->nappls++];
        memcpy(a->creator, creator, 4);
        a->dir = dir;
    }
    memcpy(a->name, name, n);
    a->n = n;
    a->tag = tag;
    save(dt);
    return MC_AFP_OK;
}

int mc_desktop_remove_appl(mc_afp_desktop *dt, const uint8_t creator[4], uint32_t dir, const uint8_t *name, size_t n) {
    for (size_t k = 0; k < dt->nappls; k++) {
        appl *a = &dt->appls[k];
        if (memcmp(a->creator, creator, 4) || a->dir != dir || !mc_mac_name_equal(a->name, a->n, name, n)) continue;
        dt->appls[k] = dt->appls[--dt->nappls];
        save(dt);
        return MC_AFP_OK;
    }
    return MC_AFP_ITEM_NOT_FOUND;
}

int mc_desktop_appl(mc_afp_desktop *dt, const uint8_t creator[4], unsigned index, uint32_t *dir, uint8_t name[32], size_t *n,
                    uint32_t *tag) {
    unsigned count = 0;
    for (size_t k = 0; k < dt->nappls; k++) {
        const appl *a = &dt->appls[k];
        if (memcmp(a->creator, creator, 4) || ++count != index) continue;
        *dir = a->dir;
        memcpy(name, a->name, a->n);
        *n = a->n;
        *tag = a->tag;
        return 1;
    }
    return 0;
}

int mc_desktop_set_comment(mc_afp_desktop *dt, uint32_t id, const uint8_t *text, size_t n) {
    if (n > 199) n = 199;
    comment *c = NULL;
    for (size_t k = 0; k < dt->ncomments && !c; k++)
        if (dt->comments[k].id == id) c = &dt->comments[k];
    if (!c) {
        comment *grown = realloc(dt->comments, (dt->ncomments + 1) * sizeof *grown);
        if (!grown) return MC_AFP_MISC_ERR;
        dt->comments = grown;
        c = &dt->comments[dt->ncomments++];
        c->id = id;
    }
    memcpy(c->text, text, n);
    c->n = n;
    save(dt);
    return MC_AFP_OK;
}

const uint8_t *mc_desktop_comment(mc_afp_desktop *dt, uint32_t id, size_t *n) {
    for (size_t k = 0; k < dt->ncomments; k++)
        if (dt->comments[k].id == id) {
            *n = dt->comments[k].n;
            return dt->comments[k].text;
        }
    return NULL;
}

int mc_desktop_remove_comment(mc_afp_desktop *dt, uint32_t id) {
    for (size_t k = 0; k < dt->ncomments; k++)
        if (dt->comments[k].id == id) {
            dt->comments[k] = dt->comments[--dt->ncomments];
            save(dt);
            return MC_AFP_OK;
        }
    return MC_AFP_ITEM_NOT_FOUND;
}
