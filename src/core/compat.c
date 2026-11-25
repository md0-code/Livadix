/* Which System each model runs (docs/NOTES.md, "Compatibility").
 *
 * Sources: the oldest System of each model is the one
 * it shipped with (Apple Spec Database, "System Software"); System 6.0.8L
 * for the Classic II and LC II; the enablers System 7.1 needs from Apple's
 * "System Enablers Read Me" (Developer CD, April 1994: models released after
 * System 7.1 was completed, September 1992, need one) and later enabler
 * lists; Mac OS 7.6 from Technote 1090 (a 32-bit clean ROM and at least a
 * 68030: not the II, IIx, IIcx, SE/30 or LC); Mac OS 8 from Technote 1102
 * (a 68040 or PowerPC); System 7.5 includes the support of the 68k models
 * released before it. The 'gbly' lists of the System file and of the
 * enablers on the disk (hfs.c) name the models they support.
 *
 * The PowerPC models: the oldest System and the enablers from Apple's Tech
 * Info Library article 11491 ("System Enablers: Current Versions & Change
 * History", 1993-2000: the PowerPC Enabler before System 7.5 on the 6100,
 * 7100 and 8100 of 1994, version 1.1.1 with System 7.5 and Finder 7.5.1
 * on the speed-bumped ones before 7.5.3; System Enabler 406 before 7.5.3
 * on the 5200 family, 701 with 7.5.2 on the 7200, 7500, 8500 and 9500,
 * 410 before 7.5.5 on the 5400 and 6400, 411 and 702 with 7.5.5 on the
 * 5500, 6500, 7300, 8600 and 9600, which Mac OS 7.6 does not run and 7.6.1
 * does; the 8600 and 9600 of 1997 from 7.6.1; the PowerBook
 * 5300/2300/190 Enabler with 7.5.2); the others' oldest System the one
 * they shipped with (Apple Spec Database). The newest: Mac OS 9.1 on
 * every PowerPC (Technote 1176: Mac OS 9 runs on every Macintosh that
 * shipped with a PowerPC), Mac OS 9.2.2 on the G3s (Mac OS 9.2 needs a G3).
 *
 * Memory: Apple's stated minimums (System 6 1 MB; System 7.0 and 7.1 2 MB;
 * System 7.5 4 MB, its user's guide; Mac OS 7.6 8 MB, its installation
 * manual; Mac OS 8 12 MB, Apple's announced requirement; Mac OS 8.5 16 MB
 * of physical RAM, Technote 1142; Mac OS 9 32 MB, Technote 1176). Observed on Livadix's
 * Quadra 700 with HD240.HF8: Mac OS 8.0 at 4 and 8 MiB stops with "The
 * Finder could not start up because there was not enough memory
 * available" and reaches the Finder at 20 MiB; System 7.1.1 reaches the
 * Finder at 4 MiB. */
#include "compat.h"
#include "config.h"
#include "models.h"

#include <stdio.h>
#include <string.h>

#define V(major, minor, bug) (uint16_t)((major) << 8 | (minor) << 4 | (bug))
#define OLD24 V(7, 5, 5)          /* Technote 1090: no Mac OS 7.6 */
#define M030 V(7, 6, 1)           /* Technote 1102: no Mac OS 8 without a 68040 */
#define M040 V(8, 1, 0)           /* the last 68k Mac OS */
#define E75 V(7, 5, 0)
#define PPC V(9, 1, 0)            /* Technote 1176: the last Mac OS of every PowerPC */
#define G3 V(9, 2, 2)             /* Mac OS 9.2 needs a G3 */

/* The 68000 Macs (uncertain, Apple's later compatibility charts): the 128K
 * ran up to System 3.2, the 512K to 4.1 (both with 64 KiB ROMs, no HFS in
 * ROM), the 512Ke to 6.0.8 (512 KB, too little for System 7); the Plus,
 * SE, Classic, Portable and PowerBook 100 to 7.5.5, the last for a 68000. */
static const mc_compat_entry table[] = {
    {"mac-128k", V(1, 0, 0), V(3, 2, 0), NULL, 0, 0},
    {"mac-512k", V(1, 1, 0), V(4, 1, 0), NULL, 0, 0},
    {"mac-512ke", V(3, 0, 0), V(6, 0, 8), NULL, 0, 0},
    {"mac-plus", V(3, 0, 0), OLD24, NULL, 0, 0},
    {"mac-plus-v1", V(3, 0, 0), OLD24, NULL, 0, 0},
    {"mac-plus-v2", V(3, 0, 0), OLD24, NULL, 0, 0},
    {"mac-se", V(4, 0, 0), OLD24, NULL, 0, 0},
    {"mac-se-fdhd", V(6, 0, 2), OLD24, NULL, 0, 0},
    {"mac-classic", V(6, 0, 7), OLD24, NULL, 0, 0},
    {"mac-portable", V(6, 0, 4), OLD24, NULL, 0, 0},
    {"powerbook-100", V(7, 0, 1), OLD24, NULL, 0, 0},
    {"mac-ii-v1", V(4, 0, 0), OLD24, NULL, 0, MC_COMPAT_24BIT},
    {"mac-ii-v2", V(4, 0, 0), OLD24, NULL, 0, MC_COMPAT_24BIT},
    {"mac-iix", V(6, 0, 0), OLD24, NULL, 0, MC_COMPAT_24BIT},
    {"mac-ii-fdhd", V(6, 0, 2), OLD24, NULL, 0, MC_COMPAT_24BIT},
    {"mac-iicx", V(6, 0, 3), OLD24, NULL, 0, MC_COMPAT_24BIT},
    {"se-30", V(6, 0, 3), OLD24, NULL, 0, MC_COMPAT_24BIT},
    {"iici", V(6, 0, 4), M030, NULL, 0, 0},
    {"iifx", V(6, 0, 5), M030, NULL, 0, 0},
    {"lc", V(6, 0, 7), OLD24, NULL, 0, 0},
    {"iisi", V(6, 0, 7), M030, NULL, 0, 0},
    {"classic-ii", V(7, 0, 1), M030, NULL, 0, MC_COMPAT_608L},
    {"quadra-700", V(7, 0, 1), M040, NULL, 0, 0},
    {"quadra-900", V(7, 0, 1), M040, NULL, 0, 0},
    {"powerbook-140", V(7, 0, 1), M030, NULL, 0, 0},
    {"powerbook-145", V(7, 1, 0), M030, NULL, 0, 0},
    {"powerbook-145b", V(7, 1, 0), M030, NULL, 0, 0},
    {"powerbook-170", V(7, 0, 1), M030, NULL, 0, 0},
    {"lc-ii", V(7, 0, 1), M030, NULL, 0, MC_COMPAT_608L},
    {"quadra-950", V(7, 0, 1), M040, NULL, 0, 0},
    {"iivx", V(7, 1, 0), M030, "System Enabler 001", E75, 0},
    {"iivi", V(7, 1, 0), M030, "System Enabler 001", E75, 0},
    {"performa-600", V(7, 1, 0), M030, "System Enabler 304", E75, 0},
    {"powerbook-160", V(7, 1, 0), M030, "System Enabler 131", E75, 0},
    {"powerbook-165", V(7, 1, 0), M030, "System Enabler 131", E75, 0},
    {"powerbook-165c", V(7, 1, 0), M030, "System Enabler 131", E75, 0},
    {"powerbook-180", V(7, 1, 0), M030, "System Enabler 131", E75, 0},
    {"powerbook-180c", V(7, 1, 0), M030, "System Enabler 131", E75, 0},
    {"powerbook-duo-210", V(7, 1, 0), M030, "System Enabler 201", E75, 0},
    {"powerbook-duo-230", V(7, 1, 0), M030, "System Enabler 201", E75, 0},
    {"powerbook-duo-250", V(7, 1, 0), M030, "PowerBook Duo Enabler", E75, 0},
    {"lc-iii-v1", V(7, 1, 0), M030, "System Enabler 003", E75, 0},
    {"lc-iii", V(7, 1, 0), M030, "System Enabler 003", E75, 0},
    {"lc-iii-plus", V(7, 1, 0), M030, "System Enabler 003 v1.1", E75, 0},
    {"color-classic", V(7, 1, 0), M030, "System Enabler 401", E75, 0},
    {"centris-610", V(7, 1, 0), M040, "System Enabler 040", E75, 0},
    {"centris-650", V(7, 1, 0), M040, "System Enabler 040", E75, 0},
    {"quadra-800", V(7, 1, 0), M040, "System Enabler 040", E75, 0},
    {"quadra-610", V(7, 1, 0), M040, "System Enabler 040", E75, 0},
    {"quadra-650", V(7, 1, 0), M040, "System Enabler 040", E75, 0},
    {"powerbook-duo-270c", V(7, 1, 0), M030, "PowerBook Duo Enabler", E75, 0},
    {"color-classic-ii", V(7, 1, 0), M030, "System Enabler 403", E75, 0},
    {"lc-520", V(7, 1, 0), M030, "System Enabler 403", E75, 0},
    {"lc-550", V(7, 1, 0), M030, "System Enabler 403", E75, 0},
    {"performa-275", V(7, 1, 0), M030, "System Enabler 403", E75, 0},
    {"performa-550", V(7, 1, 0), M030, "System Enabler 332 or 364", E75, 0},
    {"performa-560", V(7, 1, 0), M030, "System Enabler 332 or 364", E75, 0},
    {"macintosh-tv", V(7, 1, 0), M030, "System Enabler 404", E75, 0},
    {"quadra-605", V(7, 1, 0), M040, "System Enabler 065", E75, 0},
    {"lc-475", V(7, 1, 0), M040, "System Enabler 065", E75, 0},
    {"performa-475", V(7, 1, 0), M040, "System Enabler 364", E75, 0},
    {"performa-476", V(7, 1, 0), M040, "System Enabler 364", E75, 0},
    {"lc-575", V(7, 1, 0), M040, "System Enabler 065", E75, 0},
    {"performa-575", V(7, 1, 0), M040, "System Enabler 364", E75, 0},
    {"performa-577", V(7, 1, 0), M040, "System Enabler 364", E75, 0},
    {"performa-578", V(7, 1, 0), M040, "System Enabler 364", E75, 0},
    {"powerbook-duo-280", V(7, 1, 1), M040, "PowerBook Duo Enabler", E75, 0},
    {"powerbook-duo-280c", V(7, 1, 1), M040, "PowerBook Duo Enabler", E75, 0},
    {"powerbook-520", V(7, 1, 1), M040, "PowerBook 500 Series Enabler", E75, 0},
    {"powerbook-520c", V(7, 1, 1), M040, "PowerBook 500 Series Enabler", E75, 0},
    {"powerbook-540", V(7, 1, 1), M040, "PowerBook 500 Series Enabler", E75, 0},
    {"powerbook-540c", V(7, 1, 1), M040, "PowerBook 500 Series Enabler", E75, 0},
    {"quadra-630", V(7, 1, 2), M040, "System Enabler 405", E75, 0},
    {"lc-630", V(7, 1, 2), M040, "System Enabler 405", E75, 0},
    {"performa-630", V(7, 1, 2), M040, "System Enabler 405", E75, 0},
    {"powerbook-150", V(7, 1, 1), M030, "PowerBook 150 Enabler", E75, 0},
    {"quadra-840av", V(7, 1, 0), M040, "System Enabler 088", E75, 0},
    {"quadra-660av", V(7, 1, 0), M040, "System Enabler 088", E75, 0},
    {"performa-580", V(7, 5, 1), M040, NULL, 0, 0},
    {"lc-580", V(7, 5, 1), M040, NULL, 0, 0},
    {"performa-588", V(7, 5, 1), M040, NULL, 0, 0},
    {"powerbook-190", V(7, 5, 2), M040, "PowerBook 5300/2300/190 Enabler", V(7, 6, 0), 0},
    {"powerbook-190cs", V(7, 5, 2), M040, "PowerBook 5300/2300/190 Enabler", V(7, 6, 0), 0},
    /* The PowerPC Macintoshes (the Pippin, a console, has no row: it starts
     * its own discs). */
    {"power-macintosh-6100", V(7, 1, 2), PPC, "PowerPC Enabler", E75, 0},
    {"power-macintosh-7100", V(7, 1, 2), PPC, "PowerPC Enabler", E75, 0},
    {"power-macintosh-8100", V(7, 1, 2), PPC, "PowerPC Enabler", E75, 0},
    {"workgroup-server-6150", V(7, 5, 0), PPC, "PowerPC Enabler 1.1.1", V(7, 5, 3), 0},
    {"workgroup-server-8150", V(7, 1, 2), PPC, "PowerPC Enabler", E75, 0},
    {"workgroup-server-9150-80", V(7, 1, 2), PPC, "PowerPC Enabler", E75, 0},
    {"power-macintosh-7100-80", V(7, 5, 0), PPC, "PowerPC Enabler 1.1.1", V(7, 5, 3), 0},
    {"power-macintosh-8100-100", V(7, 5, 0), PPC, "PowerPC Enabler 1.1.1", V(7, 5, 3), 0},
    {"power-macintosh-8100-110", V(7, 5, 0), PPC, "PowerPC Enabler 1.1.1", V(7, 5, 3), 0},
    {"workgroup-server-9150-120", V(7, 5, 0), PPC, "PowerPC Enabler 1.1.1", V(7, 5, 3), 0},
    {"power-macintosh-5200", V(7, 5, 1), PPC, "System Enabler 406", V(7, 5, 3), 0},
    {"power-macintosh-5300", V(7, 5, 1), PPC, "System Enabler 406", V(7, 5, 3), 0},
    {"power-macintosh-6200", V(7, 5, 1), PPC, "System Enabler 406", V(7, 5, 3), 0},
    {"power-macintosh-6300", V(7, 5, 1), PPC, "System Enabler 406", V(7, 5, 3), 0},
    {"power-macintosh-7200-v1", V(7, 5, 2), PPC, "System Enabler 701", V(7, 5, 3), 0},
    {"power-macintosh-7200", V(7, 5, 2), PPC, "System Enabler 701", V(7, 5, 3), 0},
    {"power-macintosh-7500", V(7, 5, 2), PPC, "System Enabler 701", V(7, 5, 3), 0},
    {"power-macintosh-8500", V(7, 5, 2), PPC, "System Enabler 701", V(7, 5, 3), 0},
    {"power-macintosh-9500", V(7, 5, 2), PPC, "System Enabler 701", V(7, 5, 3), 0},
    {"powerbook-duo-2300c", V(7, 5, 2), PPC, "PowerBook 5300/2300/190 Enabler", V(7, 6, 0), 0},
    {"power-macintosh-6400", V(7, 5, 3), PPC, "System Enabler 410", V(7, 5, 5), 0},
    {"power-macintosh-5400", V(7, 5, 3), PPC, "System Enabler 410", V(7, 5, 5), 0},
    {"powerbook-1400", V(7, 5, 3), PPC, NULL, 0, 0},
    {"powerbook-1400cs", V(7, 5, 3), PPC, NULL, 0, 0},
    {"power-macintosh-7300", V(7, 5, 5), PPC, "System Enabler 702", V(7, 6, 1), MC_COMPAT_NO_76},
    {"power-macintosh-7600", V(7, 5, 3), PPC, NULL, 0, 0},
    {"power-macintosh-8600", V(7, 5, 5), PPC, "System Enabler 702", V(7, 6, 1), MC_COMPAT_NO_76},
    {"power-macintosh-9600", V(7, 5, 5), PPC, "System Enabler 702", V(7, 6, 1), MC_COMPAT_NO_76},
    {"power-macintosh-6500", V(7, 5, 5), PPC, "System Enabler 411", V(7, 6, 1), MC_COMPAT_NO_76},
    {"power-macintosh-5500", V(7, 5, 5), PPC, "System Enabler 411", V(7, 6, 1), MC_COMPAT_NO_76},
    {"twentieth-anniversary-macintosh", V(7, 6, 1), PPC, NULL, 0, 0},
    {"power-macintosh-8600-300", V(7, 6, 1), PPC, NULL, 0, 0},
    {"power-macintosh-9600-350", V(7, 6, 1), PPC, NULL, 0, 0},
    {"power-macintosh-g3-v1", V(8, 0, 0), G3, "System Enabler 770", V(8, 1, 0), MC_COMPAT_GESTALT_LIST},
    {"power-macintosh-g3", V(8, 0, 0), G3, "System Enabler 770", V(8, 1, 0), MC_COMPAT_GESTALT_LIST},
    {"power-macintosh-g3-minitower", V(8, 0, 0), G3, "System Enabler 770", V(8, 1, 0), MC_COMPAT_GESTALT_LIST},
    {"power-macintosh-g3-all-in-one", V(8, 1, 0), G3, NULL, 0, 0},
    {"powerbook-g3-wallstreet", V(8, 1, 0), G3, NULL, 0, 0},
    {"powerbook-g3-pdq", V(8, 1, 0), G3, NULL, 0, 0},
};
#define COUNT (sizeof table / sizeof *table)

const mc_compat_entry *mc_compat_find(const char *model) {
    for (size_t i = 0; model && i < COUNT; ++i) if (!strcmp(table[i].model, model)) return &table[i];
    return NULL;
}
size_t mc_compat_count(void) { return COUNT; }
const mc_compat_entry *mc_compat_entry_at(size_t index) { return index < COUNT ? &table[index] : NULL; }

/* "System 7.1" or "Mac OS 8.0": the name Apple gave the release. */
static const char *release_words(uint16_t bcd, char *buffer, size_t capacity) {
    char number[16];
    snprintf(buffer, capacity, "%s %s", bcd >= V(7, 6, 0) ? "Mac OS" : "System", mc_version_text(bcd, number, sizeof number));
    return buffer;
}
void mc_compat_describe(const mc_model *m, char *out, size_t capacity) {
    if (!capacity) return;
    out[0] = 0;
    if (!m) return;
    if (m->console) { snprintf(out, capacity, "Starts the discs made for it (no Mac OS of its own to install)"); return; }
    if (mc_model_builtin(m)) { snprintf(out, capacity, "Runs no Mac OS: Livadix's own ROM shows the README and the changelog"); return; }
    const mc_compat_entry *e = mc_compat_find(m->key);
    if (!e) { snprintf(out, capacity, "Which Systems it runs is not known"); return; }
    char from[24], to[24], native[16];
    release_words(e->min_system, from, sizeof from);
    release_words(e->max_system, to, sizeof to);
    /* "System 7.1 to Mac OS 8.1", "Mac OS 8.0 to 9.2.2" */
    const char *to_text = !strncmp(from, to, 6) ? strchr(to, ' ') + 1 : to;
    if (!strncmp(from, "Mac OS", 6)) to_text = strrchr(to, ' ') + 1;
    int n = snprintf(out, capacity, "Runs %s to %s", from, to_text);
    if ((e->flags & MC_COMPAT_608L) && n > 0 && (size_t)n < capacity) n += snprintf(out + n, capacity - (size_t)n, " (and System 6.0.8L)");
    if ((e->flags & MC_COMPAT_NO_76) && n > 0 && (size_t)n < capacity) n += snprintf(out + n, capacity - (size_t)n, ", not Mac OS 7.6");
    if (e->enabler && n > 0 && (size_t)n < capacity) {
        if (e->native) n += snprintf(out + n, capacity - (size_t)n, "; before %s with %s", mc_version_text(e->native, native, sizeof native), e->enabler);
        else n += snprintf(out + n, capacity - (size_t)n, "; with %s", e->enabler);
    }
    if (mc_model_ppc(m) && strstr(m->cpu,"750") && n > 0 && (size_t)n < capacity)
        n += snprintf(out + n, capacity - (size_t)n, "; Mac OS X 10.0 to 10.2.8 (128 MB RAM minimum for 10.2)");
    if ((e->flags & MC_COMPAT_24BIT) && n > 0 && (size_t)n < capacity)
        snprintf(out + n, capacity - (size_t)n, "; 32-bit addressing needs MODE32 or the 32-Bit System Enabler");
}
static int lists(const uint16_t *boxes, size_t n, uint16_t box) {
    for (size_t i = 0; i < n; ++i) if (boxes[i] == box) return 1;
    return 0;
}
/* The System as its 'vers' says it: "System 7.1.1", "Mac OS 8.0". */
static const char *system_words(const mc_volume_info *v, char *buffer, size_t capacity) {
    char number[16];
    /* The short version string unless it is not a version (a build's name,
     * such as "Z2-9.0"): then the version number. */
    const char *text = v->system_text[0] >= '0' && v->system_text[0] <= '9' ? v->system_text
                                                                           : mc_version_text(v->system_version, number, sizeof number);
    snprintf(buffer, capacity, "%s %s", v->mac_os_x ? "Mac OS X" : v->system_version >= V(7, 6, 0) ? "Mac OS" : "System", text);
    return buffer;
}

uint32_t mc_compat_min_ram(const mc_volume_info *v) {
    if (!v || !v->bootable || !v->system_version) return 0;
    const uint16_t ver = v->system_version;
    /* Apple's Jaguar, Tiger and Leopard system requirements. */
    if (v->mac_os_x) return ver >= 0x1050 ? 512 : ver >= 0x1040 ? 256 : 128;
    return ver >= V(9, 0, 0) ? 32 : ver >= V(8, 5, 0) ? 16 : ver >= V(8, 0, 0) ? 12 : ver >= V(7, 6, 0) ? 8 : ver >= V(7, 5, 0) ? 4
         : ver >= V(7, 0, 0) ? 2 : 1;
}
int mc_compat_ram(const mc_volume_info *v, uint32_t ram_kb, mc_compat *out) {
    memset(out, 0, sizeof *out);
    const uint32_t need = mc_compat_min_ram(v);
    if (!ram_kb || !need || ram_kb >= need * 1024u) return out->status = MC_COMPAT_OK;
    char system[48], have[24];
    system_words(v, system, sizeof system);
    snprintf(out->reason, sizeof out->reason, "%s needs at least %u MB of memory, not %s MB%s", system, (unsigned)need,
             mc_config_ram_text(ram_kb, have, sizeof have),
             v->system_version >= V(9, 0, 0) ? " for an Apple-supported configuration" :
             v->system_version >= V(8, 0, 0) ? ": with less the Finder cannot start up" : "");
    return out->status = MC_COMPAT_WARNING;
}

int mc_compat_ram_most(const mc_volume_info *v, uint32_t most_kb, const char *model, mc_compat *out) {
    memset(out, 0, sizeof *out);
    const uint32_t need = mc_compat_min_ram(v);
    if (!most_kb || !need || most_kb >= need * 1024u) return out->status = MC_COMPAT_OK;
    char system[48], most[24];
    system_words(v, system, sizeof system);
    snprintf(out->reason, sizeof out->reason, "%s needs at least %u MB of memory; the %s takes at most %s MB", system, (unsigned)need,
             model, mc_config_ram_text(most_kb, most, sizeof most));
    return out->status = MC_COMPAT_WARNING;
}

/* The 'PTCH' ID of a ROM's patches in the System file, by the ROM's
 * checksum (docs/NOTES.md); 0 for the ROMs not checked. */
static int16_t rom_patch_id(uint32_t rom) {
    switch (rom) {
    case 0x4D1EEEE1: case 0x4D1EEAE1: case 0x4D1F8172: return 117;  /* Plus, 512Ke: version $0075 */
    case 0xB2E362A8: case 0xB306E171: case 0xA49F9914: return 630;  /* SE, SE FDHD, Classic: $0276 */
    case 0x97851DB6: case 0x9779D2C4: case 0x97221136: return 376;  /* Macintosh II family, SE/30: $0178 */
    case 0x96CA3846: case 0x96645F9C: return 890;                   /* Portable, PowerBook 100: $037A */
    default: return 0;
    }
}

int mc_compat_check(const mc_model *m, const mc_volume_info *v, uint32_t ram_kb, mc_compat *out) {
    memset(out, 0, sizeof *out);
    const mc_compat_entry *e = m ? mc_compat_find(m->key) : NULL;
    char system[48], need[16], top[16];
    if (mc_model_builtin(m)) {
        out->status = MC_COMPAT_UNKNOWN;
        snprintf(out->reason, sizeof out->reason, "the Built-in machine starts no System: it shows Livadix's README and changelog");
        return out->status;
    }
    if (!v->bootable) {
        out->status = MC_COMPAT_UNKNOWN;
        snprintf(out->reason, sizeof out->reason, "not a startup disk: no blessed System Folder with a System file");
        return out->status;
    }
    if (v->mac_os_x) {
        system_words(v, system, sizeof system);
        if (!m || !mc_model_ppc(m) || !strstr(m->cpu, "750")) {
            out->status = MC_COMPAT_WARNING;
            snprintf(out->reason, sizeof out->reason, "%s needs a G3 Macintosh; this model is not a G3", system);
            return out->status;
        }
        if (!v->system_version || v->system_version > 0x1028) {
            out->status = MC_COMPAT_UNKNOWN;
            snprintf(out->reason, sizeof out->reason, "%s: Livadix's verified G3 target is Mac OS X 10.2.8", system);
            return out->status;
        }
        if (mc_compat_ram(v, ram_kb, out) != MC_COMPAT_OK) return out->status;
        snprintf(out->reason, sizeof out->reason, "%s on the %s with at least 128 MB RAM", system, m->name);
        return out->status = MC_COMPAT_OK;
    }
    if (!v->system_version && v->system_unversioned) {
        /* The System files before 6.0 have no 'vers' resource. */
        char old[16];
        if (e && e->min_system >= V(6, 0, 0)) {
            out->status = MC_COMPAT_WARNING;
            snprintf(out->reason, sizeof out->reason, "a System before 6.0 (its file has no 'vers' resource) does not run on the %s: it needs System %s or later",
                     m->name, mc_version_text(e->min_system, old, sizeof old));
        } else {
            out->status = MC_COMPAT_UNKNOWN;
            snprintf(out->reason, sizeof out->reason, "a System before 6.0 (its file has no 'vers' resource): its version cannot be checked against what the model runs");
        }
        return out->status;
    }
    if (!v->system_version) {
        out->status = MC_COMPAT_WARNING;
        snprintf(out->reason, sizeof out->reason, "the System file's version ('vers' resource) could not be read");
        return out->status;
    }
    system_words(v, system, sizeof system);
    if (!e) {
        out->status = MC_COMPAT_UNKNOWN;
        snprintf(out->reason, sizeof out->reason, "%s; no compatibility data for this model", system);
        return out->status;
    }
    /* Apple G3 CD 691-1826-A, Enabler770 gbly -16385 lists 510,
     * the guest Gestalt ID; earlier enablers list the legacy box flag. */
    const uint16_t ver = v->system_version, box = (uint16_t)(m->gestalt - ((e->flags & MC_COMPAT_GESTALT_LIST) ? 0 : 6));
    out->status = MC_COMPAT_WARNING;
    if (ver > e->max_system) {
        mc_version_text(e->max_system, top, sizeof top);
        if (ver >= V(9, 2, 0) && e->max_system >= V(9, 0, 0))
            snprintf(out->reason, sizeof out->reason, "%s needs a G3; the %s has a %s (it runs up to Mac OS %s)", system, m->name,
                     m->cpu, top);
        else if (ver >= V(8, 5, 0) && e->max_system < V(8, 5, 0) && e->max_system >= V(8, 0, 0))
            snprintf(out->reason, sizeof out->reason, "%s needs a PowerPC; the %s has a %s (it runs up to Mac OS %s)", system, m->name,
                     m->cpu, top);
        else if (ver >= V(8, 0, 0) && e->max_system < V(8, 0, 0))
            snprintf(out->reason, sizeof out->reason, "%s needs a 68040; the %s has a %s (it runs up to Mac OS %s)", system,
                     m->name, m->cpu, top);
        else if (ver >= V(7, 6, 0) && e->max_system < V(7, 6, 0))
            snprintf(out->reason, sizeof out->reason, "%s needs a 32-bit clean ROM and a 68030 or later; the %s runs up to System %s",
                     system, m->name, top);
        else
            snprintf(out->reason, sizeof out->reason, "%s is newer than the %s runs (up to %s)", system, m->name, top);
        return out->status;
    }
    const int is_608l = ver == V(6, 0, 8) && v->system_text[0] && v->system_text[strlen(v->system_text) - 1] == 'L';
    if (ver < e->min_system && !(is_608l && (e->flags & MC_COMPAT_608L))) {
        mc_version_text(e->min_system, need, sizeof need);
        snprintf(out->reason, sizeof out->reason, "%s does not run on the %s: it needs System %s or later%s", system, m->name, need,
                 (e->flags & MC_COMPAT_608L) && ver < V(7, 0, 0) ? " (or 6.0.8L)" : "");
        return out->status;
    }
    if ((e->flags & MC_COMPAT_NO_76) && ver == V(7, 6, 0)) {
        snprintf(out->reason, sizeof out->reason, "%s does not run on the %s: Mac OS 7.6.1 and the Systems before 7.6 do", system,
                 m->name);
        return out->status;
    }
    /* The System's ROM patches: one 'PTCH' resource per ROM it supports,
     * numbered by the ROM's version word (offset 8 of its header): 117
     * ($0075) for the Plus's, 630 ($0276) for the SE's and Classic's, 376
     * ($0178) for the Macintosh II's, 890 ($037A) for the Portable's, as the
     * System files of 6.0.8 and 7.1 show. A System the Installer made for
     * other Macintoshes lacks this ROM's and stops at an unimplemented trap. */
    const int16_t patch = rom_patch_id(m->rom);
    if (patch && v->system_ptch_count) {
        size_t k = 0;
        while (k < v->system_ptch_count && v->system_ptch[k] != patch) ++k;
        if (k == v->system_ptch_count) {
            snprintf(out->reason, sizeof out->reason,
                     "%s on this disk holds no patches for the %s's ROM ('PTCH' %d): it was installed for other Macintoshes; "
                     "install it for this one", system, m->name, (int)patch);
            return out->status;
        }
    }
    /* System 7.1 to 7.5: the System's own list, else an enabler's. */
    const char *with = NULL;
    if (!lists(v->system_boxes, v->system_box_count, box))
        for (size_t i = 0; i < v->enabler_count && !with; ++i)
            if (lists(v->enablers[i].boxes, v->enablers[i].box_count, box)) with = v->enablers[i].name;
    if (!with && e->enabler && ver >= V(7, 1, 0) && ver < e->native && !lists(v->system_boxes, v->system_box_count, box)) {
        /* An enabler without a readable list counts by its name. */
        for (size_t i = 0; i < v->enabler_count && !with; ++i)
            if (!v->enablers[i].box_count && strstr(e->enabler, v->enablers[i].name)) with = v->enablers[i].name;
        if (!with) {
            snprintf(out->reason, sizeof out->reason, "%s on a %s needs %s, not found on the disk", system, m->name, e->enabler);
            return out->status;
        }
    }
    /* Enough memory for the System; above 8 MiB, 32-bit addressing. */
    mc_compat memory;
    char have[24];
    if (mc_compat_ram(v, ram_kb, &memory) != MC_COMPAT_OK) { *out = memory; return out->status; }
    if (ram_kb > 8192u && ver < V(7, 0, 0)) {
        snprintf(out->reason, sizeof out->reason, "%s addresses 24 bits: it uses 8 of the %s MiB", system,
                 mc_config_ram_text(ram_kb, have, sizeof have));
        return out->status;
    }
    if (ram_kb > 8192u && (e->flags & MC_COMPAT_24BIT) && !v->mode32) {
        snprintf(out->reason, sizeof out->reason,
                 "the %s's ROM addresses 24 bits: %s uses 8 of the %s MiB without MODE32 or the 32-Bit System Enabler (not on the disk)",
                 m->name, system, mc_config_ram_text(ram_kb, have, sizeof have));
        return out->status;
    }
    out->status = MC_COMPAT_OK;
    if (with) snprintf(out->reason, sizeof out->reason, "%s runs on the %s with %s", system, m->name, with);
    else snprintf(out->reason, sizeof out->reason, "%s runs on the %s", system, m->name);
    return out->status;
}

/* The images in the order the startup search tries them: the CD with
 * boot_priority cdrom, the floppy drives (a disk in a drive starts the
 * Macintosh before any hard disk does), the IDE disks, then the SCSI disks
 * by ID. Returns how many it put in out. */
#define STARTUP_IMAGES (1 + MC_CONFIG_FLOPPIES + MC_CONFIG_DISKS)
static size_t startup_order(const mc_config *c, const char **out) {
    size_t n = 0;
    if (c->boot_priority == MC_BOOT_CDROM && c->cdrom[0]) out[n++] = c->cdrom;
    for (size_t i = 0; i < c->floppy_count; ++i)
        if (!c->floppy_off[i] && c->floppies[i].image[0]) out[n++] = c->floppies[i].image;
    /* The IDE disk of the machines that have one: their new PRAM names it
     * the startup device (NOTES.md, "SCSI IDs"). */
    for (size_t i = 0; i < c->disk_count; ++i)
        if (c->disks[i].bus == MC_BUS_ATA) out[n++] = c->disks[i].image;
    /* SCSI IDs: the written ones, then the free IDs in order (3 is the CD). */
    static const int free_ids[] = {0, 1, 2, 4, 5, 6};
    int id[MC_CONFIG_DISKS], taken[8] = {0};
    for (size_t i = 0; i < c->disk_count; ++i) {
        id[i] = c->disks[i].scsi_id_set ? c->disks[i].scsi_id : -1;
        if (c->disks[i].bus == MC_BUS_ATA) id[i] = -2;      /* not on SCSI */
        if (id[i] >= 0) taken[id[i]] = 1;
    }
    for (size_t i = 0; i < c->disk_count; ++i)
        if (id[i] == -1)
            for (size_t k = 0; k < 6; ++k)
                if (!taken[free_ids[k]]) { id[i] = free_ids[k]; taken[id[i]] = 1; break; }
    static const int order[] = {0, 6, 5, 4, 2, 1};
    for (size_t k = 0; k < 6; ++k)
        for (size_t i = 0; i < c->disk_count; ++i)
            if (id[i] == order[k]) out[n++] = c->disks[i].image;
    return n;
}

const char *mc_config_boot_image(const mc_config *c, mc_volume_info *info) {
    char error[256];
    const char *images[STARTUP_IMAGES];
    const size_t n = startup_order(c, images);
    for (size_t i = 0; i < n; ++i)
        if (mc_volume_inspect(images[i], info, error, sizeof error) && info->bootable) return images[i];
    memset(info, 0, sizeof *info);
    return NULL;
}

const char *mc_config_unexamined_image(const mc_config *c, mc_volume_info *info) {
    char error[256];
    const char *images[STARTUP_IMAGES];
    const size_t n = startup_order(c, images);
    for (size_t i = 0; i < n; ++i)
        if (mc_volume_inspect(images[i], info, error, sizeof error) && info->kind == MC_IMAGE_HFS_PLUS) return images[i];
    memset(info, 0, sizeof *info);
    return NULL;
}
