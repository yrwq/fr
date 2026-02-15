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

#define MAX_THREADS 8

// dynamic vec for repo paths
typedef struct {
    char **items;
    size_t count;
    size_t cap;
    pthread_mutex_t lock;
} repo_vec_t;

// queue of dirs to process
typedef struct dir_node {
    char path[PATH_MAX];
    struct dir_node *next;
} dir_node_t;

typedef struct {
    dir_node_t *head;
    dir_node_t *tail;
    pthread_mutex_t lock;
    pthread_cond_t cond;
    int active_workers;
    int pending_items;
    int shutdown;
} dir_queue_t;

static repo_vec_t repos;
static dir_queue_t queue;
static size_t base_len;

/*
   vec functions
 */

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
            pthread_mutex_unlock(&v->lock);
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

/*
   queue functions
 */

static void queue_init(dir_queue_t *q) {
    q->head = NULL;
    q->tail = NULL;
    pthread_mutex_init(&q->lock, NULL);
    pthread_cond_init(&q->cond, NULL);
    q->active_workers = 1;
    q->pending_items = 0;
    q->shutdown = 0;
}

static void queue_push(dir_queue_t *q, const char *path) {
    dir_node_t *node = malloc(sizeof(*node));
    if (!node) return;
    
    snprintf(node->path, sizeof(node->path), "%s", path);
    node->next = NULL;
    
    pthread_mutex_lock(&q->lock);
    if (q->tail) {
        q->tail->next = node;
    } else {
        q->head = node;
    }
    q->pending_items++;
    q->tail = node;
    pthread_cond_signal(&q->cond);
    pthread_mutex_unlock(&q->lock);
}

static dir_node_t *queue_pop(dir_queue_t *q) {
    pthread_mutex_lock(&q->lock);
    
    while (!q->head && !q->shutdown) {
        if (q->active_workers == 0 && q->pending_items == 0) {
            q->shutdown = 1;
            pthread_cond_broadcast(&q->cond);
            pthread_mutex_unlock(&q->lock);
            return NULL;
        }
        pthread_cond_wait(&q->cond, &q->lock);
    }
    
    if (q->shutdown) {
        pthread_mutex_unlock(&q->lock);
        return NULL;
    }
    
    dir_node_t *node = q->head;
    q->head = node->next;
    if (!q->head) {
        q->tail = NULL;
    }
    q->active_workers++;
    q->pending_items--;
    
    pthread_mutex_unlock(&q->lock);
    return node;
}

static void queue_finish_work(dir_queue_t *q) {
    pthread_mutex_lock(&q->lock);
    q->active_workers--;
    
    // no workers, signal shutdown
    if (q->active_workers == 0 && !q->head) {
        q->shutdown = 1;
        pthread_cond_broadcast(&q->cond);
    } else {
        pthread_cond_signal(&q->cond);
    }
    pthread_mutex_unlock(&q->lock);
}

static void queue_destroy(dir_queue_t *q) {
    pthread_mutex_destroy(&q->lock);
    pthread_cond_destroy(&q->cond);
}

/*
   helper functions
 */

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

// process one dir - find repos and queue subdirs

static void process_directory(const char *path) {
    DIR *dir = opendir(path);
    if (!dir) return;

    for (struct dirent *ent; (ent = readdir(dir)) != NULL;) {
        if (is_dot_dir(ent->d_name)) continue;

        char child[PATH_MAX];
        if (!build_path(child, sizeof child, path, ent->d_name)) continue;
        if (!is_dir(child)) continue;

        if (is_repo(child)) {
            // found repo - add to results
            vec_push(&repos, child + base_len + 1);
            // queue subdir
        }
        queue_push(&queue, child);
    }
    closedir(dir);
}


static void *worker_thread(void *arg) {
    (void)arg;
    
    while (1) {
        dir_node_t *node = queue_pop(&queue);
        // shutdown
        if (!node) {
            break;
        }
        process_directory(node->path);
        free(node);
        
        queue_finish_work(&queue);
    }
    
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
        return 0;
    }

    base_len = strlen(can);

    vec_init(&repos);
    queue_init(&queue);


    pthread_t threads[MAX_THREADS];
    for (int i = 0; i < MAX_THREADS; i++) {
        pthread_create(&threads[i], NULL, worker_thread, NULL);
    }

    process_directory(can);

    pthread_mutex_lock(&queue.lock);
    queue.active_workers--;
    pthread_cond_broadcast(&queue.cond);
    pthread_mutex_unlock(&queue.lock);

    for (int i = 0; i < MAX_THREADS; i++) {
        pthread_join(threads[i], NULL);
    }

    vec_print(&repos);
    vec_free(&repos);
    queue_destroy(&queue);
    return 0;
}
