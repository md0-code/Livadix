/* livadix.yaml (docs/NOTES.md). libyaml handles YAML syntax; this file
 * enforces Livadix's schema: unknown keys, duplicate keys, bad values and extra
 * documents are refused with a message naming what is wrong and its line.
 * The top-level sections are shared by every profile; a profile (under
 * profiles:) overrides any key of them and names its model. */
#ifdef _WIN32
#define _CRT_RAND_S
#endif
#include "config.h"
#include "machine.h"
#include "models.h"
#include "romdb.h"
#include "compat.h"
#include "portable.h"
#include <yaml.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <stdarg.h>
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <sys/stat.h>
#include <dirent.h>
#include <limits.h>
#include <unistd.h>
#endif

#ifndef _WIN32
/* realpath writes up to PATH_MAX bytes: into a buffer of that size, then to the caller's. */
static int resolve_path(const char *path, char *out, size_t capacity) {
    char full[PATH_MAX];
    if (!realpath(path, full) || strlen(full) >= capacity) return 0;
    strcpy(out, full);
    return 1;
}
#endif

/* The lines of the keys whose values are checked once the file is read. */
typedef struct key_lines { size_t ram, resolution, depth, refresh, monitor, mouse, storage, model; } key_lines;
typedef struct reader {
    yaml_document_t *doc; char *error; size_t cap;
    size_t line;               /* the line being read, for messages */
    const char *profile;       /* the profile being read, for messages */
    key_lines at;
} reader;
static int failf(reader *r, const char *format, ...) {
    if (!r->cap) return 0;
    size_t used = 0;
    if (r->line && r->profile) used = (size_t)snprintf(r->error, r->cap, "line %zu (profile \"%s\"): ", r->line, r->profile);
    else if (r->line) used = (size_t)snprintf(r->error, r->cap, "line %zu: ", r->line);
    else if (r->profile) used = (size_t)snprintf(r->error, r->cap, "profile \"%s\": ", r->profile);
    if (used < r->cap) {
        va_list args;
        va_start(args, format);
        vsnprintf(r->error + used, r->cap - used, format, args);
        va_end(args);
    }
    return 0;
}
static int fail(reader *r, const char *message) { return failf(r, "%s", message); }
static yaml_node_t *node(reader *r, int index) {
    yaml_node_t *n = r->doc ? yaml_document_get_node(r->doc, index) : NULL;
    if (n) r->line = n->start_mark.line + 1;
    return n;
}
static const char *scalar(reader *r, yaml_node_t *n) {
    if (n) r->line = n->start_mark.line + 1;
    if (!n || n->type != YAML_SCALAR_NODE || strlen((char *)n->data.scalar.value) != n->data.scalar.length) {
        fail(r, "Expected a scalar without embedded NUL characters"); return NULL;
    }
    return (const char *)n->data.scalar.value;
}
static int path_value(reader *r, yaml_node_t *n, const char *base, char *out) {
    const char *s = scalar(r, n);
    if (!s || !*s) return fail(r, "Path cannot be empty");
    int absolute = s[0] == '/' || s[0] == '\\' || (s[0] && s[1] == ':');
    int length = absolute ? snprintf(out, MC_CONFIG_PATH, "%s", s) : snprintf(out, MC_CONFIG_PATH, "%s%s", base, s);
    return length >= 0 && length < MC_CONFIG_PATH ? 1 : fail(r, "Path is too long");
}
static int number(reader *r, yaml_node_t *n, uint32_t *out) {
    const char *s = scalar(r, n);
    char *end;
    if (!s || !*s || *s < '0' || *s > '9') return fail(r, "Expected an unsigned decimal integer");
    errno = 0;
    unsigned long v = strtoul(s, &end, 10);
    if (errno || *end || v > UINT32_MAX) return fail(r, "Invalid or overflowing integer");
    *out = (uint32_t)v;
    return 1;
}
uint64_t mc_config_parse_mhz(const char *s) {
    if (!s || *s < '0' || *s > '9') return 0;
    uint64_t whole = 0, frac = 0;
    unsigned digits = 0;
    for (; *s >= '0' && *s <= '9'; s++) {
        whole = whole * 10u + (uint64_t)(*s - '0');
        if (whole > 10000u) return 0;
    }
    if (*s == '.') {
        s++;
        if (*s < '0' || *s > '9') return 0;
        for (; *s >= '0' && *s <= '9'; s++) {
            if (++digits > 6) return 0;
            frac = frac * 10u + (uint64_t)(*s - '0');
        }
    }
    if (*s) return 0;
    while (digits++ < 6) frac *= 10u;
    const uint64_t hz = whole * 1000000u + frac;
    return hz >= MC_CONFIG_MIN_HZ && hz <= MC_CONFIG_MAX_HZ ? hz : 0;
}
const char *mc_config_mhz_text(uint64_t hz, char *buffer, size_t capacity) {
    unsigned long long frac = (unsigned long long)(hz % 1000000u);
    int digits = 6;
    while (digits && frac % 10u == 0) { frac /= 10u; digits--; }
    if (digits) snprintf(buffer, capacity, "%llu.%0*llu", (unsigned long long)(hz / 1000000u), digits, frac);
    else snprintf(buffer, capacity, "%llu", (unsigned long long)(hz / 1000000u));
    return buffer;
}
uint32_t mc_config_ram_kb(const mc_config *c) {
    return c->ram_kb && c->ram_kb / 1024u == c->ram_mb ? c->ram_kb : c->ram_mb * 1024u;
}
void mc_config_set_ram_kb(mc_config *c, uint32_t kb) {
    c->ram_mb = kb / 1024u;
    c->ram_kb = kb % 1024u ? kb : 0;
}
uint32_t mc_config_parse_ram(const char *s) {
    if (!s || *s < '0' || *s > '9') return 0;
    uint64_t whole = 0, frac = 0, scale = 1;
    for (; *s >= '0' && *s <= '9'; s++) {
        whole = whole * 10u + (uint64_t)(*s - '0');
        if (whole > 1024u) return 0;
    }
    if (*s == '.') {
        s++;
        if (*s < '0' || *s > '9') return 0;
        for (; *s >= '0' && *s <= '9'; s++) {
            if (scale == 1000u) return 0;
            frac = frac * 10u + (uint64_t)(*s - '0');
            scale *= 10u;
        }
    }
    if (*s || frac * 1024u % scale) return 0;
    const uint64_t kb = whole * 1024u + frac * 1024u / scale;
    return kb && kb <= 1024u * 1024u ? (uint32_t)kb : 0;
}
const char *mc_config_ram_text(uint32_t kb, char *buffer, size_t capacity) {
    unsigned frac = (unsigned)((uint64_t)(kb % 1024u) * 1000u / 1024u);
    int digits = 3;
    while (digits && frac % 10u == 0) { frac /= 10u; digits--; }
    if (digits) snprintf(buffer, capacity, "%u.%0*u", (unsigned)(kb / 1024u), digits, frac);
    else snprintf(buffer, capacity, "%u", (unsigned)(kb / 1024u));
    return buffer;
}
static int boolean(reader *r, yaml_node_t *n, int *out) {
    const char *s = scalar(r, n);
    if (s && strcmp(s, "true") == 0) { *out = 1; return 1; }
    if (s && strcmp(s, "false") == 0) { *out = 0; return 1; }
    return fail(r, "Boolean must be true or false");
}
const uint16_t mc_config_resolutions[MC_CONFIG_RESOLUTIONS][2] = {
    {512,342},{512,384},{640,400},{640,480},{800,600},{832,624},{1024,768},{1152,864},{1152,870},{1280,1024},{1600,1200},{1920,1080}};
const uint8_t mc_config_depths[MC_CONFIG_DEPTHS] = {1,2,4,8,16,32};
/* Named values of the choice settings. */
typedef struct named { const char *name; uint32_t value; } named;
static const named layouts[] = {{"usa",MC_KEYBOARD_ANSI},{"iso",MC_KEYBOARD_ISO},{NULL,0}};
/* "windows" is what files before the configuration window's Windows key
 * wrote: the default, Left Alt, now. */
static const named commands[] = {{"left-alt",MC_COMMAND_LEFT_ALT},{"windows",MC_COMMAND_LEFT_ALT},{"left-control",MC_COMMAND_LEFT_CONTROL},
                                 {"right-control",MC_COMMAND_RIGHT_CONTROL},{"right-alt",MC_COMMAND_RIGHT_ALT},{NULL,0}};
static const named buttons[] = {{"disabled",MC_MOUSE_DISABLED},{"normal",MC_MOUSE_NORMAL},{"control-click",MC_MOUSE_CONTROL_CLICK},{NULL,0}};
static const named priorities[] = {{"disk",MC_BOOT_DISK},{"cdrom",MC_BOOT_CDROM},{NULL,0}};
static const named buses[] = {{"scsi",MC_BUS_SCSI},{"ata",MC_BUS_ATA},{NULL,0}};
static const named looks[] = {{"plain",MC_LOOK_PLAIN},{"crt",MC_LOOK_CRT},{"lcd",MC_LOOK_LCD},{NULL,0}};
static const named monitors[] = {{"color",MC_MONITOR_COLOR},{"mono",MC_MONITOR_MONO},
                                  {"lcd-gray",MC_MONITOR_LCD_GRAY},{NULL,0}};
static const named filters[] = {{"nearest",MC_CONFIG_FILTER_NEAREST},{"linear",MC_CONFIG_FILTER_LINEAR},{"sharp",MC_CONFIG_FILTER_SHARP},{NULL,0}};
static const named scales[] = {{"fit",MC_CONFIG_SCALE_FIT},{"integer",MC_CONFIG_SCALE_INTEGER},{NULL,0}};
static const named cards[] = {{"auto",MC_VIDEO_CARD_AUTO},{"builtin",MC_VIDEO_CARD_BUILTIN},{"livadix",MC_VIDEO_CARD_LIVADIX},{NULL,0}};
static const char *name_of(const named *names, uint32_t value) {
    for (; names->name; ++names) if (names->value == value) return names->name;
    return "?";
}
const char *mc_config_look_name(uint32_t v) { return name_of(looks, v); }
const char *mc_config_monitor_name(uint32_t v) { return name_of(monitors, v); }
int mc_config_look_ok(const mc_model *m, uint32_t look) {
    return look == MC_LOOK_PLAIN || look == (uint32_t)(m->look == MC_LOOK_LCD ? MC_LOOK_LCD : MC_LOOK_CRT);
}
uint32_t mc_config_look_fit(const mc_model *m, uint32_t look) {
    return mc_config_look_ok(m, look) ? look : (uint32_t)(m->look == MC_LOOK_LCD ? MC_LOOK_LCD : MC_LOOK_CRT);
}
int mc_config_monitor_check(const mc_model *m, unsigned w, unsigned h, unsigned depth, uint32_t monitor, char *why, size_t cap) {
    const unsigned bit = 1u << monitor;
    const int modular = (m->monitors & MC_MON_MODULAR_MONO) != 0;
    if (modular && monitor == MC_MONITOR_MONO) {
        if (((w == 640 && (h == 480 || h == 870)) || (w == 1152 && h == 870)) && depth <= 8) return 1;
        snprintf(why, cap, "mono is one of Apple's monochrome displays (640x480, 640x870 or 1152x870, 1 to 8 bits); "
                 "video.resolution is %ux%u at %u bits", w, h, depth);
        return 0;
    }
    if (m->monitors & bit) return 1;
    if (modular)
        snprintf(why, cap, "the %s has no built-in screen: lcd-gray is the PowerBooks' grayscale LCD, so video.monitor is color or mono",
                 m->name);
    else {
        const char *screen = m->monitors & MC_MON_MONO ? "monochrome" : m->monitors & MC_MON_LCD_GRAY ? "grayscale LCD" : "color";
        const uint32_t only = m->monitors & MC_MON_MONO ? MC_MONITOR_MONO : m->monitors & MC_MON_LCD_GRAY ? MC_MONITOR_LCD_GRAY
                                                                                                          : MC_MONITOR_COLOR;
        snprintf(why, cap, "the %s has a built-in %s screen, so video.monitor is %s", m->name, screen, mc_config_monitor_name(only));
    }
    return 0;
}
const char *mc_config_filter_name(uint32_t v) { return name_of(filters, v); }
const char *mc_config_scale_name(uint32_t v) { return name_of(scales, v); }
const char *mc_config_video_card_name(uint32_t v) { return name_of(cards, v); }
static const named controllers[] = {{"auto",MC_CONTROLLER_AUTO},{"protocol",MC_CONTROLLER_PROTOCOL},{"firmware",MC_CONTROLLER_FIRMWARE},{NULL,0}};
const char *mc_config_controller_name(uint32_t v) { return name_of(controllers, v); }
static const named networks[] = {{"nat",MC_NETWORK_NAT},{"none",MC_NETWORK_NONE},{NULL,0}};
const char *mc_config_network_name(uint32_t v) { return name_of(networks, v); }
static const named net_cards[] = {{"auto",MC_NET_CARD_AUTO},{"none",MC_NET_CARD_NONE},{"nubus",MC_NET_CARD_NUBUS},{"lc",MC_NET_CARD_LC},{NULL,0}};
const char *mc_config_network_card_name(uint32_t v) { return name_of(net_cards, v); }
static const named close_actions[] = {{"shutdown",MC_CLOSE_SHUTDOWN},{"quit",MC_CLOSE_QUIT},{NULL,0}};
const char *mc_config_close_action_name(uint32_t v) { return name_of(close_actions, v); }
static int choice(reader *r, yaml_node_t *n, const named *names, uint32_t *out) {
    const char *s = scalar(r, n);
    if (!s) return 0;
    for (; names->name; ++names) if (!strcmp(s, names->name)) { *out = names->value; return 1; }
    return fail(r, "Unknown setting value; see docs/NOTES.md");
}
static int mapping(reader *r, yaml_node_t *n) {
    if (n) r->line = n->start_mark.line + 1;
    if (!n || n->type != YAML_MAPPING_NODE) return fail(r, "Expected a mapping");
    for (yaml_node_pair_t *p = n->data.mapping.pairs.start; p < n->data.mapping.pairs.top; ++p) {
        const char *key = scalar(r, node(r, p->key));
        if (!key) return 0;
        for (yaml_node_pair_t *q = n->data.mapping.pairs.start; q < p; ++q) {
            const char *previous = scalar(r, node(r, q->key));
            if (!previous || strcmp(key, previous) == 0) return fail(r, "Duplicate configuration key");
        }
    }
    return 1;
}
static int disk_config(reader *r, yaml_node_t *n, const char *base, mc_disk_config *disk, int floppy) {
    if (!mapping(r, n)) return 0;
    for (yaml_node_pair_t *p = n->data.mapping.pairs.start; p < n->data.mapping.pairs.top; ++p) {
        const char *key = scalar(r, node(r, p->key));
        yaml_node_t *v = node(r, p->value);
        int ok;
        if (strcmp(key, "image") == 0) ok = path_value(r, v, base, disk->image);
        else if (strcmp(key, "readonly") == 0) ok = boolean(r, v, &disk->readonly);
        else if (!floppy && strcmp(key, "scsi_id") == 0) {
            uint32_t id = 0;
            ok = number(r, v, &id);
            /* 7 is the Macintosh, 3 the CD-ROM drive (NOTES.md). */
            if (ok && (id > 6 || id == 3)) return fail(r, "scsi_id must be 0, 1, 2, 4, 5 or 6");
            disk->scsi_id_set = 1;
            disk->scsi_id = (int)id;
        }
        else if (!floppy && strcmp(key, "bus") == 0) {
            uint32_t bus = MC_BUS_SCSI;
            ok = choice(r, v, buses, &bus);
            disk->bus = (int)bus;
        }
        else return fail(r, floppy ? "Unknown floppy key; expected image or readonly"
                                   : "Unknown disk key; expected image, readonly, scsi_id or bus");
        if (!ok) return 0;
    }
    if (disk->scsi_id_set && disk->bus == MC_BUS_ATA) return fail(r, "A disk on bus ata takes no scsi_id");
    if (!disk->image[0]) {
        if (floppy && !disk->readonly) return 1;
        return fail(r, floppy ? "An empty floppy drive takes no other key" : "Disk image is required");
    }
    return 1;
}
static int port_config(reader *r,yaml_node_t *n,const char *base,mc_port_config *port) {
    if(!mapping(r,n))return 0;
    if(n->data.mapping.pairs.top-n->data.mapping.pairs.start!=1)return fail(r,"Port requires exactly one of serial, pipe, output or disabled");
    yaml_node_pair_t *p=n->data.mapping.pairs.start;
    const char *key=scalar(r,node(r,p->key));yaml_node_t *v=node(r,p->value);
    if(!strcmp(key,"disabled")) {
        int disabled=0;if(!boolean(r,v,&disabled) || !disabled)return fail(r,"disabled must be true");
        port->type=MC_PORT_DISABLED;return 1;
    }
    if(!strcmp(key,"output")) { port->type=MC_PORT_OUTPUT;return path_value(r,v,base,port->path); }
    const char *value=scalar(r,v);if(!value)return 0;
    if(!strcmp(key,"serial")) {
#ifdef _WIN32
        if(_strnicmp(value,"COM",3) || value[3]<'1' || value[3]>'9')return fail(r,"Serial device must be COM followed by a positive port number");
        for(const char *s=value+4;*s;++s)if(*s<'0' || *s>'9')return fail(r,"Invalid COM port number");
        if(strlen(value)>12)return fail(r,"COM port number is too long");
#else
        if(strncmp(value,"/dev/",5) || !value[5] || strstr(value,"/../"))return fail(r,"Serial device must be a /dev/ device such as /dev/ttyUSB0");
#endif
        port->type=MC_PORT_SERIAL;
    } else if(!strcmp(key,"pipe")) {
#ifdef _WIN32
        if(strncmp(value,"\\\\.\\pipe\\",9) || !value[9])return fail(r,"Pipe must name a local \\\\.\\pipe\\ endpoint");
#else
        if(value[0]!='/' || !value[1])return fail(r,"Pipe must be the absolute path of a Unix-domain socket such as /tmp/livadix-modem.sock");
#endif
        port->type=MC_PORT_PIPE;
    } else return fail(r,"Unknown port key; expected serial, pipe, output or disabled");
    if(strlen(value)>=sizeof port->path)return fail(r,"Port path is too long");
    strcpy(port->path,value);return 1;
}
/* Identity of two existing files (hard links and path spellings included). */
static int same_file(const char *a, const char *b) {
#ifdef _WIN32
    HANDLE x=CreateFileA(a,0,FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE,NULL,OPEN_EXISTING,0,NULL);
    if(x==INVALID_HANDLE_VALUE)return 0;
    HANDLE y=CreateFileA(b,0,FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE,NULL,OPEN_EXISTING,0,NULL);
    BY_HANDLE_FILE_INFORMATION i,j;
    int same=y!=INVALID_HANDLE_VALUE && GetFileInformationByHandle(x,&i) && GetFileInformationByHandle(y,&j) &&
        i.dwVolumeSerialNumber==j.dwVolumeSerialNumber && i.nFileIndexHigh==j.nFileIndexHigh && i.nFileIndexLow==j.nFileIndexLow;
    if(y!=INVALID_HANDLE_VALUE)CloseHandle(y);
    CloseHandle(x);return same;
#else
    struct stat x,y;
    return !stat(a,&x) && !stat(b,&y) && x.st_dev==y.st_dev && x.st_ino==y.st_ino;
#endif
}
/* An image written through one drive must not be visible through another. */
static int shared_writable_image(const mc_config *c) {
    const char *paths[MC_CONFIG_DISKS+2+1]; int writable[MC_CONFIG_DISKS+2+1]; size_t n=0;
    for(size_t i=0;i<c->disk_count;++i) { paths[n]=c->disks[i].image; writable[n++]=!c->disks[i].readonly; }
    for(size_t i=0;i<c->floppy_count;++i)if(c->floppies[i].image[0]) { paths[n]=c->floppies[i].image; writable[n++]=!c->floppies[i].readonly; }
    if(c->cdrom[0]) { paths[n]=c->cdrom; writable[n++]=0; }
    for(size_t i=0;i<n;++i)for(size_t j=i+1;j<n;++j)
        if((writable[i] || writable[j]) && (!strcmp(paths[i],paths[j]) || same_file(paths[i],paths[j])))return 1;
    return 0;
}
void mc_config_defaults(mc_config *c) {
    memset(c, 0, sizeof *c);
    const mc_model *p = mc_model_find(MC_MODEL_BUILTIN);
    c->model = (uint32_t)mc_model_index(p);
    c->ram_mb = p->ram_mb; c->ram_kb = 0; c->cpu_hz = p->cpu_hz; c->width = p->width; c->height = p->height; c->depth = p->depth;
    c->look = p->look; c->monitor = p->monitor; c->refresh = 60;
    c->floppy_count = 1; c->floppy_off[1] = 1;
    c->keyboard_layout = MC_KEYBOARD_ANSI; c->command_key = MC_COMMAND_LEFT_ALT;
    c->mouse[0] = c->mouse[1] = MC_MOUSE_NORMAL; c->sound = 1; c->floppy_sound = 1; c->jit = 1; c->startup_sound = 1;
    c->filter = MC_CONFIG_FILTER_SHARP; c->scale = MC_CONFIG_SCALE_FIT; c->vsync = 1; c->video_acceleration = 1;
    c->close_action = MC_CLOSE_SHUTDOWN; c->close_timeout = 30;
    strcpy(c->sharing_name, "Livadix");
}
/* mc_config_parse_values: the values only, without the rules a model
 * imposes on them (its memory, screens, monitors, drives). */
static int lenient;
/* The keys (SET_*) the profile being read writes itself; the others come
 * from the shared settings, which a profile's model may not take all of: a
 * shared value the model does not take gives way to the model's own. In a
 * file without profiles every key is its own. */
static unsigned own_keys;
/* Keys a model supplies unless the file writes them. */
enum { SET_ROM = 1, SET_RAM = 2, SET_MHZ = 4, SET_WIDTH = 8, SET_HEIGHT = 16, SET_DEPTH = 32, SET_LOOK = 64,
       SET_MONITOR = 128 };
static int apply_model(reader *r, mc_config *c, const char *key, unsigned set) {
    /* No model (or auto, which chose one before 0.35): the Built-in machine. */
    if (!key || !strcmp(key, "auto")) key = MC_MODEL_BUILTIN;
    const mc_model *p = mc_model_find(key);
    if (!p) return fail(r, "Unknown model; livadix --list-models lists them");
    c->model = (uint32_t)mc_model_index(p);
    c->model_keys = set;
    /* The Built-in machine keeps its 128 KB and its screen whatever the
     * shared settings say: it is every profile's fallback. */
    const unsigned own = mc_model_builtin(p) ? 0 : set;
    if (!(own & SET_RAM)) mc_config_set_ram_kb(c, p->ram_mb * 1024u);
    if (!(set & SET_MHZ)) c->cpu_hz = p->cpu_hz;
    if (!(own & SET_WIDTH)) c->width = p->width;
    if (!(own & SET_HEIGHT)) c->height = p->height;
    if (!(own & SET_DEPTH)) c->depth = p->depth;
    if (!(set & SET_LOOK)) c->look = p->look;
    if (!(own & SET_MONITOR)) c->monitor = p->monitor;
    if (mc_config_ram_kb(c) > p->ram_max_mb * 1024u && !(own_keys & SET_RAM)) mc_config_set_ram_kb(c, p->ram_max_mb * 1024u);
    /* A size the board's sockets cannot hold: the next one they can (the
     * configuration window's rule, profile.h), else their largest. */
    if (!lenient && mc_config_ram_kb(c) <= p->ram_max_mb * 1024u) {
        uint32_t sizes[64];
        const size_t n = mc_machine_ram_sizes(p, sizes, 64);
        uint32_t fit = 0, most = 0;
        for (size_t i = 0; i < n; ++i) {
            if (sizes[i] > p->ram_max_mb * 1024u) continue;
            if (sizes[i] >= mc_config_ram_kb(c) && (!fit || sizes[i] < fit)) fit = sizes[i];
            if (sizes[i] > most) most = sizes[i];
        }
        if (n && (fit || most)) mc_config_set_ram_kb(c, fit ? fit : most);
    }
    if (!lenient && mc_config_ram_kb(c) > p->ram_max_mb * 1024u) {
        char ram[24];
        r->line = r->at.ram ? r->at.ram : r->at.model;
        return failf(r, "emulation.ram_mb: %s exceeds the %s's maximum of %u MiB", mc_config_ram_text(mc_config_ram_kb(c), ram, sizeof ram),
                     p->name, (unsigned)p->ram_max_mb);
    }
    c->rom_explicit = (set & SET_ROM) != 0;
    if (!c->rom_explicit) c->rom[0] = 0; /* mc_config_find_rom looks in the roms folders */
    return 1;
}
/* The sections of the file (docs/NOTES.md). Each takes a mapping
 * and rejects keys it does not know. */
#define PAIRS(v, q) for (yaml_node_pair_t *q = (v)->data.mapping.pairs.start; q < (v)->data.mapping.pairs.top; ++q)
static int emulation_section(reader *r, yaml_node_t *v, const char *base, mc_config *c, unsigned *set) {
    if (!mapping(r, v)) return 0;
    PAIRS(v, q) {
        const char *name = scalar(r, node(r, q->key));
        yaml_node_t *x = node(r, q->value);
        int ok;
        if (!strcmp(name, "rom")) { ok = path_value(r, x, base, c->rom); *set |= SET_ROM; }
        else if (!strcmp(name, "ram_mb")) {
            const uint32_t kb = mc_config_parse_ram(scalar(r, x));
            ok = kb ? 1 : fail(r, "emulation.ram_mb must be 1..1024 MiB, a whole number of KiB with up to three decimals (2.5)");
            if (kb) mc_config_set_ram_kb(c, kb);
            *set |= SET_RAM;
            r->at.ram = r->line;
        }
        else if (!strcmp(name, "cpu_mhz")) {
            const char *mhz = scalar(r, x);
            *set |= SET_MHZ;
            ok = mhz != NULL;
            if (ok && !(c->cpu_hz = mc_config_parse_mhz(mhz)))
                ok = fail(r, "emulation.cpu_mhz must be a number of MHz from 1 to 10000 (at most six decimals)");
        }
        else if (!strcmp(name, "jit")) ok = boolean(r, x, &c->jit);
        else if (!strcmp(name, "startup_sound")) ok = boolean(r, x, &c->startup_sound);
        else if (!strcmp(name, "fpu_full")) ok = boolean(r, x, &c->fpu_full);
        else if (!strcmp(name, "m68k_accelerator")) ok = boolean(r, x, &c->m68k_accelerator);
        else if (!strcmp(name, "charger_start")) ok = boolean(r, x, &c->charger_start);
        else if (!strcmp(name, "pmmu")) ok = boolean(r, x, &c->pmmu);
        else if (!strcmp(name, "nvram")) ok = path_value(r, x, base, c->nvram);
        else if (!strcmp(name, "nvram_id")) {
            const char *id = scalar(r, x);
            ok = id && strlen(id) == 32;
            for (unsigned j=0; ok && j<32; ++j) {
                char ch=id[j]; if(ch>='A'&&ch<='F')ch=(char)(ch+'a'-'A');
                if(!((ch>='0'&&ch<='9')||(ch>='a'&&ch<='f')))ok=0;
                c->nvram_id[j]=ch;
            }
            c->nvram_id[32]=0;
            if(!ok)ok=fail(r,"emulation.nvram_id must be 32 hexadecimal digits");
        }
        else if (!strcmp(name, "controller")) ok = choice(r, x, controllers, &c->controller);
        else if (!strcmp(name, "close_action")) ok = choice(r, x, close_actions, &c->close_action);
        else if (!strcmp(name, "close_timeout")) {
            ok = number(r, x, &c->close_timeout);
            if (ok && (c->close_timeout < 1 || c->close_timeout > 3600)) ok = fail(r, "emulation.close_timeout must be 1 to 3600");
        }
        else ok = fail(r, "Unknown emulation key; expected rom, ram_mb, cpu_mhz, jit, startup_sound, fpu_full, m68k_accelerator, charger_start, pmmu, controller, "
                          "nvram, nvram_id, close_action or close_timeout");
        if (!ok) return 0;
    }
    return 1;
}
/* video.resolution: one of the screen sizes, as WxH. */
static int resolution(reader *r, yaml_node_t *n, mc_config *c) {
    const char *s = scalar(r, n);
    if (!s) return 0;
    char name[16];
    for (unsigned i = 0; i < MC_CONFIG_RESOLUTIONS; ++i) {
        snprintf(name, sizeof name, "%ux%u", mc_config_resolutions[i][0], mc_config_resolutions[i][1]);
        if (!strcmp(s, name)) { c->width = mc_config_resolutions[i][0]; c->height = mc_config_resolutions[i][1]; return 1; }
    }
    char list[256];
    size_t used = 0;
    for (unsigned i = 0; i < MC_CONFIG_RESOLUTIONS && used < sizeof list; ++i) {
        int written = snprintf(list + used, sizeof list - used, "%s %ux%u", i ? "," : "", mc_config_resolutions[i][0], mc_config_resolutions[i][1]);
        if (written > 0) used += (size_t)written;
    }
    return failf(r, "video.resolution must be one of%s", list);
}
static int video_section(reader *r, yaml_node_t *v, mc_config *c, unsigned *set) {
    if (!mapping(r, v)) return 0;
    PAIRS(v, q) {
        const char *name = scalar(r, node(r, q->key));
        yaml_node_t *x = node(r, q->value);
        int ok;
        if (!strcmp(name, "look")) { *set |= SET_LOOK; ok = choice(r, x, looks, &c->look); }
        else if (!strcmp(name, "monitor")) { *set |= SET_MONITOR; r->at.monitor = r->line; ok = choice(r, x, monitors, &c->monitor); }
        else if (!strcmp(name, "filter")) ok = choice(r, x, filters, &c->filter);
        else if (!strcmp(name, "scale")) ok = choice(r, x, scales, &c->scale);
        else if (!strcmp(name, "fullscreen")) ok = boolean(r, x, &c->fullscreen);
        else if (!strcmp(name, "vsync")) ok = boolean(r, x, &c->vsync);
        else if (!strcmp(name, "resolution")) { *set |= SET_WIDTH | SET_HEIGHT; ok = resolution(r, x, c); r->at.resolution = r->line; }
        else if (!strcmp(name, "depth")) { *set |= SET_DEPTH; ok = number(r, x, &c->depth); r->at.depth = r->line; }
        else if (!strcmp(name, "refresh")) { ok = number(r, x, &c->refresh); r->at.refresh = r->line; }
        else if (!strcmp(name, "card")) ok = choice(r, x, cards, &c->video_card);
        else if (!strcmp(name, "acceleration")) ok = boolean(r, x, &c->video_acceleration);
        else return fail(r, "Unknown video key; expected resolution, depth, refresh, card, acceleration, look, monitor, filter, scale, fullscreen or vsync");
        if (!ok) return 0;
    }
    return 1;
}
static int storage_section(reader *r, yaml_node_t *v, const char *base, mc_config *c) {
    if (!mapping(r, v)) return 0;
    PAIRS(v, q) {
        const char *name = scalar(r, node(r, q->key));
        yaml_node_t *x = node(r, q->value);
        int ok = 1;
        r->at.storage = r->line;
        if (!strcmp(name, "disks")) {
            /* A profile's list replaces the shared one. */
            if (x->type != YAML_SEQUENCE_NODE) return fail(r, "storage.disks must be a sequence");
            c->disk_count = 0;
            memset(c->disks, 0, sizeof c->disks);
            for (yaml_node_item_t *i = x->data.sequence.items.start; ok && i < x->data.sequence.items.top; ++i) {
                if (c->disk_count == MC_CONFIG_DISKS) return fail(r, "At most ten file disks are supported");
                ok = disk_config(r, node(r, *i), base, &c->disks[c->disk_count++], 0);
                for (size_t k = 0; ok && k + 1 < c->disk_count; ++k)
                    if (c->disks[k].scsi_id_set && c->disks[c->disk_count - 1].scsi_id_set &&
                        c->disks[k].scsi_id == c->disks[c->disk_count - 1].scsi_id)
                        return fail(r, "Two disks have the same scsi_id");
            }
        }
        else if (!strcmp(name, "floppies")) {
            /* Drives in order; unlisted drives are not fitted. */
            if (x->type != YAML_SEQUENCE_NODE) return fail(r, "storage.floppies must be a sequence");
            c->floppy_count = 0; c->floppy_off[0] = c->floppy_off[1] = 1;
            memset(c->floppies, 0, sizeof c->floppies);
            for (yaml_node_item_t *i = x->data.sequence.items.start; ok && i < x->data.sequence.items.top; ++i) {
                if (c->floppy_count == 2) return fail(r, "At most two floppy drives are supported");
                yaml_node_t *item = node(r, *i); size_t index = c->floppy_count++;
                if (item && item->type == YAML_SCALAR_NODE) {
                    const char *s = scalar(r, item);
                    if (!s || strcmp(s, "off")) return fail(r, "A floppy drive is a mapping or off");
                    continue;
                }
                c->floppy_off[index] = 0;
                ok = disk_config(r, item, base, &c->floppies[index], 1);
            }
        }
        else if (!strcmp(name, "cdrom")) {
            /* A path, or a mapping with image and bus (ata: the drive in
             * the PowerBook 190's expansion bay, empty without an image). */
            c->cdrom[0] = 0;
            c->cdrom_bus = MC_BUS_SCSI;
            if (x && x->type == YAML_MAPPING_NODE) {
                PAIRS(x, d) {
                    const char *key = scalar(r, node(r, d->key));
                    yaml_node_t *y = node(r, d->value);
                    uint32_t bus = MC_BUS_SCSI;
                    if (!strcmp(key, "image")) ok = path_value(r, y, base, c->cdrom);
                    else if (!strcmp(key, "bus")) { ok = choice(r, y, buses, &bus); c->cdrom_bus = (int)bus; }
                    else ok = fail(r, "Unknown cdrom key; expected image or bus");
                    if (!ok) return 0;
                }
            }
            else ok = path_value(r, x, base, c->cdrom);
        }
        else if (!strcmp(name, "boot_priority")) ok = choice(r, x, priorities, &c->boot_priority);
        else ok = fail(r, "Unknown storage key; expected disks, floppies, cdrom or boot_priority");
        if (!ok) return 0;
    }
    return 1;
}
static int sound_section(reader *r, yaml_node_t *v, mc_config *c) {
    if (!mapping(r, v)) return 0;
    PAIRS(v, q) {
        const char *name = scalar(r, node(r, q->key));
        int *setting = !strcmp(name,"enabled") ? &c->sound : !strcmp(name,"floppy") ? &c->floppy_sound : NULL;
        if (!setting) return fail(r, "Unknown sound key; expected enabled or floppy");
        if (!boolean(r, node(r, q->value), setting)) return 0;
    }
    return 1;
}
static int input_section(reader *r, yaml_node_t *v, mc_config *c) {
    if (!mapping(r, v)) return 0;
    PAIRS(v, q) {
        const char *name = scalar(r, node(r, q->key));
        yaml_node_t *x = node(r, q->value);
        if (!strcmp(name, "keyboard")) {
            if (!mapping(r, x)) return 0;
            PAIRS(x, k) {
                const char *key = scalar(r, node(r, k->key));
                int ok = !strcmp(key, "layout") ? choice(r, node(r, k->value), layouts, &c->keyboard_layout) :
                         !strcmp(key, "command") ? choice(r, node(r, k->value), commands, &c->command_key) :
                         fail(r, "Unknown input.keyboard key; expected layout or command");
                if (!ok) return 0;
            }
        } else if (!strcmp(name, "mouse")) {
            if (!mapping(r, x)) return 0;
            r->at.mouse = r->line;
            PAIRS(x, k) {
                const char *key = scalar(r, node(r, k->key));
                int index = !strcmp(key, "left") ? 0 : !strcmp(key, "right") ? 1 : -1;
                if (index < 0) return fail(r, "Unknown input.mouse key; expected left or right");
                if (!choice(r, node(r, k->value), buttons, &c->mouse[index])) return 0;
            }
        } else return fail(r, "Unknown input key; expected keyboard or mouse");
    }
    return 1;
}
static int ports_section(reader *r, yaml_node_t *v, const char *base, mc_config *c) {
    if (!mapping(r, v)) return 0;
    PAIRS(v, q) {
        const char *name = scalar(r, node(r, q->key));
        int index = !strcmp(name, "a") ? 0 : !strcmp(name, "b") ? 1 : -1;
        if (index < 0) return fail(r, "Ports must be named a or b");
        if (!port_config(r, node(r, q->value), base, &c->ports[index])) return 0;
    }
    return 1;
}
/* network.forward entries (docs/NOTES.md, "Network"). */
static int port_number(const char *s, size_t n, uint16_t *out) {
    uint32_t v = 0;
    if (!n || n > 5) return 0;
    for (size_t i = 0; i < n; ++i) {
        if (s[i] < '0' || s[i] > '9') return 0;
        v = v * 10 + (uint32_t)(s[i] - '0');
    }
    if (v < 1 || v > 65535) return 0;
    *out = (uint16_t)v;
    return 1;
}
int mc_config_parse_forward(const char *text, mc_forward_config *f, char *error, size_t capacity) {
    memset(f, 0, sizeof *f);
    const char *fields[5];
    size_t lengths[5], count = 0;
    for (const char *p = text;;) {
        const char *colon = strchr(p, ':');
        if (count == 5) { snprintf(error, capacity, "A forward has at most four fields"); return 0; }
        fields[count] = p;
        lengths[count++] = colon ? (size_t)(colon - p) : strlen(p);
        if (!colon) break;
        p = colon + 1;
    }
    if (count != 3 && count != 4) {
        snprintf(error, capacity, "A forward is tcp|udp:[ADDRESS:]HOSTPORT:GUESTPORT, such as tcp:8080:80");
        return 0;
    }
    if (lengths[0] == 3 && !strncmp(fields[0], "tcp", 3)) f->udp = 0;
    else if (lengths[0] == 3 && !strncmp(fields[0], "udp", 3)) f->udp = 1;
    else { snprintf(error, capacity, "A forward starts with tcp or udp"); return 0; }
    f->address = 0x7F000001u;
    if (count == 4) {
        uint32_t a = 0;
        unsigned parts = 0, value = 0, digits = 0;
        for (size_t i = 0; i <= lengths[1]; ++i) {
            const char ch = i < lengths[1] ? fields[1][i] : '.';
            if (ch >= '0' && ch <= '9' && digits < 3) { value = value * 10 + (unsigned)(ch - '0'); digits++; continue; }
            if (ch != '.' || !digits || value > 255 || parts == 4) { parts = 5; break; }
            a = a << 8 | value;
            parts++;
            value = digits = 0;
        }
        if (parts != 4) { snprintf(error, capacity, "A forward's host address is an IPv4 address such as 0.0.0.0"); return 0; }
        f->address = a;
    }
    if (!port_number(fields[count - 2], lengths[count - 2], &f->host_port) ||
        !port_number(fields[count - 1], lengths[count - 1], &f->guest_port)) {
        snprintf(error, capacity, "A forward's ports are 1 to 65535");
        return 0;
    }
    return 1;
}
void mc_config_forward_text(const mc_forward_config *f, char *text, size_t capacity) {
    if (f->address == 0x7F000001u)
        snprintf(text, capacity, "%s:%u:%u", f->udp ? "udp" : "tcp", (unsigned)f->host_port, (unsigned)f->guest_port);
    else
        snprintf(text, capacity, "%s:%u.%u.%u.%u:%u:%u", f->udp ? "udp" : "tcp", (unsigned)(f->address >> 24),
                 (unsigned)(f->address >> 16 & 255), (unsigned)(f->address >> 8 & 255), (unsigned)(f->address & 255),
                 (unsigned)f->host_port, (unsigned)f->guest_port);
}
static int network_section(reader *r, yaml_node_t *v, mc_config *c) {
    if (!mapping(r, v)) return 0;
    PAIRS(v, q) {
        const char *name = scalar(r, node(r, q->key));
        yaml_node_t *x = node(r, q->value);
        if (!strcmp(name, "mode")) {
            if (!choice(r, x, networks, &c->network)) return 0;
        } else if (!strcmp(name, "card")) {
            if (!choice(r, x, net_cards, &c->network_card)) return 0;
        } else if (!strcmp(name, "forward")) {
            /* A profile's list replaces the shared one. */
            if (x->type != YAML_SEQUENCE_NODE) return fail(r, "network.forward must be a sequence");
            c->forward_count = 0;
            memset(c->forwards, 0, sizeof c->forwards);
            for (yaml_node_item_t *i = x->data.sequence.items.start; i < x->data.sequence.items.top; ++i) {
                if (c->forward_count == MC_CONFIG_FORWARDS) return fail(r, "At most 16 forwards are supported");
                const char *text = scalar(r, node(r, *i));
                if (!text) return 0;
                char problem[128];
                mc_forward_config *f = &c->forwards[c->forward_count];
                if (!mc_config_parse_forward(text, f, problem, sizeof problem)) return failf(r, "network.forward: %s", problem);
                for (size_t k = 0; k < c->forward_count; ++k)
                    if (c->forwards[k].udp == f->udp && c->forwards[k].host_port == f->host_port)
                        return fail(r, "network.forward: two forwards share a host port");
                c->forward_count++;
            }
        } else return fail(r, "Unknown network key; expected mode, card or forward");
    }
    return 1;
}
/* sharing (docs/NOTES.md, "Sharing"). Names: UTF-8 here; the file
 * server checks at start that the Macintosh's character set holds them. */
static const named metadatas[] = {{"streams",MC_SHARING_STREAMS},{"appledouble",MC_SHARING_APPLEDOUBLE},{NULL,0}};
const char *mc_config_sharing_metadata_name(uint32_t v) { return name_of(metadatas, v); }
static size_t utf8_chars(const char *s) {
    size_t n = 0;
    for (; *s; ++s) n += ((unsigned char)*s & 0xC0) != 0x80;
    return n;
}
static int share_name(reader *r, yaml_node_t *n, size_t most, char *out, const char *what) {
    const char *s = scalar(r, n);
    if (!s) return 0;
    if (!*s || utf8_chars(s) > most || strlen(s) >= MC_CONFIG_SHARE_NAME || strchr(s, ':'))
        return failf(r, "%s is 1 to %u characters without ':'", what, (unsigned)most);
    strcpy(out, s);
    return 1;
}
static int sharing_section(reader *r, yaml_node_t *v, const char *base, mc_config *c) {
    if (!mapping(r, v)) return 0;
    PAIRS(v, q) {
        const char *name = scalar(r, node(r, q->key));
        yaml_node_t *x = node(r, q->value);
        if (!name) return 0;
        if (!strcmp(name, "name")) {
            if (!share_name(r, x, 31, c->sharing_name, "sharing.name")) return 0;
        } else if (!strcmp(name, "metadata")) {
            if (!choice(r, x, metadatas, &c->sharing_metadata)) return 0;
        } else if (!strcmp(name, "password")) {
            const char *s = scalar(r, x);
            if (!s) return 0;
            if (strlen(s) > 8) return fail(r, "sharing.password has at most 8 characters");
            for (const char *p = s; *p; ++p)
                if ((unsigned char)*p < 0x20 || (unsigned char)*p > 0x7E) return fail(r, "sharing.password is printable ASCII");
            strcpy(c->sharing_password, s);
        } else if (!strcmp(name, "folders")) {
            /* A profile's list replaces the shared one. */
            if (x->type != YAML_SEQUENCE_NODE) return fail(r, "sharing.folders must be a sequence");
            c->share_count = 0;
            memset(c->shares, 0, sizeof c->shares);
            for (yaml_node_item_t *i = x->data.sequence.items.start; i < x->data.sequence.items.top; ++i) {
                yaml_node_t *item = node(r, *i);
                if (c->share_count == MC_CONFIG_SHARES) return fail(r, "At most 8 shared folders are supported");
                if (!mapping(r, item)) return 0;
                mc_share_config *f = &c->shares[c->share_count];
                int has_path = 0;
                PAIRS(item, k) {
                    const char *key = scalar(r, node(r, k->key));
                    yaml_node_t *val = node(r, k->value);
                    if (!key) return 0;
                    if (!strcmp(key, "name")) { if (!share_name(r, val, 27, f->name, "A shared folder's name")) return 0; }
                    else if (!strcmp(key, "path")) { if (!path_value(r, val, base, f->path)) return 0; has_path = 1; }
                    else if (!strcmp(key, "readonly")) { if (!boolean(r, val, &f->readonly)) return 0; }
                    else return fail(r, "Unknown sharing.folders key; expected name, path or readonly");
                }
                if (!has_path) return fail(r, "A shared folder needs a path");
                if (!f->name[0]) {
                    /* the folder's own name */
                    const char *p = f->path + strlen(f->path);
                    while (p > f->path && (p[-1] == '/' || p[-1] == '\\')) --p;
                    const char *end = p;
                    while (p > f->path && p[-1] != '/' && p[-1] != '\\' && p[-1] != ':') --p;
                    size_t n = (size_t)(end - p);
                    if (!n) return fail(r, "A shared folder needs a name");
                    if (n >= MC_CONFIG_SHARE_NAME) n = MC_CONFIG_SHARE_NAME - 1;
                    memcpy(f->name, p, n);
                    f->name[n] = 0;
                    if (utf8_chars(f->name) > 27 || strchr(f->name, ':')) return fail(r, "A shared folder's name is 1 to 27 characters without ':'; give it one with name:");
                }
                for (size_t k = 0; k < c->share_count; ++k)
                    if (!strcmp(c->shares[k].name, f->name)) return fail(r, "Two shared folders have the same name");
                c->share_count++;
            }
        } else return fail(r, "Unknown sharing key; expected name, folders, metadata or password");
    }
    return 1;
}
/* The sections and model of one mapping: the top level (shared) or a
 * profile's. */
static int apply_sections(reader *r, yaml_node_t *map, const char *base, mc_config *c, unsigned *set, char *model, int top) {
    if (!mapping(r, map)) return 0;
    PAIRS(map, p) {
        const char *key = scalar(r, node(r, p->key));
        if (!key) return 0;
        yaml_node_t *v = node(r, p->value);
        int ok;
        if (!strcmp(key, "model")) {
            const char *name = scalar(r, v);
            r->at.model = r->line;
            ok = name && strlen(name) < 64;
            if (ok) strcpy(model, name); else if (name) ok = fail(r, "Unknown model; livadix --list-models lists them");
        }
        else if (!strcmp(key, "emulation")) ok = emulation_section(r, v, base, c, set);
        else if (!strcmp(key, "video")) ok = video_section(r, v, c, set);
        else if (!strcmp(key, "storage")) ok = storage_section(r, v, base, c);
        else if (!strcmp(key, "sound")) ok = sound_section(r, v, c);
        else if (!strcmp(key, "input")) ok = input_section(r, v, c);
        else if (!strcmp(key, "ports")) ok = ports_section(r, v, base, c);
        else if (!strcmp(key, "network")) ok = network_section(r, v, c);
        else if (!strcmp(key, "sharing")) ok = sharing_section(r, v, base, c);
        else if (top && (!strcmp(key, "default") || !strcmp(key, "profiles"))) ok = 1;
        else ok = fail(r, top ? "Unknown configuration key; expected default, profiles, model, emulation, video, storage, sound, input, ports, network or sharing"
                              : "Unknown profile key; expected model, emulation, video, storage, sound, input, ports, network or sharing");
        if (!ok) return 0;
    }
    return 1;
}
/* The checks of a whole profile, once every key is read. */
static int finish(reader *r, mc_config *config, unsigned set, const char *model) {
    const size_t profile_line = r->line;
    r->line = r->at.ram;
    if (mc_config_ram_kb(config) < 1024u || mc_config_ram_kb(config) > 1024u * 1024u) return fail(r, "emulation.ram_mb must be 1..1024");
    /* The values as written, before a model takes its own. */
    r->line = r->at.resolution;
    int valid = 0;
    for (unsigned i = 0; i < MC_CONFIG_RESOLUTIONS; ++i)
        if (config->width == mc_config_resolutions[i][0] && config->height == mc_config_resolutions[i][1]) valid = 1;
    if ((set & (SET_WIDTH | SET_HEIGHT)) && !valid) return fail(r, "Unsupported screen resolution");
    r->line = r->at.depth;
    valid = 0;
    for (unsigned i = 0; i < MC_CONFIG_DEPTHS; ++i) if (config->depth == mc_config_depths[i]) valid = 1;
    if ((set & SET_DEPTH) && !valid) return fail(r, "video.depth must be 1, 2, 4, 8, 16 or 32");
    if (!lenient && (own_keys & SET_WIDTH) && (own_keys & SET_DEPTH) && (uint64_t)config->width * config->height * config->depth / 8 > MC_CONFIG_VRAM) {
        unsigned most = 1;
        for (unsigned i = 0; i < MC_CONFIG_DEPTHS; ++i)
            if ((uint64_t)config->width * config->height * mc_config_depths[i] / 8 <= MC_CONFIG_VRAM) most = mc_config_depths[i];
        r->line = r->at.depth ? r->at.depth : r->at.resolution;
        return failf(r, "video.resolution %ux%u at video.depth %u needs %llu KiB, more than the 2 MiB of VRAM; %ux%u goes up to %u bits",
                     (unsigned)config->width, (unsigned)config->height, (unsigned)config->depth,
                     (unsigned long long)config->width * config->height * config->depth / 8 / 1024, (unsigned)config->width,
                     (unsigned)config->height, most);
    }
    r->line = r->at.model;
    if (!apply_model(r, config, model[0] ? model : NULL, set)) return 0;
    /* A look the model's screen never had: its own (crt or lcd). */
    config->look = mc_config_look_fit(mc_model_entry(config->model), config->look);
    {
        /* A screen from the shared settings the model does not show: the
         * model's own. */
        const mc_model *p = mc_model_entry(config->model);
        char why[400];
        const unsigned screen = SET_WIDTH | SET_HEIGHT | SET_DEPTH | SET_MONITOR;
        if (!(own_keys & screen) && ((uint64_t)config->width * config->height * config->depth / 8 > MC_CONFIG_VRAM ||
                                      !mc_config_monitor_check(p, config->width, config->height, config->depth, config->monitor, why, sizeof why))) {
            config->width = p->width;
            config->height = p->height;
            config->depth = p->depth;
            config->monitor = p->monitor;
        }
    }
    if (!lenient && (uint64_t)config->width * config->height * config->depth / 8 > MC_CONFIG_VRAM) {
        unsigned most = 1;
        for (unsigned i = 0; i < MC_CONFIG_DEPTHS; ++i)
            if ((uint64_t)config->width * config->height * mc_config_depths[i] / 8 <= MC_CONFIG_VRAM) most = mc_config_depths[i];
        r->line = r->at.depth ? r->at.depth : r->at.resolution;
        return failf(r, "video.resolution %ux%u at video.depth %u needs %llu KiB, more than the 2 MiB of VRAM; %ux%u goes up to %u bits",
                     (unsigned)config->width, (unsigned)config->height, (unsigned)config->depth,
                     (unsigned long long)config->width * config->height * config->depth / 8 / 1024, (unsigned)config->width,
                     (unsigned)config->height, most);
    }
    /* The window shows every Nth of the 60 Hz frames (N = 1..6). */
    r->line = r->at.refresh;
    if (config->refresh != 60 && config->refresh != 30 && config->refresh != 20 && config->refresh != 15 && config->refresh != 12 &&
        config->refresh != 10)
        return fail(r, "video.refresh must be 60, 30, 20, 15, 12 or 10");
    r->line = r->at.mouse;
    if (config->mouse[0] != MC_MOUSE_NORMAL && config->mouse[1] != MC_MOUSE_NORMAL) return fail(r, "At least one mouse button must be normal");
    if (!lenient) {
        char why[400];
        const mc_model *chosen = mc_model_entry(config->model);
        if (!mc_config_monitor_check(chosen, config->width, config->height, config->depth, config->monitor, why, sizeof why)) {
            r->line = r->at.monitor ? r->at.monitor : r->at.model;
            return failf(r, "video.monitor %s: %s", mc_config_monitor_name(config->monitor), why);
        }
    }
    r->line = r->at.storage;
    if ((mc_machine_features(mc_model_entry(config->model)) & MC_FEATURE_NO_FLOPPY)) {
        for (size_t i = 0; i < config->floppy_count; i++)
            if (config->floppies[i].image[0] && !lenient) return fail(r, "This model has no built-in floppy drive");
        config->floppy_count = 0;
        config->floppy_off[0] = config->floppy_off[1] = 1;
    }
    if ((mc_machine_features(mc_model_entry(config->model)) & MC_FEATURE_ONE_FLOPPY)) {
        /* A second drive without an image (shared settings, say) is left out. */
        if (!lenient && config->floppy_count > 1 && !config->floppy_off[1] && config->floppies[1].image[0])
            return fail(r, "This model has one floppy expansion-bay module");
        if (config->floppy_count > 1) {
            config->floppy_count = 1;
            config->floppy_off[1] = 1;
            memset(&config->floppies[1], 0, sizeof config->floppies[1]);
        }
    }
    /* A console has no hard disk: it starts from a CD (storage.cdrom); disks
     * the shared settings name are not its. */
    if (mc_model_entry(config->model)->console) {
        memset(config->disks, 0, sizeof config->disks);
        config->disk_count = 0;
    }
    if (shared_writable_image(config)) return fail(r, "The same image is configured twice and at least one use is writable");
    r->line = profile_line;
    return 1;
}

/* A profile's PRAM file when emulation.nvram names none: in the folder nvram
 * beside the configuration, the configuration's name, the profile's name in
 * lower-case letters and digits, .nvram. */
static void profile_nvram(mc_config *c) {
    char slug[MC_CONFIG_NAME];size_t n=0;
    for(const char *p=c->profile;*p&&n+1<sizeof slug;++p){
        char ch=*p;if(ch>='A'&&ch<='Z')ch=(char)(ch+'a'-'A');
        if((ch>='a'&&ch<='z')||(ch>='0'&&ch<='9'))slug[n++]=ch;
        else if(n&&slug[n-1]!='-')slug[n++]='-';
    }
    while(n&&slug[n-1]=='-')--n;
    slug[n]=0;
    const char *slash=strrchr(c->source,'/'),*back=strrchr(c->source,'\\');if(back&&(!slash||back>slash))slash=back;
    const char *name=slash?slash+1:c->source,*dot=strrchr(name,'.');int stem=dot?(int)(dot-name):(int)strlen(name);
    char path[MC_CONFIG_PATH];
    snprintf(path,sizeof path,"%.*snvram/%.*s%s%s.nvram",(int)(name-c->source),c->source,stem,name,
             c->profile[0]?"-":"",c->profile[0]?(n?slug:"profile"):"");
#ifdef _WIN32
    if(!_fullpath(c->nvram,path,sizeof c->nvram))snprintf(c->nvram,sizeof c->nvram,"%s",path);
    for(char *p=c->nvram;*p;++p)if(*p=='\\')*p='/';
#else
    snprintf(c->nvram,sizeof c->nvram,"%s",path);
#endif
}

/* Two independent 64-bit streams identify an existing unnamed profile.
 * Once written, the identity follows a rename. Duplicate IDs are rejected
 * in the document; a duplicate profile receives a new identity. */
void mc_config_nvram_identity(mc_config *c) {
    if(c->nvram_id[0])return;
    char source[MC_CONFIG_PATH];
#ifdef _WIN32
    if(!_fullpath(source,c->source[0]?c->source:c->nvram,sizeof source))snprintf(source,sizeof source,"%s",c->source);
    for(char *p=source;*p;++p){if(*p=='\\')*p='/';if(*p>='A'&&*p<='Z')*p=(char)(*p+'a'-'A');}
#else
    {   /* the file's absolute path, as _fullpath gives it: the working folder does not change who owns the PRAM */
        const char *given=c->source[0]?c->source:c->nvram;
        if(!resolve_path(given,source,sizeof source)){
            if(given[0]=='/'||!getcwd(source,sizeof source)||strlen(source)+strlen(given)+2>sizeof source)snprintf(source,sizeof source,"%s",given);
            else{size_t k=strlen(source);snprintf(source+k,sizeof source-k,"/%s",given);}
        }
    }
#endif
    uint64_t a=14695981039346656037ull,b=7809847782465536322ull;
    const char *parts[]={source,"\n",c->profile};
    for(unsigned i=0;i<3;++i)for(const unsigned char *p=(const unsigned char *)parts[i];*p;++p){a=(a^*p)*1099511628211ull;b=(b^*p)*14029467366897019727ull;}
    snprintf(c->nvram_id,sizeof c->nvram_id,"%016llx%016llx",(unsigned long long)a,(unsigned long long)b);
}

/* New or copied profiles need a fresh identity even if their name was
 * used before. Existing files without an ID use the deterministic namespace
 * above until Save freezes it; no files are changed while reading config. */
int mc_config_new_nvram_identity(mc_config *c) {
    unsigned char bytes[16];
#ifdef _WIN32
    for(unsigned i=0;i<4;++i){unsigned int value;if(rand_s(&value))return 0;memcpy(bytes+4*i,&value,4);}
#else
    FILE *f=fopen("/dev/urandom","rb");if(!f)return 0;
    int ok=fread(bytes,1,sizeof bytes,f)==sizeof bytes;fclose(f);if(!ok)return 0;
#endif
    static const char hex[]="0123456789abcdef";
    for(unsigned i=0;i<16;++i){c->nvram_id[2*i]=hex[bytes[i]>>4];c->nvram_id[2*i+1]=hex[bytes[i]&15];}
    c->nvram_id[32]=0;return 1;
}

/* A document: its profiles, and the configuration of one of them (NULL for
 * the default) into out when out is not NULL. */
static int read_document(const char *text, size_t length, const char *source, const char *wanted, mc_config *out,
                         mc_profile_list *list, char *error, size_t capacity) {
    yaml_parser_t parser;
    yaml_document_t doc;
    reader r;
    memset(&r, 0, sizeof r);
    r.error = error; r.cap = capacity;
    if (capacity) error[0] = 0;
    if (!yaml_parser_initialize(&parser)) return fail(&r, "Cannot allocate YAML parser");
    yaml_parser_set_input_string(&parser, (const unsigned char *)text, length);
    if (!yaml_parser_load(&parser, &doc)) {
        if (capacity) snprintf(error, capacity, "YAML line %zu: %s", parser.problem_mark.line + 1, parser.problem ? parser.problem : "parse error");
        yaml_parser_delete(&parser);
        return 0;
    }
    r.doc = &doc;
    char base[MC_CONFIG_PATH];
    const size_t source_length = strlen(source);
    int ok = 1;
    if (source_length >= sizeof base) ok = fail(&r, "Configuration path is too long");
    else {
        memcpy(base, source, source_length + 1);
        char *slash = strrchr(base, '/'), *backslash = strrchr(base, '\\');
        if (backslash && (!slash || backslash > slash)) slash = backslash;
        if (slash) slash[1] = 0; else base[0] = 0;
        /* The folder absolute: the paths read do not depend on the working
         * folder, and a file named without one (livadix.yaml) reads back the
         * relative paths it writes as the absolute ones the window edits. */
        char full[MC_CONFIG_PATH];
#ifdef _WIN32
        const int resolved = _fullpath(full, base[0] ? base : ".", sizeof full) != NULL;
#else
        const int resolved = resolve_path(base[0] ? base : ".", full, sizeof full);
#endif
        const size_t end = resolved ? strlen(full) : 0;
        if (end && end + 1 < sizeof full) {
            for (char *s = full; *s; ++s) if (*s == '\\') *s = '/';
            if (full[end - 1] != '/' && full[end - 1] != '\\') { full[end] = '/'; full[end + 1] = 0; }
            memcpy(base, full, strlen(full) + 1);
        }
    }
    yaml_node_t *root = yaml_document_get_root_node(&doc);
    yaml_node_t *profiles = NULL, *def = NULL;
    if (ok && !root) ok = fail(&r, "The configuration is empty");
    if (ok) ok = mapping(&r, root);
    if (ok) PAIRS(root, p) {
        const char *key = scalar(&r, node(&r, p->key));
        if (!key) { ok = 0; break; }
        if (!strcmp(key, "profiles")) profiles = node(&r, p->value);
        else if (!strcmp(key, "default")) def = node(&r, p->value);
    }
    /* The profiles: a mapping of names to mappings. */
    mc_profile_list found;
    memset(&found, 0, sizeof found);
    found.default_index = -1;
    yaml_node_t *bodies[MC_CONFIG_PROFILES];
    if (ok && profiles) {
        if (!mapping(&r, profiles)) ok = 0;
        else PAIRS(profiles, p) {
            const char *name = scalar(&r, node(&r, p->key));
            yaml_node_t *body = node(&r, p->value);
            if (!name) { ok = 0; break; }
            if (!*name) { ok = fail(&r, "A profile needs a name"); break; }
            if (strlen(name) >= MC_CONFIG_NAME) { ok = fail(&r, "A profile name is at most 127 bytes"); break; }
            if (found.count == MC_CONFIG_PROFILES) { ok = fail(&r, "At most 256 profiles are supported"); break; }
            /* "name:" with nothing after it is an empty profile. */
            if (body && body->type == YAML_SCALAR_NODE && !body->data.scalar.length) body = NULL;
            else if (!mapping(&r, body)) { ok = 0; break; }
            snprintf(found.names[found.count], MC_CONFIG_NAME, "%s", name);
            bodies[found.count++] = body;
        }
        if (ok && !found.count) ok = fail(&r, "profiles: holds no profile");
    }
    if (ok && def) {
        const char *name = scalar(&r, def);
        if (!name) ok = 0;
        else if (!found.count) ok = fail(&r, "default: names a profile, but the file has no profiles:");
        else {
            for (size_t i = 0; i < found.count; ++i) if (!strcmp(found.names[i], name)) found.default_index = (int)i;
            if (found.default_index < 0) ok = failf(&r, "default: there is no profile \"%s\"", name);
        }
    }
    if (ok && found.count && found.default_index < 0) found.default_index = 0;
    /* The profile wanted: by its name (letter case aside), else the default. */
    int chosen = found.count ? found.default_index : -1;
    if (ok && wanted && *wanted) {
        chosen = -1;
        for (size_t i = 0; i < found.count && chosen < 0; ++i) if (!strcmp(found.names[i], wanted)) chosen = (int)i;
        for (size_t i = 0; i < found.count && chosen < 0; ++i) if (!_stricmp(found.names[i], wanted)) chosen = (int)i;
        r.line = 0;
        if (chosen < 0 && !found.count) ok = failf(&r, "There is no profile \"%s\": the file has no profiles:", wanted);
        else if (chosen < 0) ok = failf(&r, "There is no profile \"%s\" (livadix --list-profiles lists them)", wanted);
    }
    char nvram_ids[MC_CONFIG_PROFILES][33]; memset(nvram_ids,0,sizeof nvram_ids);
    /* Every profile is read and checked; the chosen one is kept. */
    for (size_t i = 0; ok && i < (found.count ? found.count : 1); ++i) {
        mc_config config;
        mc_config_defaults(&config);
        snprintf(config.source, sizeof config.source, "%s", source);
        unsigned set = 0;
        char model[64] = "";
        memset(&r.at, 0, sizeof r.at);
        r.profile = NULL;
        ok = apply_sections(&r, root, base, &config, &set, model, 1);
        own_keys = set;
        if(ok&&found.count&&config.nvram_id[0])ok=fail(&r,"emulation.nvram_id belongs to an individual profile, not shared settings");
        if (ok && found.count) {
            r.profile = found.names[i];
            snprintf(config.profile, sizeof config.profile, "%s", found.names[i]);
            r.line = bodies[i] ? bodies[i]->start_mark.line + 1 : 0;
            unsigned mine = 0;
            if (bodies[i]) ok = apply_sections(&r, bodies[i], base, &config, &mine, model, 0);
            set |= mine;
            own_keys = mine;
        }
        if (ok) ok = finish(&r, &config, set, model);
        if(ok) {
            mc_config_nvram_identity(&config);
            for(size_t j=0;j<i;++j)if(!strcmp(config.nvram_id,nvram_ids[j]))ok=fail(&r,"Profiles must have different emulation.nvram_id values (a duplicate needs a new identity)");
            snprintf(nvram_ids[i],sizeof nvram_ids[i],"%s",config.nvram_id);
        }
        if (ok && config.source[0] && !config.nvram[0] && !mc_model_builtin(mc_model_entry(config.model)))
            profile_nvram(&config);
        if (ok) snprintf(found.models[i], sizeof found.models[i], "%s", model[0] ? model : MC_MODEL_BUILTIN);
        if (ok && out && (int)i == (found.count ? chosen : 0)) *out = config;
    }
    yaml_document_delete(&doc);
    if (ok) {
        r.doc = NULL; r.line = 0; r.profile = NULL;
        if (!yaml_parser_load(&parser, &doc)) ok = fail(&r, "Invalid trailing YAML document");
        else { if (yaml_document_get_root_node(&doc)) ok = fail(&r, "Only one YAML document is allowed"); yaml_document_delete(&doc); }
    }
    yaml_parser_delete(&parser);
    if (ok && list) *list = found;
    return ok;
}

static char *read_text(const char *path, size_t *length) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    char *text = NULL;
    long size = -1;
    if (!fseek(f, 0, SEEK_END)) size = ftell(f);
    if (size >= 0 && size < 16 * 1024 * 1024 && !fseek(f, 0, SEEK_SET) && (text = malloc((size_t)size + 1)) != NULL &&
        fread(text, 1, (size_t)size, f) != (size_t)size) {
        free(text);
        text = NULL;
    }
    fclose(f);
    if (text) { text[size] = 0; *length = (size_t)size; }
    return text;
}
int mc_config_parse(const char *text, size_t length, const char *source, const char *profile, mc_config *out, char *error,
                    size_t capacity) {
    mc_config config;
    if (!read_document(text, length, source, profile, &config, NULL, error, capacity)) return 0;
    *out = config;
    return 1;
}
int mc_config_parse_values(const char *text, size_t length, const char *source, const char *profile, mc_config *out, char *error,
                           size_t capacity) {
    lenient = 1;
    const int ok = mc_config_parse(text, length, source, profile, out, error, capacity);
    lenient = 0;
    return ok;
}
int mc_config_load_profile(const char *path, const char *profile, mc_config *out, char *error, size_t capacity) {
    size_t length = 0;
    char *text = read_text(path, &length);
    if (!text) {
        if (capacity) snprintf(error, capacity, "Cannot open YAML configuration");
        return 0;
    }
    const int ok = mc_config_parse(text, length, path, profile, out, error, capacity);
    free(text);
    return ok;
}
int mc_config_load(const char *path, mc_config *out, char *error, size_t capacity) {
    return mc_config_load_profile(path, NULL, out, error, capacity);
}
int mc_config_profiles_text(const char *text, size_t length, const char *source, mc_profile_list *list, char *error, size_t capacity) {
    return read_document(text, length, source, NULL, NULL, list, error, capacity);
}
int mc_config_profiles(const char *path, mc_profile_list *list, char *error, size_t capacity) {
    size_t length = 0;
    char *text = read_text(path, &length);
    if (!text) {
        if (capacity) snprintf(error, capacity, "Cannot open YAML configuration");
        return 0;
    }
    const int ok = mc_config_profiles_text(text, length, path, list, error, capacity);
    free(text);
    return ok;
}

/* The header checksum and size of a ROM file; 0 when it cannot be read. */
static int rom_header(const char *path, uint32_t *checksum, uint64_t *size) {
    FILE *f = fopen(path, "rb");
    if (!f) return 0;
    uint8_t h[4] = {0};
    int ok = fread(h, 1, 4, f) == 4 && !fseek(f, 0, SEEK_END);
    long end = ok ? ftell(f) : -1;
    fclose(f);
    if (!ok || end < 0) return 0;
    *checksum = ((uint32_t)h[0] << 24) | ((uint32_t)h[1] << 16) | ((uint32_t)h[2] << 8) | h[3];
    *size = (uint64_t)end;
    return 1;
}
/* The folder part of a path, with its trailing separator ("" for none). */
static void folder_of(const char *path, char *out) {
    snprintf(out, MC_CONFIG_PATH, "%s", path);
    char *slash = strrchr(out, '/'), *backslash = strrchr(out, '\\');
    if (backslash && (!slash || backslash > slash)) slash = backslash;
    if (slash) slash[1] = 0; else out[0] = 0;
}
/* Two folder paths naming the same folder (a relative one resolved against
 * the current folder; Windows paths compare without case). */
static int same_folder(const char *a, const char *b) {
    char fa[MC_CONFIG_PATH], fb[MC_CONFIG_PATH];
#ifdef _WIN32
    if (!_fullpath(fa, a, sizeof fa) || !_fullpath(fb, b, sizeof fb)) return !strcmp(a, b);
    for (char *s = fa; *s; ++s) if (*s == '\\') *s = '/';
    for (char *s = fb; *s; ++s) if (*s == '\\') *s = '/';
    return !_stricmp(fa, fb);
#else
    if (!resolve_path(a, fa, sizeof fa) || !resolve_path(b, fb, sizeof fb)) return !strcmp(a, b);
    return !strcmp(fa, fb);
#endif
}
size_t mc_config_rom_folders(const mc_config *c, const char *program_folder, char folders[2][MC_CONFIG_PATH]) {
    char base[MC_CONFIG_PATH];
    size_t count = 0;
    if (program_folder) snprintf(folders[count++], MC_CONFIG_PATH, "%sroms/", program_folder);
    folder_of(c->source, base);
    snprintf(folders[count], MC_CONFIG_PATH, "%sroms/", base);
    if (!count || !same_folder(folders[0], folders[1])) ++count;
    return count;
}
static int find_rom(mc_config *c, const mc_rom_index *ix, char *error, size_t capacity) {
    reader r = {NULL, error, capacity};
    const mc_model *p = mc_model_entry(c->model);
    const mc_known_rom *want = mc_model_rom(p);
    if (capacity) error[0] = 0;

    if (!p || !want) return fail(&r, "Unknown model");
    c->rom_scanned = 0;
    if (c->rom_explicit) {
        uint32_t sum; uint64_t size;
        if (!rom_header(c->rom, &sum, &size)) {
            if (capacity) snprintf(error, capacity, "Cannot read ROM %s", c->rom);
            return 0;
        }
        if (sum == want->checksum && size == want->size) { c->roms = ix; return 1; }
        const mc_known_rom *is = mc_romdb_find(sum, (size_t)size);
        if (capacity) {
            if (is) snprintf(error, capacity, "ROM %s is the %s ROM (%08X, %s), but model: %s needs ROM %08X (%s)",
                             c->rom, is->models, (unsigned)is->checksum, is->date, p->key, (unsigned)want->checksum, want->models);
            else snprintf(error, capacity, "ROM %s (checksum %08X, %llu KiB) is not a known Macintosh ROM; model: %s needs ROM %08X (%s)",
                          c->rom, (unsigned)sum, (unsigned long long)(size / 1024), p->key, (unsigned)want->checksum, want->models);
        }
        return 0;
    }
    /* The embedded Built-in image always has this executable's documents;
     * a leftover external livadix.rom must not supply an older desktop. */
    if (mc_model_builtin(p)) {
        c->rom[0] = 0;
        c->roms = ix;
        return 1;
    }
    /* The roms folders' file holding the ROM, whatever its name. */
    const mc_rom_file *f = mc_rom_index_rom(ix, want);
    if (f) {
        snprintf(c->rom, sizeof c->rom, "%s", f->path);
        c->rom_scanned = !f->standard;
        c->roms = ix;
        return 1;
    }
    if (capacity) {
        const int two = ix && ix->folder_count > 1;
        snprintf(error, capacity, "Cannot find ROM: model: %s needs ROM %08X (%s), a file of any name (standard name %s) in %s%s%s; "
                 "copy it there or name it with emulation.rom", p->key, (unsigned)want->checksum, want->models, want->file,
                 ix && ix->folder_count ? ix->folders[0] : "roms/", two ? " or " : "", two ? ix->folders[1] : "");
    }
    return 0;
}

/* emulation.controller: firmware: the model's controller dump must be in
 * the index (auto runs it when there, else the protocol level). */
static int check_controller(const mc_config *c, const mc_rom_index *ix, char *error, size_t capacity) {
    if (c->controller != MC_CONTROLLER_FIRMWARE) return 1;
    const mc_firmware *fw = mc_firmware_find(mc_model_controller_firmware(mc_model_entry(c->model)));
    if (!fw || mc_rom_index_firmware(ix, fw)) return 1;
    if (capacity)
        snprintf(error, capacity, "emulation.controller: firmware needs the %s dump (%u bytes, CRC32 %08x; standard name %s) "
                 "in a roms folder, under any name; auto runs the protocol model without it", fw->part, (unsigned)fw->size,
                 (unsigned)fw->crc32, fw->file);
    return 0;
}
int mc_config_find_rom(mc_config *c, const mc_rom_index *ix, char *error, size_t capacity) {
    return find_rom(c, ix, error, capacity) && check_controller(c, ix, error, capacity);
}
