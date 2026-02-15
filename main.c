#define _XOPEN_SOURCE 700
#define _POSIX_C_SOURCE 200809L

#include <limits.h>
#include <stddef.h>
#include <stdio.h>
#include <dirent.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
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

// safely built path
static int build_path(
        char *buf, size_t buf_size,
        const char *dir, const char *name) {
    
    int res = snprintf(buf, buf_size, "%s/%s", dir, name);
    if (res >= (int)buf_size) return 0;
    return 1;
}

// check if directory is a git repo
static int is_repo(const char *path) {
    char buf[PATH_MAX];
    if (!build_path(buf, sizeof buf, path, ".git")) return 0;
    return is_dir(buf);
}

// recursively find repos in dir tree
static void find_repos(const char *path, size_t len) {
    DIR *dir = opendir(path);
    if (!dir) return;

    for (struct dirent *ent; (ent = readdir(dir)) != NULL;) {
        if (is_dot_dir(ent->d_name)) continue;

        char child[PATH_MAX];
        if (!build_path(child, sizeof child, path, ent->d_name)) continue;

        if (!is_dir(child)) continue;

        if (is_repo(child)) {
            printf("%s\n", child);
        } else {
            find_repos(child, len);
        }
    }
    closedir(dir);
}


int main() {
    char start[PATH_MAX];

    const char *home = getenv("HOME");
    snprintf(start, sizeof start, "%s/dev", home);

    char can[PATH_MAX];

    if (!realpath(start, can)) {
        return 0;
    }

    find_repos(can, strlen(can));
    return 0;
}
