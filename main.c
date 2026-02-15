#include <linux/limits.h>
#include <stddef.h>
#include <stdio.h>

#include <stdlib.h>
#include <sys/stat.h>

static int is_dir(const char *path) {
    struct stat st;
    return stat(path, &st) == 0 && S_ISDIR(st.st_mode);
}

// check if dir is "." or ".."
// static int is_dot_dir(const char *path) {
//     if (!is_dir(path)) return 0;
//     return path[0] == '.' &&
//         (path[1] == '\0' || (path[1] == '.' && path[2] == '\0'));
// }

// safely built path
static int build_path(
        char *buf, size_t buf_size,
        const char *dir, const char *name) {
    
    int res = snprintf(buf, buf_size, "%s/%s", dir, name);
    if (res >= (int)buf_size) return 0;
    return 1;
}

static int is_repo(const char *path) {
    char buf[PATH_MAX];
    if (!build_path(buf, sizeof buf, path, ".git")) return 0;
    return is_dir(buf);
}


int main() {
    printf("%d\n", is_repo("../fr"));
    printf("%d\n", is_repo("."));
    printf("%d\n", is_repo(getenv("HOME")));

    return 0;
}
