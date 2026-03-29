/* =============================================================================
 * examples/signal_monitor.c — libash signal monitor example
 *
 * Usage: ash-example-monitor [host] [port] [iface]
 *        Default: host=127.0.0.1, port=4000, iface=vcan0
 *
 * Demonstrates: attach to a CAN interface, define a cyclic signal, then print
 * decoded SIG_RX values to stdout as they arrive.
 *
 * This is a passive monitor.  It defines WheelSpeed and starts cyclic TX, but
 * SIG_RX events are only delivered when a frame is received from the bus.  On
 * vcan0 the server does not receive its own transmitted frames, so run
 * ash-example-drive (or another client writing to a frame on the same
 * interface) in a second terminal to generate observable traffic.
 * ============================================================================= */

#include "ash/ash.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <stdint.h>

#define MONITOR_DURATION_S  5   /* poll for this many seconds then exit */
#define TX_PERIOD_MS       50   /* cyclic transmit period */

static volatile int g_stop = 0;

static void sigint_handler(int sig) { (void)sig; g_stop = 1; }

int main(int argc, char *argv[])
{
    const char *host  = (argc > 1) ? argv[1] : "127.0.0.1";
    uint16_t    port  = (argc > 2) ? (uint16_t)atoi(argv[2]) : 4000;
    const char *iface = (argc > 3) ? argv[3] : "vcan0";

    signal(SIGINT, sigint_handler);

    /* ------------------------------------------------------------------ */
    /* Connect                                                              */
    /* ------------------------------------------------------------------ */
    ash_ctx_t *ctx = ash_connect(host, port, "signal-monitor");
    if (!ctx) {
        fprintf(stderr, "ash_connect failed\n");
        return 1;
    }
    printf("Connected to %s:%u\n", host, port);

    /* Ensure vcan0 exists; ignore error if it's already there */
    ash_vcan_create(ctx, iface);

    /* ------------------------------------------------------------------ */
    /* Clean up any leftover definitions from a prior run                  */
    /* ------------------------------------------------------------------ */
    ash_delete_def(ctx, "MonitorFrame",  ASH_DEF_FRAME);
    ash_delete_def(ctx, "MonitorPDU",    ASH_DEF_PDU);
    ash_delete_def(ctx, "WheelSpeed",    ASH_DEF_SIGNAL);

    /* ------------------------------------------------------------------ */
    /* Define signal: WheelSpeed — 16-bit unsigned, LE, 0.1 km/h per LSB  */
    /* ------------------------------------------------------------------ */
    ash_signal_def_t sig = {
        .name       = "WheelSpeed",
        .data_type  = ASH_DATA_TYPE_UINT,
        .byte_order = ASH_BYTE_ORDER_LE,
        .bit_length = 16,
        .scale      = 0.1,
        .offset     = 0.0,
        .min        = 0.0,
        .max        = 6553.5,
    };
    if (ash_define_signal(ctx, &sig) < 0) {
        fprintf(stderr, "ash_define_signal failed\n");
        ash_disconnect(ctx);
        return 1;
    }

    ash_pdu_def_t pdu = {
        .name         = "MonitorPDU",
        .length       = 8,
        .signal_count = 1,
        .signals      = { { .signal_name = "WheelSpeed", .start_bit = 0 } },
    };
    if (ash_define_pdu(ctx, &pdu) < 0) {
        fprintf(stderr, "ash_define_pdu failed\n");
        ash_disconnect(ctx);
        return 1;
    }

    /* Cyclic frame — server will retransmit at TX_PERIOD_MS automatically */
    ash_frame_def_t frame = {
        .name         = "MonitorFrame",
        .can_id       = 0x200,
        .id_type      = ASH_ID_TYPE_STD,
        .dlc          = 8,
        .tx_period_ms = TX_PERIOD_MS,
        .pdu_count    = 1,
        .pdus         = { { .pdu_name = "MonitorPDU", .byte_offset = 0 } },
    };
    if (ash_define_frame(ctx, &frame) < 0) {
        fprintf(stderr, "ash_define_frame failed\n");
        ash_disconnect(ctx);
        return 1;
    }

    /* ------------------------------------------------------------------ */
    /* Attach to interface                                                  */
    /* ------------------------------------------------------------------ */
    if (ash_iface_attach(ctx, iface, ASH_MODE_CAN20B, 0) < 0) {
        fprintf(stderr, "ash_iface_attach failed\n");
        ash_disconnect(ctx);
        return 1;
    }
    printf("Attached to %s\n", iface);

    /* ------------------------------------------------------------------ */
    /* Acquire ownership and seed an initial value (starts cyclic TX)      */
    /* ------------------------------------------------------------------ */
    if (ash_acquire(ctx, "WheelSpeed", ASH_ON_DISCONNECT_STOP) < 0) {
        fprintf(stderr, "ash_acquire failed\n");
        ash_disconnect(ctx);
        return 1;
    }
    if (ash_write(ctx, "WheelSpeed", 80.0) < 0) {
        fprintf(stderr, "ash_write failed\n");
        ash_disconnect(ctx);
        return 1;
    }
    printf("Monitoring WheelSpeed (cyclic %d ms) for %d s — press Ctrl+C to stop\n",
           TX_PERIOD_MS, MONITOR_DURATION_S);

    /* ------------------------------------------------------------------ */
    /* Event loop                                                           */
    /* ------------------------------------------------------------------ */
    int events = 0;
    int deadline_ms = MONITOR_DURATION_S * 1000;

    while (!g_stop && deadline_ms > 0) {
        ash_event_t ev;
        int ret = ash_poll(ctx, &ev, 200);
        if (ret < 0) {
            fprintf(stderr, "ash_poll error\n");
            break;
        }
        deadline_ms -= 200;

        if (ret == 0)
            continue;  /* timeout slice, keep looping */

        if (ev.type == ASH_EVENT_SIG_RX) {
            printf("  SIG_RX  %-20s = %.1f km/h\n",
                   ev.u.sig_rx.signal, ev.u.sig_rx.value);
            events++;
        } else if (ev.type == ASH_EVENT_NOTIFY_IFACE_DOWN) {
            printf("  IFACE_DOWN: %s\n", ev.u.iface_down.iface);
            break;
        } else if (ev.type == ASH_EVENT_NOTIFY_SERVER_CLOSE) {
            printf("  SERVER_CLOSE\n");
            break;
        }
    }

    printf("Received %d SIG_RX event(s)\n", events);

    /* ------------------------------------------------------------------ */
    /* Clean up                                                             */
    /* ------------------------------------------------------------------ */
    ash_release(ctx, "WheelSpeed");
    ash_iface_detach(ctx, iface);
    ash_disconnect(ctx);
    return 0;
}
