/* in case terminal mount failed to mount None(in-memory) deve */

#include <sys/mount.h>
#include <stdio.h>
#include <errno.h>
#include <string.h>

int main(void)
{
    if (mount("none", "/mnt/nomfs", "nomfs", 0, NULL) != 0) {
        fprintf(stderr, "mount failed: %s\n", strerror(errno));
        return 1;
    }
    printf("mounted nomfs successfully\n");
    return 0;
}
