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
    char branch[256];
    char status[128];
    char upstream[512];
} repo_info_t;

typedef struct {
    int has_staged;
    int has_modified;
    int has_untracked;
    int has_conflicted;
} status_summary_t;

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
static int max_depth = -1;
static int collect_repo_info = 1;
static int use_color = 0;

#define COLOR_RESET "\033[0m"
#define COLOR_BOLD "\033[1m"
#define COLOR_GREEN "\033[32m"
#define COLOR_YELLOW "\033[33m"
#define COLOR_RED "\033[31m"
#define COLOR_CYAN "\033[36m"
#define COLOR_BLUE "\033[34m"
#define COLOR_DIM "\033[90m"

/*
   repo functions
 */

static int collect_repo_status(const char *path, unsigned int status_flags, void *payload) {
    (void)path;

    status_summary_t *summary = payload;
    if (status_flags & GIT_STATUS_CONFLICTED) {
        summary->has_conflicted = 1;
    }
    if (status_flags & (GIT_STATUS_INDEX_NEW |
                        GIT_STATUS_INDEX_MODIFIED |
                        GIT_STATUS_INDEX_DELETED |
                        GIT_STATUS_INDEX_RENAMED |
                        GIT_STATUS_INDEX_TYPECHANGE)) {
        summary->has_staged = 1;
    }
    if (status_flags & (GIT_STATUS_WT_MODIFIED |
                        GIT_STATUS_WT_DELETED |
                        GIT_STATUS_WT_RENAMED |
                        GIT_STATUS_WT_TYPECHANGE)) {
        summary->has_modified = 1;
    }
    if (status_flags & GIT_STATUS_WT_NEW) {
        summary->has_untracked = 1;
    }

    return 0;
}

static void format_repo_status(const status_summary_t *summary, repo_info_t *info) {
    if (!summary->has_staged && !summary->has_modified &&
        !summary->has_untracked && !summary->has_conflicted) {
        snprintf(info->status, sizeof(info->status), "clean");
        return;
    }

    if (summary->has_conflicted) {
        snprintf(info->status, sizeof(info->status), "conflicted");
        return;
    }
    if (summary->has_staged) {
        snprintf(info->status, sizeof(info->status), "staged");
        return;
    }
    if (summary->has_modified) {
        snprintf(info->status, sizeof(info->status), "modified");
        return;
    }
    if (summary->has_untracked) {
        snprintf(info->status, sizeof(info->status), "untracked");
        return;
    }

    snprintf(info->status, sizeof(info->status), "unknown");
}

static int get_repo_info(const char *repo_path, repo_info_t *info) {
    git_repository *repo = NULL;
    git_reference *head = NULL;
    git_reference *upstream = NULL;
    git_remote *remote = NULL;
    git_strarray remotes = {0};
    git_buf remote_name = GIT_BUF_INIT;
    status_summary_t summary = {0};

    if (git_repository_open(&repo, repo_path) != 0) {
        return -1;
    }

    snprintf(info->branch, sizeof(info->branch), "HEAD");
    snprintf(info->status, sizeof(info->status), "unknown");
    snprintf(info->upstream, sizeof(info->upstream), "-");

    if (git_repository_head(&head, repo) == 0) {
        const char *branch_name = git_reference_shorthand(head);
        snprintf(info->branch, sizeof(info->branch), "%s", branch_name);

        if (git_reference_is_branch(head) &&
            git_branch_upstream(&upstream, head) == 0 &&
            git_branch_remote_name(&remote_name, repo, git_reference_name(head)) == 0 &&
            git_remote_lookup(&remote, repo, remote_name.ptr) == 0) {
            const char *url = git_remote_url(remote);
            if (url && url[0] != '\0') {
                snprintf(info->upstream, sizeof(info->upstream), "%s", url);
            }
        }
    }

    if (strcmp(info->upstream, "-") == 0 &&
        git_remote_list(&remotes, repo) == 0 &&
        remotes.count > 0 &&
        git_remote_lookup(&remote, repo, remotes.strings[0]) == 0) {
        const char *url = git_remote_url(remote);
        if (url && url[0] != '\0') {
            snprintf(info->upstream, sizeof(info->upstream), "%s", url);
        }
    }

    git_status_options status_opts;
    if (git_status_options_init(&status_opts, GIT_STATUS_OPTIONS_VERSION) == 0) {
        status_opts.show = GIT_STATUS_SHOW_INDEX_AND_WORKDIR;
        status_opts.flags = GIT_STATUS_OPT_INCLUDE_UNTRACKED |
                            GIT_STATUS_OPT_EXCLUDE_SUBMODULES;
        if (git_status_foreach_ext(repo, &status_opts, collect_repo_status, &summary) == 0) {
            format_repo_status(&summary, info);
        }
    }

    git_remote_free(remote);
    git_strarray_dispose(&remotes);
    git_buf_dispose(&remote_name);
    git_reference_free(upstream);
    git_reference_free(head);
    git_repository_free(repo);

    return 0;
}

static const char *status_color(const char *status) {
    if (!use_color) return "";
    if (strcmp(status, "clean") == 0) return COLOR_GREEN;
    if (strstr(status, "conflicted") != NULL) return COLOR_RED;
    return COLOR_YELLOW;
}

static const char *name_color(void) {
    return use_color ? COLOR_BOLD : "";
}

static const char *branch_color(void) {
    return "";
}

static const char *path_color(void) {
    return use_color ? COLOR_DIM : "";
}

static const char *upstream_color(void) {
    return "";
}

static const char *color_reset(void) {
    return use_color ? COLOR_RESET : "";
}

static void format_repo_name(char *buf, size_t buf_size, const char *display, size_t repo_width) {
    size_t display_len = strlen(display);

    if (buf_size == 0) return;

    if (repo_width > 2 && display_len > repo_width) {
        snprintf(buf, buf_size, "%.*s..", (int)(repo_width - 2), display);
    } else {
        snprintf(buf, buf_size, "%s", display);
    }
}

static void format_home_relative_path(char *buf, size_t buf_size, const char *path) {
    const char *home = getenv("HOME");
    size_t home_len;

    if (buf_size == 0) return;

    if (!home || home[0] == '\0') {
        snprintf(buf, buf_size, "%s", path);
        return;
    }

    home_len = strlen(home);
    if (strncmp(path, home, home_len) == 0 &&
        (path[home_len] == '/' || path[home_len] == '\0')) {
        snprintf(buf, buf_size, "~%s", path + home_len);
    } else {
        snprintf(buf, buf_size, "%s", path);
    }
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
static void vec_push(repo_vec_t *v, const char *full_path) {
    char *item = strdup(full_path);
    if (!item) return;

    repo_info_t info;
    memset(&info, 0, sizeof(info));
    if (!collect_repo_info) {
        info.branch[0] = '\0';
    } else if (get_repo_info(full_path, &info) != 0) {
        snprintf(info.branch, sizeof(info.branch), "HEAD");
        snprintf(info.status, sizeof(info.status), "unknown");
        snprintf(info.upstream, sizeof(info.upstream), "-");
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
    size_t repo_width = (size_t)width;

    for (size_t i = 0; i < v->count; i++) {
        const char *name = strrchr(v->items[i], '/');
        const char *display = name ? name + 1 : v->items[i];

        if (mode == 1) {
            printf("%s\n", v->items[i]);
        } else {
            char display_buf[PATH_MAX];
            char path_buf[PATH_MAX];
            const char *status = v->infos[i].status;
            const char *branch = v->infos[i].branch;
            const char *upstream = v->infos[i].upstream[0] != '\0' ? v->infos[i].upstream : "-";
            const char *name_prefix = name_color();
            const char *status_prefix = status_color(status);
            const char *branch_prefix = branch_color();
            const char *path_prefix = path_color();
            const char *upstream_prefix = upstream_color();
            const char *reset = color_reset();

            format_repo_name(display_buf, sizeof(display_buf), display, repo_width);
            format_home_relative_path(path_buf, sizeof(path_buf), v->items[i]);

            printf("%s%s%s", name_prefix, display_buf, reset);
            printf("  %s%s%s\n", path_prefix, path_buf, reset);
            printf("  %s%s%s  %s(%s)%s\n",
                   status_prefix,
                   status,
                   reset,
                   branch_prefix,
                   branch,
                   reset);

            if (strcmp(upstream, "-") != 0) {
                printf("  %s%s%s\n", upstream_prefix, upstream, reset);
            }
            printf("\n");
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
    q->active_workers = 0;
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

static int should_prune_dir_name(const char *name) {
    return strcmp(name, "node_modules") == 0 ||
        strcmp(name, "target") == 0 ||
        strcmp(name, ".venv") == 0 ||
        strcmp(name, "dist") == 0 ||
        strcmp(name, "build") == 0;
}

// safely built path
static int build_path(char *buf, size_t buf_size,
        const char *dir, const char *name) {
    
    int res = snprintf(buf, buf_size, "%s/%s", dir, name);
    return res < (int)buf_size;
}

// process one dir - find repos and queue subdirs
static void process_directory(const char *path, int depth) {
    DIR *dir = opendir(path);
    if (!dir) return;

    int found_git_dir = 0;

    for (struct dirent *ent; (ent = readdir(dir)) != NULL;) {
        if (is_dot_dir(ent->d_name)) continue;

        if (is_git_dir_name(ent->d_name)) {
            if (ent->d_type == DT_DIR) {
                found_git_dir = 1;
            } else if (ent->d_type == DT_UNKNOWN) {
                char git_path[PATH_MAX];
                if (!build_path(git_path, sizeof git_path, path, ent->d_name)) continue;
                if (is_dir(git_path)) found_git_dir = 1;
            }
            continue;
        }

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
        if (should_prune_dir_name(ent->d_name)) continue;

        if (max_depth == -1 || depth <= max_depth) {
            queue_push(&queue, child, depth + 1);
        }
    }
    closedir(dir);

    if (found_git_dir && depth > 0) {
        vec_push(&repos, path);
    }
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
    args->max_width = 20;
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

    max_depth = args.max_depth;
    collect_repo_info = (args.mode == 0);
    use_color = (args.mode == 0 && isatty(STDOUT_FILENO));

    git_libgit2_init();

    vec_init(&repos);
    queue_init(&queue);

    queue_push(&queue, can, 0);

    pthread_t threads[MAX_THREADS];
    for (int i = 0; i < MAX_THREADS; i++) {
        pthread_create(&threads[i], NULL, worker_thread, &max_depth);
    }

    for (int i = 0; i < MAX_THREADS; i++) {
        pthread_join(threads[i], NULL);
    }

    vec_print(&repos, args.max_width, args.mode);
    vec_free(&repos);
    queue_destroy(&queue);
    git_libgit2_shutdown();
    return 0;
}
