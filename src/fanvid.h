/* fanvid: video wallpapers for fanshell-qs. See README.md. */
#ifndef FANVID_H
#define FANVID_H

#include <stdbool.h>
#include <stdint.h>

/* The version of the command line, stdin and stdout protocol, which
 * `fanvid --protocol` prints; fanshell-qs takes no other. */
#define FANVID_PROTOCOL 1

/* Exit statuses. */
enum {
    EXIT_OK = 0,
    EXIT_ERROR = 1,
    EXIT_USAGE = 2,
    /* The GPU cannot decode this video: the caller shows its still. */
    EXIT_NO_GPU = 3,
};

#define MAX_OUTPUTS 16

/* still.c: writes `path`'s first frame to `out` as PNG. An exit status. */
int still_main(const char *path, const char *out);

/* play.c: plays `path` on the outputs in `specs` ("name:mode"). An exit
 * status. */
int play_main(const char *path, int n_specs, char **specs);

#endif
