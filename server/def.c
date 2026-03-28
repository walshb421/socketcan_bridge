#include "def.h"
#include "proto.h"

#include "ash/proto.h"

#include <stdint.h>
#include <string.h>
#include <stdio.h>

/* -------------------------------------------------------------------------
 * Store limits
 * ---------------------------------------------------------------------- */

#define MAX_SIGNALS  512
#define MAX_PDUS     256
#define MAX_FRAMES   128

#define MAX_SIGNALS_PER_PDU   32
#define MAX_PDUS_PER_FRAME     8

/* -------------------------------------------------------------------------
 * In-memory definition types
 * ---------------------------------------------------------------------- */

typedef struct {
    char    sig_name[PROTO_MAX_NAME + 1];
    uint8_t start_bit;
} pdu_signal_mapping_t;

typedef struct {
    char    pdu_name[PROTO_MAX_NAME + 1];
    uint8_t byte_offset;
} frame_pdu_mapping_t;

typedef struct {
    char     name[PROTO_MAX_NAME + 1];
    uint8_t  data_type;
    uint8_t  byte_order;
    uint8_t  bit_length;
    double   scale;
    double   offset_val;
    double   min_val;
    double   max_val;
} signal_def_t;

typedef struct {
    char                 name[PROTO_MAX_NAME + 1];
    uint8_t              length;
    uint8_t              signal_count;
    pdu_signal_mapping_t signals[MAX_SIGNALS_PER_PDU];
} pdu_def_t;

typedef struct {
    char                name[PROTO_MAX_NAME + 1];
    uint32_t            can_id;
    uint8_t             id_type;
    uint8_t             dlc;
    uint16_t            tx_period;
    uint8_t             pdu_count;
    frame_pdu_mapping_t pdus[MAX_PDUS_PER_FRAME];
} frame_def_t;

/* -------------------------------------------------------------------------
 * Global stores
 * ---------------------------------------------------------------------- */

static signal_def_t g_signals[MAX_SIGNALS];
static int          g_signal_count;

static pdu_def_t    g_pdus[MAX_PDUS];
static int          g_pdu_count;

static frame_def_t  g_frames[MAX_FRAMES];
static int          g_frame_count;

/* -------------------------------------------------------------------------
 * Def type identifiers  (wire values per SPEC §6.4)
 * ---------------------------------------------------------------------- */

#define DEF_TYPE_SIGNAL  0x01u
#define DEF_TYPE_PDU     0x02u
#define DEF_TYPE_FRAME   0x03u

/* -------------------------------------------------------------------------
 * Lookup helpers
 * ---------------------------------------------------------------------- */

static signal_def_t *signal_find(const char *name)
{
    for (int i = 0; i < g_signal_count; i++) {
        if (strcmp(g_signals[i].name, name) == 0)
            return &g_signals[i];
    }
    return NULL;
}

static pdu_def_t *pdu_find(const char *name)
{
    for (int i = 0; i < g_pdu_count; i++) {
        if (strcmp(g_pdus[i].name, name) == 0)
            return &g_pdus[i];
    }
    return NULL;
}

static frame_def_t *frame_find(const char *name)
{
    for (int i = 0; i < g_frame_count; i++) {
        if (strcmp(g_frames[i].name, name) == 0)
            return &g_frames[i];
    }
    return NULL;
}

/*
 * Returns the wire DEF_TYPE_* constant if `name` exists in any table,
 * or 0 if the name is not defined.
 */
static uint8_t namespace_type(const char *name)
{
    if (signal_find(name)) return DEF_TYPE_SIGNAL;
    if (pdu_find(name))    return DEF_TYPE_PDU;
    if (frame_find(name))  return DEF_TYPE_FRAME;
    return 0;
}

/* Returns 1 if any PDU references `sig_name`. */
static int signal_has_dependents(const char *sig_name)
{
    for (int i = 0; i < g_pdu_count; i++) {
        for (int j = 0; j < (int)g_pdus[i].signal_count; j++) {
            if (strcmp(g_pdus[i].signals[j].sig_name, sig_name) == 0)
                return 1;
        }
    }
    return 0;
}

/* Returns 1 if any frame references `pdu_name`. */
static int pdu_has_dependents(const char *pdu_name)
{
    for (int i = 0; i < g_frame_count; i++) {
        for (int j = 0; j < (int)g_frames[i].pdu_count; j++) {
            if (strcmp(g_frames[i].pdus[j].pdu_name, pdu_name) == 0)
                return 1;
        }
    }
    return 0;
}

/* -------------------------------------------------------------------------
 * Wire read helper for IEEE 754 doubles stored big-endian
 * ---------------------------------------------------------------------- */

static double read_be_double(const uint8_t *p)
{
    uint64_t raw = ((uint64_t)p[0] << 56) | ((uint64_t)p[1] << 48)
                 | ((uint64_t)p[2] << 40) | ((uint64_t)p[3] << 32)
                 | ((uint64_t)p[4] << 24) | ((uint64_t)p[5] << 16)
                 | ((uint64_t)p[6] <<  8) |  (uint64_t)p[7];
    double d;
    memcpy(&d, &raw, sizeof(d));
    return d;
}

/* -------------------------------------------------------------------------
 * DEF_SIGNAL handler  (SPEC §6.1)
 *
 * Payload layout:
 *   1B  name_len
 *   NB  name
 *   1B  data_type  (0x01=uint, 0x02=sint, 0x03=float)
 *   1B  byte_order (0x01=little, 0x02=big)
 *   1B  bit_length
 *   8B  scale   (IEEE 754 double, big-endian)
 *   8B  offset  (IEEE 754 double, big-endian)
 *   8B  min     (IEEE 754 double, big-endian)
 *   8B  max     (IEEE 754 double, big-endian)
 * ---------------------------------------------------------------------- */

static int handle_def_signal(int fd, const proto_frame_t *frame)
{
    /* Minimum: name_len(1) + name(≥1) + data_type(1) + byte_order(1) +
     *          bit_length(1) + scale(8) + offset(8) + min(8) + max(8) = 37 */
    if (frame->hdr.payload_len < 37u) {
        proto_send_err(fd, ERR_DEF_INVALID, NULL);
        return 0;
    }

    const uint8_t *p         = frame->payload;
    uint32_t       remaining = frame->hdr.payload_len;

    uint8_t name_len = *p++; remaining--;
    if (name_len == 0 || name_len > PROTO_MAX_NAME ||
        remaining < (uint32_t)name_len) {
        proto_send_err(fd, ERR_DEF_INVALID, NULL);
        return 0;
    }

    char name[PROTO_MAX_NAME + 1];
    memcpy(name, p, name_len);
    name[name_len] = '\0';
    p += name_len; remaining -= name_len;

    /* Need: data_type(1) + byte_order(1) + bit_length(1) + 4 * 8B doubles */
    if (remaining < 35u) {
        proto_send_err(fd, ERR_DEF_INVALID, NULL);
        return 0;
    }

    uint8_t data_type  = *p++; remaining--;
    uint8_t byte_order = *p++; remaining--;
    uint8_t bit_length = *p++; remaining--;

    if (data_type < 0x01u || data_type > 0x03u ||
        byte_order < 0x01u || byte_order > 0x02u ||
        bit_length == 0) {
        proto_send_err(fd, ERR_DEF_INVALID, NULL);
        return 0;
    }

    double scale      = read_be_double(p); p += 8; remaining -= 8;
    double offset_val = read_be_double(p); p += 8; remaining -= 8;
    double min_val    = read_be_double(p); p += 8; remaining -= 8;
    double max_val    = read_be_double(p); p += 8; remaining -= 8;

    /* Namespace conflict: name exists as a different type */
    uint8_t existing_type = namespace_type(name);
    if (existing_type != 0 && existing_type != DEF_TYPE_SIGNAL) {
        proto_send_err(fd, ERR_DEF_CONFLICT, NULL);
        return 0;
    }

    /* Same-type redefinition: reject if a PDU depends on this signal */
    if (existing_type == DEF_TYPE_SIGNAL && signal_has_dependents(name)) {
        proto_send_err(fd, ERR_DEF_IN_USE, NULL);
        return 0;
    }

    /* Insert or overwrite */
    signal_def_t *slot = signal_find(name);
    if (!slot) {
        if (g_signal_count >= MAX_SIGNALS) {
            proto_send_err(fd, ERR_DEF_INVALID, NULL);
            return 0;
        }
        slot = &g_signals[g_signal_count++];
    }

    memcpy(slot->name, name, name_len + 1);
    slot->data_type  = data_type;
    slot->byte_order = byte_order;
    slot->bit_length = bit_length;
    slot->scale      = scale;
    slot->offset_val = offset_val;
    slot->min_val    = min_val;
    slot->max_val    = max_val;

    return proto_send_ack(fd, MSG_DEF_ACK, NULL, 0);
}

/* -------------------------------------------------------------------------
 * DEF_PDU handler  (SPEC §6.2)
 *
 * Payload layout:
 *   1B  name_len
 *   NB  name
 *   1B  pdu_length  (in bytes)
 *   1B  signal_count  (≤ 32)
 *   for each signal:
 *     1B  sig_name_len
 *     NB  sig_name
 *     1B  start_bit
 * ---------------------------------------------------------------------- */

static int handle_def_pdu(int fd, const proto_frame_t *frame)
{
    /* Minimum: name_len(1) + name(≥1) + pdu_length(1) + signal_count(1) = 4 */
    if (frame->hdr.payload_len < 4u) {
        proto_send_err(fd, ERR_DEF_INVALID, NULL);
        return 0;
    }

    const uint8_t *p         = frame->payload;
    uint32_t       remaining = frame->hdr.payload_len;

    uint8_t name_len = *p++; remaining--;
    if (name_len == 0 || name_len > PROTO_MAX_NAME ||
        remaining < (uint32_t)name_len) {
        proto_send_err(fd, ERR_DEF_INVALID, NULL);
        return 0;
    }

    char name[PROTO_MAX_NAME + 1];
    memcpy(name, p, name_len);
    name[name_len] = '\0';
    p += name_len; remaining -= name_len;

    if (remaining < 2u) {
        proto_send_err(fd, ERR_DEF_INVALID, NULL);
        return 0;
    }

    uint8_t pdu_length    = *p++; remaining--;
    uint8_t signal_count  = *p++; remaining--;

    if (signal_count > MAX_SIGNALS_PER_PDU) {
        proto_send_err(fd, ERR_DEF_INVALID, NULL);
        return 0;
    }

    /* Parse signal mappings, validating references as we go */
    pdu_signal_mapping_t mappings[MAX_SIGNALS_PER_PDU];

    for (int i = 0; i < (int)signal_count; i++) {
        if (remaining < 1u) {
            proto_send_err(fd, ERR_DEF_INVALID, NULL);
            return 0;
        }
        uint8_t sname_len = *p++; remaining--;

        if (sname_len == 0 || sname_len > PROTO_MAX_NAME ||
            remaining < (uint32_t)sname_len + 1u) {
            proto_send_err(fd, ERR_DEF_INVALID, NULL);
            return 0;
        }

        char sname[PROTO_MAX_NAME + 1];
        memcpy(sname, p, sname_len);
        sname[sname_len] = '\0';
        p += sname_len; remaining -= sname_len;

        uint8_t start_bit = *p++; remaining--;

        /* Signal must already be defined */
        if (!signal_find(sname)) {
            proto_send_err(fd, ERR_DEF_INVALID, NULL);
            return 0;
        }

        memcpy(mappings[i].sig_name, sname, sname_len + 1);
        mappings[i].start_bit = start_bit;
    }

    /* Namespace conflict check */
    uint8_t existing_type = namespace_type(name);
    if (existing_type != 0 && existing_type != DEF_TYPE_PDU) {
        proto_send_err(fd, ERR_DEF_CONFLICT, NULL);
        return 0;
    }

    /* Same-type redefinition: reject if a frame depends on this PDU */
    if (existing_type == DEF_TYPE_PDU && pdu_has_dependents(name)) {
        proto_send_err(fd, ERR_DEF_IN_USE, NULL);
        return 0;
    }

    /* Insert or overwrite */
    pdu_def_t *slot = pdu_find(name);
    if (!slot) {
        if (g_pdu_count >= MAX_PDUS) {
            proto_send_err(fd, ERR_DEF_INVALID, NULL);
            return 0;
        }
        slot = &g_pdus[g_pdu_count++];
    }

    memcpy(slot->name, name, name_len + 1);
    slot->length       = pdu_length;
    slot->signal_count = signal_count;
    memcpy(slot->signals, mappings, signal_count * sizeof(pdu_signal_mapping_t));

    return proto_send_ack(fd, MSG_DEF_ACK, NULL, 0);
}

/* -------------------------------------------------------------------------
 * DEF_FRAME handler  (SPEC §6.3)
 *
 * Payload layout:
 *   1B  name_len
 *   NB  name
 *   4B  can_id     (big-endian)
 *   1B  id_type    (0x01=standard, 0x02=extended)
 *   1B  dlc
 *   2B  tx_period  (big-endian, ms; 0=event-driven)
 *   1B  pdu_count  (≤ 8)
 *   for each PDU:
 *     1B  pdu_name_len
 *     NB  pdu_name
 *     1B  byte_offset
 * ---------------------------------------------------------------------- */

static int handle_def_frame(int fd, const proto_frame_t *frame)
{
    /* Minimum: name_len(1) + name(≥1) + can_id(4) + id_type(1) +
     *          dlc(1) + tx_period(2) + pdu_count(1) = 11 */
    if (frame->hdr.payload_len < 11u) {
        proto_send_err(fd, ERR_DEF_INVALID, NULL);
        return 0;
    }

    const uint8_t *p         = frame->payload;
    uint32_t       remaining = frame->hdr.payload_len;

    uint8_t name_len = *p++; remaining--;
    if (name_len == 0 || name_len > PROTO_MAX_NAME ||
        remaining < (uint32_t)name_len) {
        proto_send_err(fd, ERR_DEF_INVALID, NULL);
        return 0;
    }

    char name[PROTO_MAX_NAME + 1];
    memcpy(name, p, name_len);
    name[name_len] = '\0';
    p += name_len; remaining -= name_len;

    /* Need: can_id(4) + id_type(1) + dlc(1) + tx_period(2) + pdu_count(1) */
    if (remaining < 9u) {
        proto_send_err(fd, ERR_DEF_INVALID, NULL);
        return 0;
    }

    uint32_t can_id    = ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16)
                       | ((uint32_t)p[2] <<  8) |  (uint32_t)p[3];
    p += 4; remaining -= 4;

    uint8_t  id_type   = *p++; remaining--;
    uint8_t  dlc       = *p++; remaining--;
    uint16_t tx_period = (uint16_t)(((uint16_t)p[0] << 8) | p[1]);
    p += 2; remaining -= 2;
    uint8_t  pdu_count = *p++; remaining--;

    if ((id_type != 0x01u && id_type != 0x02u) || pdu_count > MAX_PDUS_PER_FRAME) {
        proto_send_err(fd, ERR_DEF_INVALID, NULL);
        return 0;
    }

    /* Parse PDU mappings, validating references as we go */
    frame_pdu_mapping_t mappings[MAX_PDUS_PER_FRAME];

    for (int i = 0; i < (int)pdu_count; i++) {
        if (remaining < 1u) {
            proto_send_err(fd, ERR_DEF_INVALID, NULL);
            return 0;
        }
        uint8_t pname_len = *p++; remaining--;

        if (pname_len == 0 || pname_len > PROTO_MAX_NAME ||
            remaining < (uint32_t)pname_len + 1u) {
            proto_send_err(fd, ERR_DEF_INVALID, NULL);
            return 0;
        }

        char pname[PROTO_MAX_NAME + 1];
        memcpy(pname, p, pname_len);
        pname[pname_len] = '\0';
        p += pname_len; remaining -= pname_len;

        uint8_t byte_offset = *p++; remaining--;

        /* PDU must already be defined */
        if (!pdu_find(pname)) {
            proto_send_err(fd, ERR_DEF_INVALID, NULL);
            return 0;
        }

        memcpy(mappings[i].pdu_name, pname, pname_len + 1);
        mappings[i].byte_offset = byte_offset;
    }

    /* Namespace conflict check */
    uint8_t existing_type = namespace_type(name);
    if (existing_type != 0 && existing_type != DEF_TYPE_FRAME) {
        proto_send_err(fd, ERR_DEF_CONFLICT, NULL);
        return 0;
    }

    /* Insert or overwrite (frames have no dependents, so no DEF_IN_USE check) */
    frame_def_t *slot = frame_find(name);
    if (!slot) {
        if (g_frame_count >= MAX_FRAMES) {
            proto_send_err(fd, ERR_DEF_INVALID, NULL);
            return 0;
        }
        slot = &g_frames[g_frame_count++];
    }

    memcpy(slot->name, name, name_len + 1);
    slot->can_id    = can_id;
    slot->id_type   = id_type;
    slot->dlc       = dlc;
    slot->tx_period = tx_period;
    slot->pdu_count = pdu_count;
    memcpy(slot->pdus, mappings, pdu_count * sizeof(frame_pdu_mapping_t));

    return proto_send_ack(fd, MSG_DEF_ACK, NULL, 0);
}

/* -------------------------------------------------------------------------
 * DEF_DELETE handler  (SPEC §6.4)
 *
 * Payload layout:
 *   1B  name_len
 *   NB  name
 *   1B  def_type  (0x01=signal, 0x02=PDU, 0x03=frame)
 * ---------------------------------------------------------------------- */

static int handle_def_delete(int fd, const proto_frame_t *frame)
{
    /* Minimum: name_len(1) + name(≥1) + def_type(1) = 3 */
    if (frame->hdr.payload_len < 3u) {
        proto_send_err(fd, ERR_DEF_INVALID, NULL);
        return 0;
    }

    const uint8_t *p         = frame->payload;
    uint32_t       remaining = frame->hdr.payload_len;

    uint8_t name_len = *p++; remaining--;
    if (name_len == 0 || name_len > PROTO_MAX_NAME ||
        remaining < (uint32_t)name_len + 1u) {
        proto_send_err(fd, ERR_DEF_INVALID, NULL);
        return 0;
    }

    char name[PROTO_MAX_NAME + 1];
    memcpy(name, p, name_len);
    name[name_len] = '\0';
    p += name_len; remaining -= name_len;

    uint8_t def_type = *p++; remaining--;

    if (def_type < DEF_TYPE_SIGNAL || def_type > DEF_TYPE_FRAME) {
        proto_send_err(fd, ERR_DEF_INVALID, NULL);
        return 0;
    }

    /* Verify the name exists and matches the declared type */
    uint8_t actual_type = namespace_type(name);
    if (actual_type == 0 || actual_type != def_type) {
        proto_send_err(fd, ERR_DEF_INVALID, NULL);
        return 0;
    }

    if (def_type == DEF_TYPE_SIGNAL) {
        if (signal_has_dependents(name)) {
            proto_send_err(fd, ERR_DEF_IN_USE, NULL);
            return 0;
        }
        /* Remove by swapping with the last entry */
        signal_def_t *entry = signal_find(name);
        int idx = (int)(entry - g_signals);
        if (idx < g_signal_count - 1)
            g_signals[idx] = g_signals[g_signal_count - 1];
        g_signal_count--;

    } else if (def_type == DEF_TYPE_PDU) {
        if (pdu_has_dependents(name)) {
            proto_send_err(fd, ERR_DEF_IN_USE, NULL);
            return 0;
        }
        pdu_def_t *entry = pdu_find(name);
        int idx = (int)(entry - g_pdus);
        if (idx < g_pdu_count - 1)
            g_pdus[idx] = g_pdus[g_pdu_count - 1];
        g_pdu_count--;

    } else { /* DEF_TYPE_FRAME */
        frame_def_t *entry = frame_find(name);
        int idx = (int)(entry - g_frames);
        if (idx < g_frame_count - 1)
            g_frames[idx] = g_frames[g_frame_count - 1];
        g_frame_count--;
    }

    return proto_send_ack(fd, MSG_DEF_ACK, NULL, 0);
}

/* -------------------------------------------------------------------------
 * Module lifecycle
 * ---------------------------------------------------------------------- */

int def_signal_exists(const char *name)
{
    return signal_find(name) != NULL;
}

void def_init(void)
{
    g_signal_count = 0;
    g_pdu_count    = 0;
    g_frame_count  = 0;
}

void def_destroy(void)
{
    g_signal_count = 0;
    g_pdu_count    = 0;
    g_frame_count  = 0;
}

void def_register_handlers(void)
{
    proto_register_handler(MSG_DEF_SIGNAL, handle_def_signal);
    proto_register_handler(MSG_DEF_PDU,    handle_def_pdu);
    proto_register_handler(MSG_DEF_FRAME,  handle_def_frame);
    proto_register_handler(MSG_DEF_DELETE, handle_def_delete);
}
