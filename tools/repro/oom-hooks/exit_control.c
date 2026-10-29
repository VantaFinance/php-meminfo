/* Control executable for the preload probe; no PHP is involved. */
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

int main(int argc, char **argv)
{
    int status = argc > 1 ? atoi(argv[1]) : 37;

    if (argc > 2 && strcmp(argv[2], "return") == 0) {
        return status;
    }
    _exit(status);
}
