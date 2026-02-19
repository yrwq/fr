#define _DEFAULT_SOURCE
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
#include <git2.h>

#define MAX_THREADS 8

// args
typedef struct {
    char path[PATH_MAX];
    int max_depth;
    int max_width;
    int mode;
} args_t;


// repo info
typedef struct {
    char *path;
    char branch[256];
} repo_info_t;

// dynamic vec for repo paths
typedef struct {
    char **items;
    repo_info_t *infos;
    size_t count;
    size_t cap;
    pthread_mutex_t lock;
} repo_vec_t;

// queue of dirs to process
typedef struct dir_node {
    char path[PATH_MAX];
    int depth;
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
static int max_depth = -1;
static int collect_repo_info = 1;

/*
   repo functions
 */

static int get_repo_info(const char *repo_path, repo_info_t *info) {
    git_repository *repo = NULL;

    if (git_repository_open(&repo, repo_path) != 0) {
        return -1;
    }

    // current branch
    git_reference *head = NULL;
    if (git_repository_head(&head, repo) == 0) {
        const char *branch_name = git_reference_shorthand(head);
        snprintf(info->branch, sizeof(info->branch), "%s", branch_name);
        git_reference_free(head);
    } else {
        snprintf(info->branch, sizeof(info->branch), "HEAD");
    }

    git_repository_free(repo);

    return 0;
}

/*
   vec functions
 */

static void vec_init(repo_vec_t *v) {
    v->items = NULL;
    v->infos = NULL;
    v->count = 0;
    v->cap = 0;
    pthread_mutex_init(&v->lock, NULL);
}

static void vec_free(repo_vec_t *v) {
    for (size_t i = 0; i < v->count; i++) {
        free(v->items[i]);
    }
    free(v->items);
    free(v->infos);
    pthread_mutex_destroy(&v->lock);
}

// add item to vector
static void vec_push(repo_vec_t *v, const char *str, const char *full_path) {
    char *item = strdup(str);
    if (!item) return;

    repo_info_t info;
    memset(&info, 0, sizeof(info));
    if (!collect_repo_info) {
        info.branch[0] = '\0';
    } else if (get_repo_info(full_path, &info) != 0) {
        snprintf(info.branch, sizeof(info.branch), "HEAD");
    }

    pthread_mutex_lock(&v->lock);

    if (v->count >= v->cap) {
        size_t new_cap = v->cap == 0 ? 16 : v->cap * 2;
        char **new_items = realloc(v->items, new_cap * sizeof(char *));
        if (!new_items) {
            pthread_mutex_unlock(&v->lock);
            free(item);
            return;
        }
        v->items = new_items;

        repo_info_t *new_infos = realloc(v->infos, new_cap * sizeof(repo_info_t));
        if (!new_infos) {
            pthread_mutex_unlock(&v->lock);
            free(item);
            return;
        }
        v->infos = new_infos;
        v->cap = new_cap;
    }

    v->items[v->count] = item;
    v->infos[v->count] = info;
    v->count++;

    pthread_mutex_unlock(&v->lock);
}

// print all items
static void vec_print(const repo_vec_t *v, int width, int mode) {
    for (size_t i = 0; i < v->count; i++) {
        const char *name = strrchr(v->items[i], '/');
        const char *display = name ? name + 1 : v->items[i];
        
        size_t len = strlen(display);
        if (mode == 1) {
            printf("/home/yrwq/%s\n", v->items[i]);
        } else {
            if (len > (size_t)width) {
                printf("%.*s..  %s\n", width - 2, display, v->infos[i].branch);
            } else {
                printf("%-*s  %s\n", width, display, v->infos[i].branch);
            }
        }
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

static void queue_push(dir_queue_t *q, const char *path, int depth) {
    dir_node_t *node = malloc(sizeof(*node));
    if (!node) return;
    
    snprintf(node->path, sizeof(node->path), "%s", path);
    node->depth = depth;
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

static int is_git_dir_name(const char *name) {
    return strcmp(name, ".git") == 0;
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

static void process_directory(const char *path, int depth) {
    DIR *dir = opendir(path);
    if (!dir) return;

    for (struct dirent *ent; (ent = readdir(dir)) != NULL;) {
        if (is_dot_dir(ent->d_name)) continue;
        if (is_git_dir_name(ent->d_name)) continue;

        int is_directory = 0;
        if (ent->d_type == DT_DIR) {
            is_directory = 1;
        } else if (ent->d_type == DT_UNKNOWN) {
            char child[PATH_MAX];
            if (!build_path(child, sizeof child, path, ent->d_name)) continue;
            if (is_dir(child)) is_directory = 1;
        }
        if (!is_directory) continue;

        char child[PATH_MAX];
        if (!build_path(child, sizeof child, path, ent->d_name)) continue;

        if (is_repo(child)) {
            // found repo - add to results
            vec_push(&repos, child + base_len + 1, child);
            // skip walking inside repos to avoid traversing massive trees
            continue;
        }
        
        // only queue subdirectories if we haven't reached max depth
        if (max_depth == -1 || depth < max_depth) {
            queue_push(&queue, child, depth + 1);
        }
    }
    closedir(dir);
}


static void *worker_thread(void *arg) {
    (void)arg;
    
    while (1) {
        dir_node_t *node = queue_pop(&queue);
        if (!node) {
            break;
        }
        process_directory(node->path, node->depth);
        free(node);
        
        queue_finish_work(&queue);
    }
    
    return NULL;
}

static void print_usage(const char *prog) {
    fprintf(stderr, "usage: %s <opts> [dir]\n", prog);
    fprintf(stderr, "opts:\n");
    fprintf(stderr, "  -d <depth>  max depth to search (def: unlimited)\n");
    fprintf(stderr, "  -w <width   max width for repo names (def: 10)\n");
    fprintf(stderr, "  -c          run with clean mode (only shows full path) \n");
    fprintf(stderr, "  -h          show this help message\n");
    fprintf(stderr, "\nif [dir] is not provided, defaults to $HOME\n");
}

static int parse_args(int argc, char *argv[], args_t *args) {
    // set defaults
    args->max_depth = -1;
    args->max_width = 10;
    args->mode = 0;
    args->path[0] = '\0';
    
    int i = 1;
    while (i < argc) {
        if (strcmp(argv[i], "-d") == 0) {
            if (i + 1 >= argc) {
                fprintf(stderr, "error: -d requires an argument\n");
                return -1;
            }
            args->max_depth = atoi(argv[i + 1]);
            if (args->max_depth < 0) {
                fprintf(stderr, "error: depth must be non-negative\n");
                return -1;
            }
            i += 2;
        } else if (strcmp(argv[i], "-w") == 0) {
            if (i + 1 >= argc) {
                fprintf(stderr, "error: -w requires an argument\n");
                return -1;
            }
            args->max_width = atoi(argv[i + 1]);
            i += 2;
        } else if (strcmp(argv[i], "-c") == 0 || strcmp(argv[i], "--clean") == 0) {
            args->mode = 1;
            i += 1;
        } else if (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--help") == 0) {
            print_usage(argv[0]);
            return 1;
        } else if (argv[i][0] == '-') {
            fprintf(stderr, "error: unknown option '%s'\n", argv[i]);
            print_usage(argv[0]);
            return -1;
        } else {
            // Must be the directory argument
            if (args->path[0] != '\0') {
                fprintf(stderr, "error: multiple directories specified\n");
                return -1;
            }
            snprintf(args->path, sizeof(args->path), "%s", argv[i]);
            i++;
        }
    }
    
    // no path specified
    if (args->path[0] == '\0') {
        const char *home = getenv("HOME");
        if (!home) {
            fprintf(stderr, "Error: HOME environment variable not set\n");
            return -1;
        }
        snprintf(args->path, sizeof(args->path), "%s", home);
    }
    
    return 0;
}

int main(int argc, char *argv[]) {
    args_t args;
    
    int parse_result = parse_args(argc, argv, &args);
    if (parse_result != 0) {
        // 1 = help, -1 = error
        return parse_result == 1 ? 0 : 1;
    }
    
    char can[PATH_MAX];
    if (!realpath(args.path, can)) {
        return 0;
    }

    base_len = strlen(can);
    max_depth = args.max_depth;
    collect_repo_info = (args.mode == 0);

    git_libgit2_init();

    vec_init(&repos);
    queue_init(&queue);


    pthread_t threads[MAX_THREADS];
    for (int i = 0; i < MAX_THREADS; i++) {
        pthread_create(&threads[i], NULL, worker_thread, &max_depth);
    }

    process_directory(can, 0);

    pthread_mutex_lock(&queue.lock);
    queue.active_workers--;
    pthread_cond_broadcast(&queue.cond);
    pthread_mutex_unlock(&queue.lock);

    for (int i = 0; i < MAX_THREADS; i++) {
        pthread_join(threads[i], NULL);
    }

    vec_print(&repos, args.max_width, args.mode);
    vec_free(&repos);
    queue_destroy(&queue);
    git_libgit2_shutdown();
    return 0;
}
