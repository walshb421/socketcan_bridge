/* =============================================================================
 * examples/vcan_loopback.c — libash vcan loopback integration test
 *
 * Usage: ash-example-loopback [host] [port]
 *        Default: host=127.0.0.1, port=4000
 *
 * Demonstrates: self-contained integration test using two sequential client
 * sessions.  The writer session acquires a signal and writes a value;
 * the server encodes it into a CAN frame and transmits on vcan0 — the vcan
 * driver loops the frame back to the server, which decodes it and stores the
 * updated value.  The reader session then attaches and reads the value back,
 * confirming the full TX → vcan loopback → RX → decode round-trip.
 *
 * Note: the server enforces exclusive interface attachment, so the writer
 * detaches vcan0 before the reader attaches.
 * ============================================================================= */

#include "ash/ash.h"

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <math.h>

#define IFACE     "vcan0"
#define SIGNAL    "BattVolt"
#define PDU_NAME  "BattPDU"
#define FRAME_NAME "BattFrame"
#define CAN_ID    0x400u
#define TEST_VALUE 12.8  /* volts */

int main(int argc, char *argv[])
{
    const char *host = (argc > 1) ? argv[1] : "127.0.0.1";
    uint16_t    port = (argc > 2) ? (uint16_t)atoi(argv[2]) : 4000;

    /* ------------------------------------------------------------------ */
    /* Writer session                                                       */
    /* ------------------------------------------------------------------ */
    printf("=== Writer session ===\n");
    ash_ctx_t *writer = ash_connect(host, port, "loopback-writer");
    if (!writer) {
        fprintf(stderr, "ash_connect (writer) failed\n");
        return 1;
    }

    ash_vcan_create(writer, IFACE);  /* ignore error if vcan0 already exists */

    /* Clean up leftover defs from a prior run */
    ash_delete_def(writer, FRAME_NAME, ASH_DEF_FRAME);
    ash_delete_def(writer, PDU_NAME,   ASH_DEF_PDU);
    ash_delete_def(writer, SIGNAL,     ASH_DEF_SIGNAL);

    /* BattVolt: 16-bit unsigned LE, 0.001 V/LSB, range 0–65.535 V */
    ash_signal_def_t sig = {
        .name       = SIGNAL,
        .data_type  = ASH_DATA_TYPE_UINT,
        .byte_order = ASH_BYTE_ORDER_LE,
        .bit_length = 16,
        .scale      = 0.001,
        .offset     = 0.0,
        .min        = 0.0,
        .max        = 65.535,
    };
    if (ash_define_signal(writer, &sig) < 0) {
        fprintf(stderr, "ash_define_signal failed\n");
        ash_disconnect(writer);
        return 1;
    }

    ash_pdu_def_t pdu = {
        .name         = PDU_NAME,
        .length       = 8,
        .signal_count = 1,
        .signals      = { { .signal_name = SIGNAL, .start_bit = 0 } },
    };
    if (ash_define_pdu(writer, &pdu) < 0) {
        fprintf(stderr, "ash_define_pdu failed\n");
        ash_disconnect(writer);
        return 1;
    }

    ash_frame_def_t frame = {
        .name         = FRAME_NAME,
        .can_id       = CAN_ID,
        .id_type      = ASH_ID_TYPE_STD,
        .dlc          = 8,
        .tx_period_ms = 0,  /* event-driven */
        .pdu_count    = 1,
        .pdus         = { { .pdu_name = PDU_NAME, .byte_offset = 0 } },
    };
    if (ash_define_frame(writer, &frame) < 0) {
        fprintf(stderr, "ash_define_frame failed\n");
        ash_disconnect(writer);
        return 1;
    }

    if (ash_iface_attach(writer, IFACE, ASH_MODE_CAN20B, 0) < 0) {
        fprintf(stderr, "ash_iface_attach (writer) failed\n");
        ash_disconnect(writer);
        return 1;
    }
    if (ash_acquire(writer, SIGNAL, ASH_ON_DISCONNECT_LAST) < 0) {
        fprintf(stderr, "ash_acquire failed\n");
        ash_disconnect(writer);
        return 1;
    }

    if (ash_write(writer, SIGNAL, TEST_VALUE) < 0) {
        fprintf(stderr, "ash_write failed\n");
        ash_disconnect(writer);
        return 1;
    }
    printf("  Wrote  %-12s = %.3f V\n", SIGNAL, TEST_VALUE);

    /* Detach so the reader session can attach the same interface */
    ash_release(writer, SIGNAL);
    ash_iface_detach(writer, IFACE);
    ash_disconnect(writer);
    printf("  Writer disconnected\n");

    /* ------------------------------------------------------------------ */
    /* Reader session                                                       */
    /* ------------------------------------------------------------------ */
    printf("=== Reader session ===\n");
    ash_ctx_t *reader = ash_connect(host, port, "loopback-reader");
    if (!reader) {
        fprintf(stderr, "ash_connect (reader) failed\n");
        return 1;
    }

    if (ash_iface_attach(reader, IFACE, ASH_MODE_CAN20B, 0) < 0) {
        fprintf(stderr, "ash_iface_attach (reader) failed\n");
        ash_disconnect(reader);
        return 1;
    }

    double read_val = 0.0;
    if (ash_read(reader, SIGNAL, &read_val) < 0) {
        fprintf(stderr, "ash_read failed\n");
        ash_iface_detach(reader, IFACE);
        ash_disconnect(reader);
        return 1;
    }
    printf("  Read   %-12s = %.3f V\n", SIGNAL, read_val);

    ash_iface_detach(reader, IFACE);
    ash_disconnect(reader);

    /* ------------------------------------------------------------------ */
    /* Verify                                                               */
    /* ------------------------------------------------------------------ */
    double tolerance = sig.scale;  /* one LSB */
    if (fabs(read_val - TEST_VALUE) <= tolerance) {
        printf("PASS  (|%.3f - %.3f| <= %.3f)\n",
               read_val, TEST_VALUE, tolerance);
        return 0;
    } else {
        fprintf(stderr, "FAIL  (|%.3f - %.3f| > %.3f)\n",
                read_val, TEST_VALUE, tolerance);
        return 1;
    }
}
