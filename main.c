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

// dynamic vec for repo paths
typedef struct {
    char **items;
    size_t count;
    size_t cap;
} repo_vec_t;

static repo_vec_t repos;

static void vec_init(repo_vec_t *v) {
    v->items = NULL;
    v->count = 0;
    v->cap = 0;
}

static void vec_free(repo_vec_t *v) {
    for (size_t i = 0; i < v->count; i++) {
        free(v->items[i]);
    }
    free(v->items);
}

// add item to vector
static void vec_push(repo_vec_t *v, const char *str) {
    if (v->count >= v->cap) {
        size_t new_cap = v->cap == 0 ? 16 : v->cap * 2;
        char **new_items = realloc(v->items, new_cap * sizeof(char *));
        if (!new_items) return;
        v->items = new_items;
        v->cap = new_cap;
    }

    v->items[v->count] = strdup(str);
    if (v->items[v->count]) {
        v->count++;
    }
}

// print all items
static void vec_print(const repo_vec_t *v) {
    for (size_t i = 0; i < v->count; i++) {
        puts(v->items[i]);
    }
}

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
            vec_push(&repos, child + len + 1);
        } else {
            find_repos(child, len);
        }
    }
    closedir(dir);
}


int main(int argc, char *argv[]) {
    char start[PATH_MAX];

    if (argc == 2) {
        snprintf(start, sizeof start, "%s", argv[1]);
    } else {
        const char *home = getenv("HOME");
        snprintf(start, sizeof start, "%s/dev", home);
    }

    char can[PATH_MAX];

    if (!realpath(start, can)) {
        fprintf(stderr, "failed to resolve path: %s\n", start);
        return 0;
    }

    vec_init(&repos);
    find_repos(can, strlen(can));
    vec_print(&repos);
    vec_free(&repos);
    return 0;
}
