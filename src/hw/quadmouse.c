/* Livadix — the quadrature mouse (quadmouse.h). Livadix's own code. */
#include "quadmouse.h"

#include <string.h>

#define MOST 4096       /* steps waiting per axis */

static void step(mc_quadmouse *m) {
    /* Right: X2 differs from the new X1; down: Y2 equals the new Y1. An
     * axis whose last edge is not taken yet waits. */
    if (m->dx && (!m->ready || m->ready(m->ready_ctx, 0))) {
        const int right = m->dx > 0;
        m->dx += right ? -1 : 1;
        m->x1 ^= 1u;
        m->x2 = (uint8_t)(m->x1 ^ (right ? 1u : 0u));
        mc_dev_line(&m->dev, MC_QUADMOUSE_LINE_X2, m->x2);
        mc_dev_line(&m->dev, MC_QUADMOUSE_LINE_X1, m->x1);
    }
    if (m->dy && (!m->ready || m->ready(m->ready_ctx, 1))) {
        const int down = m->dy > 0;
        m->dy += down ? -1 : 1;
        m->y1 ^= 1u;
        m->y2 = (uint8_t)(m->y1 ^ (down ? 0u : 1u));
        mc_dev_line(&m->dev, MC_QUADMOUSE_LINE_Y2, m->y2);
        mc_dev_line(&m->dev, MC_QUADMOUSE_LINE_Y1, m->y1);
    }
}

static void quadmouse_event(void *dev, uint64_t now) {
    mc_quadmouse *m = dev;
    step(m);
    m->armed = m->dx || m->dy;
    mc_dev_schedule(&m->dev, m->armed ? now + MC_QUADMOUSE_STEP : MC_NEVER);
}

static void quadmouse_reset(void *dev) {
    mc_quadmouse *m = dev;
    m->dx = m->dy = 0;
    m->armed = 0;
    mc_dev_schedule(&m->dev, MC_NEVER);
}

const mc_dev_ops mc_quadmouse_ops = {NULL, NULL, quadmouse_event, quadmouse_reset, NULL};

void mc_quadmouse_init(mc_quadmouse *m, const mc_hw_host *host, const char *name) {
    memset(m, 0, sizeof *m);
    m->dev.ops = &mc_quadmouse_ops;
    m->dev.host = host;
    m->dev.name = name;
}

int mc_quadmouse_move(mc_quadmouse *m, int dx, int dy, int button) {
    if ((m->dx > MOST || m->dx < -MOST) && dx) return 0;
    if ((m->dy > MOST || m->dy < -MOST) && dy) return 0;
    m->dx += dx;
    m->dy += dy;
    if ((uint8_t)(button != 0) != m->button) {
        m->button = (uint8_t)(button != 0);
        mc_dev_line(&m->dev, MC_QUADMOUSE_LINE_BUTTON, m->button);
    }
    if ((m->dx || m->dy) && !m->armed) {
        m->armed = 1;
        mc_dev_schedule(&m->dev, mc_dev_now(&m->dev) + MC_QUADMOUSE_STEP);
    }
    return 1;
}
