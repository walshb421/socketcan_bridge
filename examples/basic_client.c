/* =========================================================================
 * examples/basic_client.c — libash basic usage example
 *
 * Demonstrates: connect → define signal → acquire ownership → write → read
 * ========================================================================= */

#include "ash/ash.h"

#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>

int main(void)
{
    /* Connect to the ash server */
    ash_ctx_t *ctx = ash_connect("127.0.0.1", 4000, "basic-client");
    if (!ctx) {
        fprintf(stderr, "ash_connect failed\n");
        return 1;
    }
    printf("Connected (session established)\n");

    /* Create a virtual CAN interface */
    if (ash_vcan_create(ctx, "vcan0") < 0) {
        /* vcan0 may already exist — ignore error */
    }

    /*
     * Clean up any leftover definitions from a prior run.
     * Definitions persist across sessions, so delete frame → PDU → signal
     * before re-defining them.  Errors here are expected (nothing to delete
     * on the first run) so they are silently ignored.
     */
    ash_delete_def(ctx, "EngineFrame", ASH_DEF_FRAME);
    ash_delete_def(ctx, "EnginePDU",   ASH_DEF_PDU);
    ash_delete_def(ctx, "EngineRPM",   ASH_DEF_SIGNAL);

    /* Define a signal */
    ash_signal_def_t sig = {
        .name       = "EngineRPM",
        .data_type  = ASH_DATA_TYPE_UINT,
        .byte_order = ASH_BYTE_ORDER_LE,
        .bit_length = 16,
        .scale      = 0.25,
        .offset     = 0.0,
        .min        = 0.0,
        .max        = 16383.75,
    };

    if (ash_define_signal(ctx, &sig) < 0) {
        fprintf(stderr, "ash_define_signal failed\n");
        ash_disconnect(ctx);
        return 1;
    }
    printf("Signal 'EngineRPM' defined\n");

    /* Define a PDU that contains the signal */
    ash_pdu_def_t pdu = {
        .name         = "EnginePDU",
        .length       = 8,
        .signal_count = 1,
        .signals      = {
            { .signal_name = "EngineRPM", .start_bit = 0 },
        },
    };

    if (ash_define_pdu(ctx, &pdu) < 0) {
        fprintf(stderr, "ash_define_pdu failed\n");
        ash_disconnect(ctx);
        return 1;
    }
    printf("PDU 'EnginePDU' defined\n");

    /* Define an event-driven frame that carries the PDU */
    ash_frame_def_t frame = {
        .name         = "EngineFrame",
        .can_id       = 0x100,
        .id_type      = ASH_ID_TYPE_STD,
        .dlc          = 8,
        .tx_period_ms = 0,  /* event-driven */
        .pdu_count    = 1,
        .pdus         = {
            { .pdu_name = "EnginePDU", .byte_offset = 0 },
        },
    };

    if (ash_define_frame(ctx, &frame) < 0) {
        fprintf(stderr, "ash_define_frame failed\n");
        ash_disconnect(ctx);
        return 1;
    }
    printf("Frame 'EngineFrame' defined\n");

    /* Attach to the virtual CAN interface.
     * Use bitrate=0 for vcan: the interface has no real hardware to configure
     * and requesting bitrate 0 skips iface_set_bitrate on the server. */
    if (ash_iface_attach(ctx, "vcan0", ASH_MODE_CAN20B, 0) < 0) {
        fprintf(stderr, "ash_iface_attach failed\n");
        ash_disconnect(ctx);
        return 1;
    }
    printf("Interface 'vcan0' attached\n");

    /* Acquire ownership of the signal */
    if (ash_acquire(ctx, "EngineRPM", ASH_ON_DISCONNECT_STOP) < 0) {
        fprintf(stderr, "ash_acquire failed\n");
        ash_disconnect(ctx);
        return 1;
    }
    printf("Ownership of 'EngineRPM' acquired\n");

    /* Write a value */
    double write_val = 3000.0;
    if (ash_write(ctx, "EngineRPM", write_val) < 0) {
        fprintf(stderr, "ash_write failed\n");
        ash_disconnect(ctx);
        return 1;
    }
    printf("Wrote EngineRPM = %.2f RPM\n", write_val);

    /* Read the value back */
    double read_val = 0.0;
    if (ash_read(ctx, "EngineRPM", &read_val) < 0) {
        fprintf(stderr, "ash_read failed\n");
        ash_disconnect(ctx);
        return 1;
    }
    printf("Read  EngineRPM = %.2f RPM\n", read_val);

    /* Clean up */
    ash_release(ctx, "EngineRPM");
    ash_iface_detach(ctx, "vcan0");
    ash_disconnect(ctx);

    printf("Disconnected\n");
    return 0;
}
