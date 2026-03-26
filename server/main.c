#include "server.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

int main(int argc, char *argv[])
{
    uint16_t    port        = DEFAULT_PORT;
    const char *storage_dir = NULL;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--port") == 0 && i + 1 < argc) {
            port = (uint16_t)atoi(argv[++i]);
        } else if (strcmp(argv[i], "--storage-dir") == 0 && i + 1 < argc) {
            storage_dir = argv[++i];
        } else {
            fprintf(stderr, "Usage: %s [--port PORT] [--storage-dir DIR]\n",
                    argv[0]);
            return 1;
        }
    }

    server_t s;
    if (server_init(&s, port, storage_dir) < 0)
        return 1;

    server_run(&s);
    server_destroy(&s);
    return 0;
}
