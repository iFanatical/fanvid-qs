/* fanvid --protocol
 * fanvid still <file> <out.png>
 * fanvid play <file> <output>:<mode>...
 *
 * See README.md for the protocol. */
#include <stdio.h>
#include <string.h>

#include "fanvid.h"

static int usage(void)
{
    fprintf(stderr, "usage: fanvid --protocol\n"
                    "       fanvid still <file> <out.png>\n"
                    "       fanvid play <file> <output>:<mode>...\n");
    return EXIT_USAGE;
}

int main(int argc, char **argv)
{
    if (argc == 2 && !strcmp(argv[1], "--protocol")) {
        printf("%d\n", FANVID_PROTOCOL);
        return EXIT_OK;
    }
    if (argc == 4 && !strcmp(argv[1], "still"))
        return still_main(argv[2], argv[3]);
    if (argc >= 4 && !strcmp(argv[1], "play"))
        return play_main(argv[2], argc - 3, argv + 3);
    return usage();
}
