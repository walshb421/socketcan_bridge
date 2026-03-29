/* =============================================================================
 * examples/drive_sequence.c — libash scripted drive sequence example
 *
 * Usage: ash-example-drive [host] [port] [iface]
 *        Default: host=127.0.0.1, port=4000, iface=vcan0
 *
 * Demonstrates: acquire ownership of a signal and drive it through a
 * time-stamped sequence of values, illustrating ash_write and cyclic TX.
 * ============================================================================= */

#include "ash/ash.h"

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <time.h>
#include <string.h>

#define TX_PERIOD_MS  100   /* cyclic transmit period */

static void sleep_ms(int ms)
{
    struct timespec ts = { .tv_sec = ms / 1000, .tv_nsec = (ms % 1000) * 1000000L };
    nanosleep(&ts, NULL);
}

int main(int argc, char *argv[])
{
    const char *host  = (argc > 1) ? argv[1] : "127.0.0.1";
    uint16_t    port  = (argc > 2) ? (uint16_t)atoi(argv[2]) : 4000;
    const char *iface = (argc > 3) ? argv[3] : "vcan0";

    /* ------------------------------------------------------------------ */
    /* Connect                                                              */
    /* ------------------------------------------------------------------ */
    ash_ctx_t *ctx = ash_connect(host, port, "drive-sequence");
    if (!ctx) {
        fprintf(stderr, "ash_connect failed\n");
        return 1;
    }
    printf("Connected to %s:%u\n", host, port);

    ash_vcan_create(ctx, iface);

    /* ------------------------------------------------------------------ */
    /* Clean up any leftover definitions                                   */
    /* ------------------------------------------------------------------ */
    ash_delete_def(ctx, "ThrottleFrame", ASH_DEF_FRAME);
    ash_delete_def(ctx, "ThrottlePDU",   ASH_DEF_PDU);
    ash_delete_def(ctx, "ThrottlePos",   ASH_DEF_SIGNAL);

    /* ------------------------------------------------------------------ */
    /* Define signal: ThrottlePos — 8-bit unsigned, LE, 0.4% per LSB      */
    /* ------------------------------------------------------------------ */
    ash_signal_def_t sig = {
        .name       = "ThrottlePos",
        .data_type  = ASH_DATA_TYPE_UINT,
        .byte_order = ASH_BYTE_ORDER_LE,
        .bit_length = 8,
        .scale      = 0.4,
        .offset     = 0.0,
        .min        = 0.0,
        .max        = 100.0,
    };
    if (ash_define_signal(ctx, &sig) < 0) {
        fprintf(stderr, "ash_define_signal failed\n");
        ash_disconnect(ctx);
        return 1;
    }

    ash_pdu_def_t pdu = {
        .name         = "ThrottlePDU",
        .length       = 8,
        .signal_count = 1,
        .signals      = { { .signal_name = "ThrottlePos", .start_bit = 0 } },
    };
    if (ash_define_pdu(ctx, &pdu) < 0) {
        fprintf(stderr, "ash_define_pdu failed\n");
        ash_disconnect(ctx);
        return 1;
    }

    ash_frame_def_t frame = {
        .name         = "ThrottleFrame",
        .can_id       = 0x300,
        .id_type      = ASH_ID_TYPE_STD,
        .dlc          = 8,
        .tx_period_ms = TX_PERIOD_MS,
        .pdu_count    = 1,
        .pdus         = { { .pdu_name = "ThrottlePDU", .byte_offset = 0 } },
    };
    if (ash_define_frame(ctx, &frame) < 0) {
        fprintf(stderr, "ash_define_frame failed\n");
        ash_disconnect(ctx);
        return 1;
    }

    /* ------------------------------------------------------------------ */
    /* Attach and acquire                                                   */
    /* ------------------------------------------------------------------ */
    if (ash_iface_attach(ctx, iface, ASH_MODE_CAN20B, 0) < 0) {
        fprintf(stderr, "ash_iface_attach failed\n");
        ash_disconnect(ctx);
        return 1;
    }
    if (ash_acquire(ctx, "ThrottlePos", ASH_ON_DISCONNECT_DEFAULT) < 0) {
        fprintf(stderr, "ash_acquire failed\n");
        ash_disconnect(ctx);
        return 1;
    }
    printf("Driving ThrottlePos through sequence (cyclic %d ms)\n", TX_PERIOD_MS);

    /* ------------------------------------------------------------------ */
    /* Drive sequence: ramp up then back to 0                              */
    /* ------------------------------------------------------------------ */
    static const double sequence[] = { 0.0, 25.0, 50.0, 75.0, 100.0,
                                        75.0, 50.0, 25.0, 0.0 };
    static const int    hold_ms[]  = { 200, 500,  500,  500,  1000,
                                        300,  300,  300,  500 };

    for (int i = 0; i < (int)(sizeof(sequence) / sizeof(sequence[0])); i++) {
        if (ash_write(ctx, "ThrottlePos", sequence[i]) < 0) {
            fprintf(stderr, "ash_write failed at step %d\n", i);
            break;
        }
        printf("  ThrottlePos → %5.1f %%  (hold %d ms)\n",
               sequence[i], hold_ms[i]);
        sleep_ms(hold_ms[i]);
    }

    printf("Sequence complete\n");

    /* ------------------------------------------------------------------ */
    /* Clean up                                                             */
    /* ------------------------------------------------------------------ */
    ash_release(ctx, "ThrottlePos");
    ash_iface_detach(ctx, iface);
    ash_disconnect(ctx);
    return 0;
}
