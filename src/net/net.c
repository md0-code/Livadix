/* Livadix — the Ethernet segment, its thread and its queues (net.h).
 *
 * Livadix's own code. */
#include "net.h"
#include "share.h"
#include "sys.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define FRAME_MAX 1514
/* Frames waiting each way; beyond this a new frame is dropped (TCP sends
 * it again, as after a congested Ethernet). */
#define QUEUE 512

typedef struct queue {
    uint8_t (*data)[FRAME_MAX];
    uint16_t *length;
    unsigned head, count;
} queue;

typedef struct host {
    const mc_net_host_ops *ops;
    void *self;
} host;

struct mc_net {
    host hosts[MC_NET_HOSTS];
    unsigned nhosts;
    mc_thread thread;
    int running, stop;
    mc_mutex lock;
    mc_cond cond;               /* the thread's wait when no host waits for it */
    int pending;
    queue out, in;              /* out: from the Macintosh; in: to it */
    mc_nat *nat;                /* the NAT host, when mc_net_open put one on */
    mc_nat_stats stats;
};

static int q_init(queue *q) {
    q->data = malloc((size_t)QUEUE * FRAME_MAX);
    q->length = malloc(QUEUE * sizeof *q->length);
    q->head = q->count = 0;
    return q->data && q->length;
}
static void q_free(queue *q) {
    free(q->data);
    free(q->length);
}
static void q_put(queue *q, const uint8_t *frame, size_t length) {
    if (q->count == QUEUE) return;
    if (length > FRAME_MAX) length = FRAME_MAX;
    const unsigned at = (q->head + q->count) % QUEUE;
    memcpy(q->data[at], frame, length);
    q->length[at] = (uint16_t)length;
    q->count++;
}

/* The host the thread waits in: the first with a wake (net.h), or none. */
static int waiter(const mc_net *n) {
    for (unsigned k = 0; k < n->nhosts; k++)
        if (n->hosts[k].ops->wake && n->hosts[k].ops->poll) return (int)k;
    return -1;
}

static void wake(mc_net *n) {
    const int w = waiter(n);
    if (w >= 0) {
        n->hosts[w].ops->wake(n->hosts[w].self);
        return;
    }
    mc_mutex_lock(&n->lock);
    n->pending = 1;
    mc_cond_signal(&n->cond);
    mc_mutex_unlock(&n->lock);
}

void mc_net_emit(mc_net *n, const void *from, const uint8_t *frame, size_t length) {
    mc_mutex_lock(&n->lock);
    q_put(&n->in, frame, length);
    mc_mutex_unlock(&n->lock);
    for (unsigned k = 0; k < n->nhosts; k++)
        if (n->hosts[k].self != from && n->hosts[k].ops->input) n->hosts[k].ops->input(n->hosts[k].self, frame, length);
}

static MC_THREAD_RETURN net_main(void *arg) {
    mc_net *n = arg;
    uint8_t frame[FRAME_MAX];
    for (;;) {
        mc_mutex_lock(&n->lock);
        const int stop = n->stop;
        mc_mutex_unlock(&n->lock);
        if (stop) break;
        /* The Macintosh's frames to every host, one at a time outside the lock. */
        for (;;) {
            size_t length = 0;
            mc_mutex_lock(&n->lock);
            if (n->out.count) {
                length = n->out.length[n->out.head];
                memcpy(frame, n->out.data[n->out.head], length);
                n->out.head = (n->out.head + 1) % QUEUE;
                n->out.count--;
            }
            mc_mutex_unlock(&n->lock);
            if (!length) break;
            for (unsigned k = 0; k < n->nhosts; k++)
                if (n->hosts[k].ops->input) n->hosts[k].ops->input(n->hosts[k].self, frame, length);
        }
        const int w = waiter(n);
        for (unsigned k = 0; k < n->nhosts; k++)
            if ((int)k != w && n->hosts[k].ops->poll) n->hosts[k].ops->poll(n->hosts[k].self, 0);
        /* The waiting host waits for its own work or a wake; without one the
           thread waits for the Macintosh. The other hosts are served at
           least every 20 ms. */
        const int most = n->nhosts > (w >= 0 ? 1u : 0u) ? 20 : 100;
        if (w >= 0) n->hosts[w].ops->poll(n->hosts[w].self, most);
        else {
            mc_mutex_lock(&n->lock);
            if (!n->pending && !n->stop && !n->out.count) mc_cond_wait_ms(&n->cond, &n->lock, most);
            n->pending = 0;
            mc_mutex_unlock(&n->lock);
        }
        if (n->nat) {
            mc_nat_stats s;
            mc_nat_get_stats(n->nat, &s);
            mc_mutex_lock(&n->lock);
            n->stats = s;
            mc_mutex_unlock(&n->lock);
        }
    }
    return 0;
}

mc_net *mc_net_create(char *error, size_t capacity) {
    mc_net *n = calloc(1, sizeof *n);
    if (!n || !q_init(&n->out) || !q_init(&n->in)) {
        if (n) { q_free(&n->out); q_free(&n->in); free(n); }
        snprintf(error, capacity, "Out of memory");
        return NULL;
    }
    mc_mutex_init(&n->lock);
    mc_cond_init(&n->cond);
    return n;
}

int mc_net_attach_host(mc_net *n, const mc_net_host_ops *ops, void *self) {
    if (n->running || n->nhosts == MC_NET_HOSTS) return 0;
    n->hosts[n->nhosts].ops = ops;
    n->hosts[n->nhosts].self = self;
    n->nhosts++;
    return 1;
}

int mc_net_start(mc_net *n, char *error, size_t capacity) {
    if (!mc_thread_start(&n->thread, net_main, n)) {
        snprintf(error, capacity, "Cannot start the network's thread");
        return 0;
    }
    n->running = 1;
    return 1;
}

void mc_net_close(mc_net *n) {
    if (!n) return;
    if (n->running) {
        mc_mutex_lock(&n->lock);
        n->stop = 1;
        mc_mutex_unlock(&n->lock);
        wake(n);
        mc_thread_join(n->thread);
    }
    for (unsigned k = 0; k < n->nhosts; k++)
        if (n->hosts[k].ops->destroy) n->hosts[k].ops->destroy(n->hosts[k].self);
    mc_cond_free(&n->cond);
    mc_mutex_free(&n->lock);
    q_free(&n->out);
    q_free(&n->in);
    free(n);
}

void mc_net_send(mc_net *n, const uint8_t *frame, size_t length) {
    mc_mutex_lock(&n->lock);
    q_put(&n->out, frame, length);
    mc_mutex_unlock(&n->lock);
    wake(n);
}

int mc_net_next(mc_net *n, uint8_t *frame, size_t *length) {
    int got = 0;
    mc_mutex_lock(&n->lock);
    if (n->in.count) {
        *length = n->in.length[n->in.head];
        memcpy(frame, n->in.data[n->in.head], *length);
        got = 1;
    }
    mc_mutex_unlock(&n->lock);
    return got;
}

void mc_net_pop(mc_net *n) {
    mc_mutex_lock(&n->lock);
    if (n->in.count) {
        n->in.head = (n->in.head + 1) % QUEUE;
        n->in.count--;
    }
    mc_mutex_unlock(&n->lock);
}

/* ---- the NAT as a host ------------------------------------------------------ */

typedef struct nat_host {
    mc_net *net;
    mc_nat *nat;
} nat_host;

static void nat_output(void *ctx, const uint8_t *frame, size_t length) {
    nat_host *h = ctx;
    mc_net_emit(h->net, h, frame, length);
}
static void nat_input(void *self, const uint8_t *frame, size_t length) { mc_nat_input(((nat_host *)self)->nat, frame, length); }
static void nat_poll(void *self, int timeout_ms) { mc_nat_poll(((nat_host *)self)->nat, timeout_ms); }
static void nat_wake(void *self) { mc_nat_wake(((nat_host *)self)->nat); }
static void nat_destroy(void *self) {
    nat_host *h = self;
    mc_nat_destroy(h->nat);
    free(h);
}
static const mc_net_host_ops nat_ops = {nat_input, nat_poll, nat_wake, nat_destroy};

/* The segment with the NAT on it, not yet started. */
static mc_net *open_nat(const mc_nat_config *config, char *error, size_t capacity) {
    mc_net *n = mc_net_create(error, capacity);
    nat_host *h = n ? calloc(1, sizeof *h) : NULL;
    if (!h) {
        if (n) snprintf(error, capacity, "Out of memory");
        mc_net_close(n);
        return NULL;
    }
    h->net = n;
    mc_nat_config c = *config;
    c.output = nat_output;
    c.output_ctx = h;
    h->nat = mc_nat_create(&c, error, capacity);
    if (!h->nat) { free(h); mc_net_close(n); return NULL; }
    n->nat = h->nat;
    mc_net_attach_host(n, &nat_ops, h);
    return n;
}

mc_net *mc_net_open(const mc_nat_config *config, char *error, size_t capacity) {
    mc_net *n = open_nat(config, error, capacity);
    if (n && !mc_net_start(n, error, capacity)) { mc_net_close(n); return NULL; }
    return n;
}

void mc_net_stats(mc_net *n, mc_nat_stats *stats) {
    mc_mutex_lock(&n->lock);
    *stats = n->stats;
    mc_mutex_unlock(&n->lock);
}

uint16_t mc_net_forward_port(mc_net *n, unsigned index) { return n->nat ? mc_nat_forward_port(n->nat, index) : 0; }

static void log_line(void *ctx, const char *line) {
    (void)ctx;
    fprintf(stderr, "network: %s\n", line);
}

mc_net *mc_net_open_config(const mc_config *c, char *error, size_t capacity) {
    if (capacity) error[0] = 0;
    if (c->network == MC_NETWORK_NONE) return NULL;
    mc_nat_config nc;
    memset(&nc, 0, sizeof nc);
    for (size_t k = 0; k < c->forward_count && k < MC_NAT_FORWARDS; k++) {
        nc.forward[k].udp = c->forwards[k].udp;
        nc.forward[k].bind = c->forwards[k].address;
        nc.forward[k].host_port = c->forwards[k].host_port;
        nc.forward[k].guest_port = c->forwards[k].guest_port;
        nc.forwards++;
    }
    if (getenv("LIVADIX_NET_LOG")) nc.log = log_line;
    mc_net *n = open_nat(&nc, error, capacity);
    if (!n) return NULL;
    /* The file server (sharing:), when folders are shared. */
    if (c->share_count) {
        mc_afp_config ac;
        memset(&ac, 0, sizeof ac);
        snprintf(ac.server_name, sizeof ac.server_name, "%s", c->sharing_name);
        ac.metadata = c->sharing_metadata == MC_SHARING_APPLEDOUBLE ? MC_META_APPLEDOUBLE : MC_META_STREAMS;
        snprintf(ac.password, sizeof ac.password, "%s", c->sharing_password);
        for (size_t k = 0; k < c->share_count && k < MC_AFP_VOLUMES; k++) {
            snprintf(ac.volumes[k].name, sizeof ac.volumes[k].name, "%s", c->shares[k].name);
            snprintf(ac.volumes[k].path, sizeof ac.volumes[k].path, "%s", c->shares[k].path);
            ac.volumes[k].readonly = c->shares[k].readonly;
            ac.volume_count++;
        }
        char why[512];
        mc_share *share = mc_share_create(&ac, why, sizeof why);
        if (!share) {
            snprintf(error, capacity, "Sharing: %s", why);
            mc_net_close(n);
            return NULL;
        }
        for (unsigned k = 0; k < ac.volume_count; k++) {
            const char *note = NULL;
            if (mc_afp_volume_metadata(mc_share_afp(share), k, &note) >= 0 && note && note[0]) fprintf(stderr, "Sharing: %s\n", note);
        }
        if (!mc_share_attach(n, share)) {
            mc_share_destroy(share);
            snprintf(error, capacity, "Sharing: the network has no room for the file server");
            mc_net_close(n);
            return NULL;
        }
    }
    if (!mc_net_start(n, error, capacity)) {
        mc_net_close(n);
        return NULL;
    }
    return n;
}

/* ---- the machine ------------------------------------------------------------- */

static void machine_transmit(void *ctx, const uint8_t *frame, size_t length) { mc_net_send(ctx, frame, length); }

void mc_net_attach(mc_net *n, mc_machine *m) {
    if (!n) { mc_machine_set_net(m, NULL); return; }
    const mc_net_host ops = {n, machine_transmit};
    mc_machine_set_net(m, &ops);
}

void mc_net_pump(mc_net *n, mc_machine *m) {
    if (!n) return;
    uint8_t frame[FRAME_MAX];
    size_t length;
    while (mc_net_next(n, frame, &length) && mc_machine_net_receive(m, frame, length)) mc_net_pop(n);
}
