/* The fake machine host of the chip tests (hw_test.h). */
#include "hw_test.h"

#include <stdio.h>
#ifdef _WIN32
#include <direct.h>
#define make_dir(p) _mkdir(p)
#else
#include <sys/stat.h>
#define make_dir(p) mkdir(p, 0777)
#endif

int mc_test_failures;

static int slot(hw_fake *f, const void *dev) {
    for (int i = 0; i < HW_FAKE_DEVS; i++)
        if (f->dev[i] == dev) return i;
    for (int i = 0; i < HW_FAKE_DEVS; i++)
        if (!f->dev[i]) { f->dev[i] = (void *)dev; return i; }
    fprintf(stderr, "hw_fake: too many devices\n");
    return 0;
}
static uint64_t fake_now(void *ctx) { return ((hw_fake *)ctx)->now; }
static void fake_schedule(void *ctx, void *dev, uint64_t when) {
    hw_fake *f = ctx;
    f->deadline[slot(f, dev)] = when;
}
static void fake_line(void *ctx, void *dev, int line, int state) {
    hw_fake *f = ctx;
    int i = slot(f, dev);
    if (line >= 0 && line < HW_FAKE_LINES) {
        if (f->line[i][line] != (state != 0)) f->changes[i][line]++;
        f->line[i][line] = state != 0;
        f->value[i][line] = state;
        f->reports[i][line]++;
    }
    if (f->on_line) f->on_line(f, dev, line, state);
}

void hw_fake_init(hw_fake *f) {
    memset(f, 0, sizeof *f);
    f->host.ctx = f;
    f->host.now = fake_now;
    f->host.schedule = fake_schedule;
    f->host.line = fake_line;
    for (int i = 0; i < HW_FAKE_DEVS; i++) {
        f->deadline[i] = MC_NEVER;
        for (int l = 0; l < HW_FAKE_LINES; l++) f->value[i][l] = -1;
    }
}
uint64_t hw_fake_deadline(hw_fake *f, const void *dev) { return f->deadline[slot(f, dev)]; }
int hw_fake_line(hw_fake *f, const void *dev, int line) { return f->line[slot(f, dev)][line]; }
unsigned hw_fake_changes(hw_fake *f, const void *dev, int line) { return f->changes[slot(f, dev)][line]; }
int hw_fake_value(hw_fake *f, const void *dev, int line) { return f->value[slot(f, dev)][line]; }
unsigned hw_fake_reports(hw_fake *f, const void *dev, int line) { return f->reports[slot(f, dev)][line]; }

void hw_fake_advance(hw_fake *f, uint64_t to) {
    for (;;) {
        int next = -1;
        for (int i = 0; i < HW_FAKE_DEVS; i++)
            if (f->dev[i] && f->deadline[i] <= to && (next < 0 || f->deadline[i] < f->deadline[next])) next = i;
        if (next < 0) break;
        uint64_t when = f->deadline[next];
        if (when > f->now) f->now = when;
        f->deadline[next] = MC_NEVER;
        mc_dev *d = f->dev[next];
        if (d->ops && d->ops->event) d->ops->event(d, f->now);
    }
    if (to > f->now) f->now = to;
}
void hw_fake_step(hw_fake *f, uint64_t periods) { hw_fake_advance(f, f->now + periods); }

const char *hw_work_path(const char *suite, const char *name) {
    static char path[4][1024];
    static unsigned next;
    char *p = path[next++ % 4];
    snprintf(p, 1024, "%s/tests", MC_WORK); make_dir(p);
    snprintf(p, 1024, "%s/tests/%s", MC_WORK, suite); make_dir(p);
    snprintf(p, 1024, "%s/tests/%s/%s", MC_WORK, suite, name);
    return p;
}
