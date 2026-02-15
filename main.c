#include <stdio.h>

#include <sys/stat.h>

static int is_dir(const char *path) {
    struct stat st;
    return stat(path, &st) == 0 && S_ISDIR(st.st_mode);
}

int main() {
    printf("%d\n", is_dir("."));
    return 0;
}
