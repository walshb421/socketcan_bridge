#include "cfg.h"
#include "def.h"
#include "proto.h"

#include "ash/proto.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dirent.h>
#include <limits.h>
#include <errno.h>

/* -------------------------------------------------------------------------
 * CRC32  (IEEE 802.3, polynomial 0xEDB88320)
 * ---------------------------------------------------------------------- */

static uint32_t g_crc_table[256];
static int      g_crc_table_ready = 0;

static void crc32_init(void)
{
    for (uint32_t i = 0; i < 256u; i++) {
        uint32_t c = i;
        for (int j = 0; j < 8; j++)
            c = (c & 1u) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
        g_crc_table[i] = c;
    }
    g_crc_table_ready = 1;
}

static uint32_t crc32_compute(const uint8_t *data, size_t len)
{
    if (!g_crc_table_ready)
        crc32_init();
    uint32_t crc = 0xFFFFFFFFu;
    for (size_t i = 0; i < len; i++)
        crc = g_crc_table[(crc ^ data[i]) & 0xFFu] ^ (crc >> 8);
    return crc ^ 0xFFFFFFFFu;
}

/* -------------------------------------------------------------------------
 * File format constants  (SPEC §9.2)
 * ---------------------------------------------------------------------- */

#define CFG_MAGIC        0x41534843u   /* "ASHC" */
#define CFG_VERSION      0x0001u
#define CFG_HEADER_SIZE  12u           /* magic(4)+version(2)+count(2)+crc32(4) */
#define CFG_MAX_BUF      (1u << 20)    /* 1 MiB — generous upper bound */

/* -------------------------------------------------------------------------
 * Module state
 * ---------------------------------------------------------------------- */

static const char *g_storage_dir = NULL;

/* -------------------------------------------------------------------------
 * Path helper
 * ---------------------------------------------------------------------- */

static int build_path(char *out, size_t outsz, const char *name)
{
    if (strchr(name, '/') || strchr(name, '\\'))
        return -1;
    int r = snprintf(out, outsz, "%s/%s.ashcfg", g_storage_dir, name);
    if (r < 0 || (size_t)r >= outsz)
        return -1;
    return 0;
}

/* -------------------------------------------------------------------------
 * load_file — internal: read, validate, conflict-check, apply
 * ---------------------------------------------------------------------- */

static int load_file(const char *path, uint16_t *err_code)
{
    FILE *f = fopen(path, "rb");
    if (!f) {
        *err_code = ERR_CFG_IO;
        return -1;
    }

    if (fseek(f, 0, SEEK_END) != 0) {
        fclose(f);
        *err_code = ERR_CFG_IO;
        return -1;
    }
    long fsize = ftell(f);
    rewind(f);

    if (fsize < (long)CFG_HEADER_SIZE || fsize > (long)CFG_MAX_BUF) {
        fclose(f);
        *err_code = ERR_CFG_IO;
        return -1;
    }

    uint8_t *buf = malloc((size_t)fsize);
    if (!buf) {
        fclose(f);
        *err_code = ERR_CFG_IO;
        return -1;
    }

    if (fread(buf, 1, (size_t)fsize, f) != (size_t)fsize) {
        free(buf);
        fclose(f);
        *err_code = ERR_CFG_IO;
        return -1;
    }
    fclose(f);

    /* Parse header */
    uint32_t magic   = ((uint32_t)buf[0] << 24) | ((uint32_t)buf[1] << 16)
                     | ((uint32_t)buf[2] <<  8) |  (uint32_t)buf[3];
    uint16_t version = ((uint16_t)buf[4] << 8) | buf[5];
    uint16_t count   = ((uint16_t)buf[6] << 8) | buf[7];
    uint32_t stored_crc = ((uint32_t)buf[8]  << 24) | ((uint32_t)buf[9]  << 16)
                        | ((uint32_t)buf[10] <<  8) |  (uint32_t)buf[11];

    if (magic != CFG_MAGIC || version != CFG_VERSION) {
        free(buf);
        *err_code = ERR_CFG_IO;
        return -1;
    }

    /* CRC covers all bytes after the CRC field (offset 12 onward) */
    uint32_t calc_crc = crc32_compute(buf + CFG_HEADER_SIZE,
                                      (size_t)fsize - CFG_HEADER_SIZE);
    if (calc_crc != stored_crc) {
        free(buf);
        *err_code = ERR_CFG_CHECKSUM;
        return -1;
    }

    /* Phase 1: conflict-check every entry */
    size_t pos = CFG_HEADER_SIZE;
    for (uint16_t i = 0; i < count; i++) {
        if (pos + 3u > (size_t)fsize) {
            free(buf);
            *err_code = ERR_CFG_IO;
            return -1;
        }
        uint8_t  entry_type = buf[pos];
        uint16_t entry_len  = ((uint16_t)buf[pos+1] << 8) | buf[pos+2];
        pos += 3;

        if (pos + entry_len > (size_t)fsize) {
            free(buf);
            *err_code = ERR_CFG_IO;
            return -1;
        }

        int rc = def_validate_entry(entry_type, buf + pos, entry_len);
        if (rc == DEF_APPLY_CONFLICT) {
            free(buf);
            *err_code = ERR_CFG_CONFLICT;
            return -1;
        }
        if (rc == DEF_APPLY_INVALID) {
            free(buf);
            *err_code = ERR_CFG_IO;
            return -1;
        }

        pos += entry_len;
    }

    /* Phase 2: apply all entries */
    pos = CFG_HEADER_SIZE;
    for (uint16_t i = 0; i < count; i++) {
        uint8_t  entry_type = buf[pos];
        uint16_t entry_len  = ((uint16_t)buf[pos+1] << 8) | buf[pos+2];
        pos += 3;
        def_apply_entry(entry_type, buf + pos, entry_len);
        pos += entry_len;
    }

    free(buf);
    return 0;
}

/* -------------------------------------------------------------------------
 * CFG_SAVE handler  (SPEC §9.3)
 * ---------------------------------------------------------------------- */

static int handle_cfg_save(int fd, const proto_frame_t *frame)
{
    if (!g_storage_dir) {
        proto_send_err(fd, ERR_CFG_IO, NULL);
        return 0;
    }

    if (frame->hdr.payload_len < 2u) {
        proto_send_err(fd, ERR_CFG_IO, NULL);
        return 0;
    }

    uint8_t name_len = frame->payload[0];
    if (name_len == 0 || name_len > PROTO_MAX_NAME ||
        frame->hdr.payload_len < (uint32_t)(1u + name_len)) {
        proto_send_err(fd, ERR_CFG_IO, NULL);
        return 0;
    }

    char name[PROTO_MAX_NAME + 1];
    memcpy(name, frame->payload + 1, name_len);
    name[name_len] = '\0';

    char path[PATH_MAX];
    if (build_path(path, sizeof(path), name) < 0) {
        proto_send_err(fd, ERR_CFG_IO, NULL);
        return 0;
    }

    /* Serialize definitions into a heap buffer (header + body) */
    uint8_t *buf = malloc(CFG_MAX_BUF);
    if (!buf) {
        proto_send_err(fd, ERR_CFG_IO, NULL);
        return 0;
    }

    uint16_t entry_count = 0;
    ssize_t  body_len    = def_serialize_entries(buf + CFG_HEADER_SIZE,
                                                 CFG_MAX_BUF - CFG_HEADER_SIZE,
                                                 &entry_count);
    if (body_len < 0) {
        free(buf);
        proto_send_err(fd, ERR_CFG_IO, NULL);
        return 0;
    }

    /* CRC32 of the body */
    uint32_t crc = crc32_compute(buf + CFG_HEADER_SIZE, (size_t)body_len);

    /* Write file header */
    buf[0]  = (uint8_t)((CFG_MAGIC >> 24) & 0xFF);
    buf[1]  = (uint8_t)((CFG_MAGIC >> 16) & 0xFF);
    buf[2]  = (uint8_t)((CFG_MAGIC >>  8) & 0xFF);
    buf[3]  = (uint8_t)( CFG_MAGIC        & 0xFF);
    buf[4]  = (uint8_t)((CFG_VERSION >> 8) & 0xFF);
    buf[5]  = (uint8_t)( CFG_VERSION       & 0xFF);
    buf[6]  = (uint8_t)((entry_count >> 8) & 0xFF);
    buf[7]  = (uint8_t)( entry_count       & 0xFF);
    buf[8]  = (uint8_t)((crc >> 24) & 0xFF);
    buf[9]  = (uint8_t)((crc >> 16) & 0xFF);
    buf[10] = (uint8_t)((crc >>  8) & 0xFF);
    buf[11] = (uint8_t)( crc        & 0xFF);

    FILE *outf = fopen(path, "wb");
    if (!outf) {
        free(buf);
        proto_send_err(fd, ERR_CFG_IO, NULL);
        return 0;
    }

    size_t total = CFG_HEADER_SIZE + (size_t)body_len;
    if (fwrite(buf, 1, total, outf) != total) {
        fclose(outf);
        free(buf);
        proto_send_err(fd, ERR_CFG_IO, NULL);
        return 0;
    }

    fclose(outf);
    free(buf);

    return proto_send_ack(fd, MSG_CFG_ACK, NULL, 0);
}

/* -------------------------------------------------------------------------
 * CFG_LOAD handler  (SPEC §9.4)
 * ---------------------------------------------------------------------- */

static int handle_cfg_load(int fd, const proto_frame_t *frame)
{
    if (!g_storage_dir) {
        proto_send_err(fd, ERR_CFG_IO, NULL);
        return 0;
    }

    if (frame->hdr.payload_len < 2u) {
        proto_send_err(fd, ERR_CFG_IO, NULL);
        return 0;
    }

    uint8_t name_len = frame->payload[0];
    if (name_len == 0 || name_len > PROTO_MAX_NAME ||
        frame->hdr.payload_len < (uint32_t)(1u + name_len)) {
        proto_send_err(fd, ERR_CFG_IO, NULL);
        return 0;
    }

    char name[PROTO_MAX_NAME + 1];
    memcpy(name, frame->payload + 1, name_len);
    name[name_len] = '\0';

    char path[PATH_MAX];
    if (build_path(path, sizeof(path), name) < 0) {
        proto_send_err(fd, ERR_CFG_IO, NULL);
        return 0;
    }

    uint16_t err_code = ERR_CFG_IO;
    if (load_file(path, &err_code) < 0) {
        proto_send_err(fd, err_code, NULL);
        return 0;
    }

    return proto_send_ack(fd, MSG_CFG_ACK, NULL, 0);
}

/* -------------------------------------------------------------------------
 * Module init / registration / autoload
 * ---------------------------------------------------------------------- */

void cfg_init(const char *storage_dir)
{
    g_storage_dir = storage_dir;
}

void cfg_register_handlers(void)
{
    proto_register_handler(MSG_CFG_SAVE, handle_cfg_save);
    proto_register_handler(MSG_CFG_LOAD, handle_cfg_load);
}

void cfg_autoload(void)
{
    if (!g_storage_dir)
        return;

    DIR *dir = opendir(g_storage_dir);
    if (!dir) {
        if (errno != ENOENT)
            fprintf(stderr,
                    "ash-server: cfg_autoload: cannot open storage dir '%s': %s\n",
                    g_storage_dir, strerror(errno));
        return;
    }

    struct dirent *ent;
    while ((ent = readdir(dir)) != NULL) {
        const char *dname = ent->d_name;
        size_t      dlen  = strlen(dname);

        /* Must end with ".ashcfg" (7 chars) and have at least one char before */
        if (dlen < 8 || strcmp(dname + dlen - 7, ".ashcfg") != 0)
            continue;

        char path[PATH_MAX];
        int r = snprintf(path, sizeof(path), "%s/%s", g_storage_dir, dname);
        if (r < 0 || (size_t)r >= sizeof(path))
            continue;

        uint16_t err_code = ERR_CFG_IO;
        if (load_file(path, &err_code) < 0) {
            fprintf(stderr,
                    "ash-server: cfg_autoload: failed to load '%s' (err=0x%04X)\n",
                    path, err_code);
        } else {
            printf("ash-server: cfg_autoload: loaded '%s'\n", path);
        }
    }

    closedir(dir);
}
