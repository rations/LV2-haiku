/*
 * lv2jackhost — headless LV2 host: a stereo JACK client with a stdin
 * parameter REPL, for running and validating LV2 plugins on Haiku.
 * The hosting itself lives in the shared core (lv2host_core.h).
 */
#include "../common/lv2host_core.h"

#include <signal.h>
#include <stdio.h>
#include <string.h>

static void print_params(const Lv2Host *h)
{
    printf("%u parameter(s):\n", h->n_params);
    for (uint32_t i = 0; i < h->n_params; i++) {
        const Lv2hParam *pr = &h->params[i];
        printf("  [%u] %-24s min %-9g max %-9g def %-9g cur %-9g", i, pr->name, pr->min, pr->max,
               pr->def, lv2host_get_control(h, pr->port_index));
        if (pr->toggled) {
            printf(" [toggled]");
        }
        if (pr->integer) {
            printf(" [integer]");
        }
        if (pr->enumeration) {
            printf(" [enum]");
        }
        if (pr->n_scale_points) {
            printf(" [%u scale points]", pr->n_scale_points);
        }
        printf("\n");
    }
}

static void print_outs(const Lv2Host *h)
{
    printf("%u control output(s):\n", h->n_outs);
    for (uint32_t i = 0; i < h->n_outs; i++) {
        const Lv2hParam *pr = &h->outs[i];
        printf("  [%u] %-24s min %-9g max %-9g cur %-9g\n", i, pr->name, pr->min, pr->max,
               lv2host_get_control(h, pr->port_index));
    }
}

/* SIGTERM/SIGINT/SIGHUP request a clean shutdown: the handler only sets a
   flag; installed without SA_RESTART so the blocking REPL fgets() returns
   with EINTR and main() runs the normal teardown (jack_client_close() must
   happen — a killed client leaves the server graph wedged). */
static volatile sig_atomic_t quit_requested = 0;

static void quit_signal_handler(int sig)
{
    (void)sig;
    quit_requested = 1;
}

static void repl(Lv2Host *h)
{
    char line[256];
    printf("commands: list | get N | set N VALUE | out | quit\n");
    while (!quit_requested && fgets(line, sizeof(line), stdin)) {
        unsigned idx = 0;
        float val = 0.0f;
        if (!strncmp(line, "quit", 4) || !strncmp(line, "exit", 4)) {
            break;
        }
        if (!strncmp(line, "list", 4)) {
            print_params(h);
        } else if (!strncmp(line, "out", 3)) {
            print_outs(h);
        } else if (sscanf(line, "set %u %f", &idx, &val) == 2) {
            if (!lv2host_set_param(h, idx, val)) {
                printf("no such parameter %u\n", idx);
                continue;
            }
            const Lv2hParam *pr = &h->params[idx];
            if (val < pr->min) {
                val = pr->min;
            }
            if (val > pr->max) {
                val = pr->max;
            }
            printf("%s = %g\n", pr->name, val);
        } else if (sscanf(line, "get %u", &idx) == 1) {
            if (idx >= h->n_params) {
                printf("no such parameter %u\n", idx);
                continue;
            }
            printf("%s = %g\n", h->params[idx].name,
                   lv2host_get_control(h, h->params[idx].port_index));
        } else if (line[0] != '\n' && line[0] != '\0') {
            printf("commands: list | get N | set N VALUE | out | quit\n");
        }
        if (h->block_too_big) {
            printf("warning: JACK buffer size grew past the pre-allocated block; "
                   "output muted — restart the host\n");
        }
    }
}

int main(int argc, char **argv)
{
    if (argc != 2) {
        fprintf(stderr, "usage: lv2jackhost <plugin-uri>\n");
        fprintf(stderr, "       (list installed plugins with lv2ls)\n");
        return 1;
    }

    static Lv2Host h;
    if (!lv2host_open(&h, argv[1], "lv2jackhost")) {
        return 1;
    }
    lv2host_autoconnect(&h);

    LilvNode *name = lilv_plugin_get_name(h.plugin);
    printf("loaded: %s <%s>\n", name ? lilv_node_as_string(name) : "?", argv[1]);
    if (name) {
        lilv_node_free(name);
    }
    printf("audio: %d in, %d out%s; sr %.0f Hz, block %u\n", h.n_audio_in, h.n_audio_out,
           h.dual_mono ? " (dual-mono x2)" : "", h.sample_rate, h.max_block);
    print_params(&h);
    if (h.n_outs) {
        print_outs(&h);
    }

    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = quit_signal_handler;
    sigemptyset(&sa.sa_mask);
    if (sigaction(SIGTERM, &sa, NULL) != 0 || sigaction(SIGINT, &sa, NULL) != 0 ||
        sigaction(SIGHUP, &sa, NULL) != 0) {
        fprintf(stderr, "warning: failed to install signal handlers\n");
    }

    repl(&h);

    unsigned xruns = h.n_xruns;
    lv2host_close(&h);
    printf("xruns while running: %u\n", xruns);
    return 0;
}
