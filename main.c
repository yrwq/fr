#include <stdio.h>

#include <sys/stat.h>

static int is_dir(const char *path) {
    struct stat st;
    return stat(path, &st) == 0 && S_ISDIR(st.st_mode);
}

// check if dir is "." or ".."
static int is_dot_dir(const char *path) {
    if (!is_dir(path)) return 0;
    return path[0] == '.' &&
        (path[1] == '\0' || (path[1] == '.' && path[2] == '\0'));
}

int main() {
    printf("%d\n", is_dot_dir("."));
    printf("%d\n", is_dot_dir("/"));
    return 0;
}
