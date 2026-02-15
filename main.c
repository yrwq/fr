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
#include <pthread.h>
#include <semaphore.h>

#define MAX_THREADS 8

// dynamic vec for repo paths
typedef struct {
    char **items;
    size_t count;
    size_t cap;
    pthread_mutex_t lock;
} repo_vec_t;

static repo_vec_t repos;
static sem_t thread_limit;

static void vec_init(repo_vec_t *v) {
    v->items = NULL;
    v->count = 0;
    v->cap = 0;
    pthread_mutex_init(&v->lock, NULL);
}

static void vec_free(repo_vec_t *v) {
    for (size_t i = 0; i < v->count; i++) {
        free(v->items[i]);
    }
    free(v->items);
    pthread_mutex_destroy(&v->lock);
}

// add item to vector
static void vec_push(repo_vec_t *v, const char *str) {
    pthread_mutex_lock(&v->lock);
    if (v->count >= v->cap) {
        size_t new_cap = v->cap == 0 ? 16 : v->cap * 2;
        char **new_items = realloc(v->items, new_cap * sizeof(char *));
        if (!new_items) {
            pthread_mutex_lock(&v->lock);
            return;
        }
        v->items = new_items;
        v->cap = new_cap;
    }

    v->items[v->count] = strdup(str);
    if (v->items[v->count]) {
        v->count++;
    }

    pthread_mutex_unlock(&v->lock);
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
    return path[0] == '.' &&
        (path[1] == '\0' || (path[1] == '.' && path[2] == '\0'));
}

// safely built path
static int build_path(char *buf, size_t buf_size,
        const char *dir, const char *name) {
    
    int res = snprintf(buf, buf_size, "%s/%s", dir, name);
    return res < (int)buf_size;
}

// check if directory is a git repo
static int is_repo(const char *path) {
    char buf[PATH_MAX];
    if (!build_path(buf, sizeof buf, path, ".git")) return 0;
    return is_dir(buf);
}

// forward
static void *find_repos_thread(void *arg);

typedef struct {
    char path[PATH_MAX];
    size_t len;
} thread_arg_t;

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
            // relative path to vec
            vec_push(&repos, child + len + 1);
        } else {
            // try to spawn async thread for subdir
            if (sem_trywait(&thread_limit) == 0) {
                thread_arg_t *arg = malloc(sizeof(*arg));
                if (arg) {
                    snprintf(arg->path, sizeof(arg->path), "%s", child);
                    arg->len = len;
                    pthread_t tid;
                    pthread_attr_t attr;
                    pthread_attr_init(&attr);
                    pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);

                    if (pthread_create(&tid, &attr, find_repos_thread, arg) == 0) {
                        pthread_attr_destroy(&attr);
                        // spawned thread
                        continue;
                    }

                    // failed to create thread
                    pthread_attr_destroy(&attr);
                    free(arg);
                    sem_post(&thread_limit);
                }
            }

            // no threads available, or failed to spawn
            // process synchronously
            find_repos(child, len);
        }
    }
    closedir(dir);
}

static void *find_repos_thread(void *arg) {
    thread_arg_t *ta = arg;
    find_repos(ta->path, ta->len);
    free(ta);
    // release thread slot
    sem_post(&thread_limit);
    return NULL;
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
    sem_init(&thread_limit, 0, MAX_THREADS);

    // reserve one slot for main search
    sem_wait(&thread_limit);

    find_repos(can, strlen(can));

    // release main slot
    sem_post(&thread_limit);

    // wait for all threads to complete
    // acquire all slots means all threads are done
    for (int i = 0; i < MAX_THREADS; i++) {
        sem_wait(&thread_limit);
    }

    vec_print(&repos);

    vec_free(&repos);
    sem_destroy(&thread_limit);
    return 0;
}
