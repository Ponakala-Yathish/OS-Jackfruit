/*
 * engine.c - Supervised Multi-Container Runtime (User Space)
 *
 * Full implementation of:
 *   - bounded-buffer producer/consumer logging
 *   - container lifecycle via clone() with PID/UTS/mount namespaces
 *   - UNIX domain socket control plane (supervisor + client)
 *   - SIGCHLD / SIGINT / SIGTERM handling
 *   - kernel monitor registration via ioctl
 */

#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <pthread.h>
#include <sched.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mount.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "monitor_ioctl.h"

#define STACK_SIZE (1024 * 1024)
#define CONTAINER_ID_LEN 32
#define CONTROL_PATH "/tmp/mini_runtime.sock"
#define LOG_DIR "logs"
#define CONTROL_MESSAGE_LEN 256
#define CHILD_COMMAND_LEN 256
#define LOG_CHUNK_SIZE 4096
#define LOG_BUFFER_CAPACITY 16
#define DEFAULT_SOFT_LIMIT (40UL << 20)
#define DEFAULT_HARD_LIMIT (64UL << 20)

typedef enum {
    CMD_SUPERVISOR = 0,
    CMD_START,
    CMD_RUN,
    CMD_PS,
    CMD_LOGS,
    CMD_STOP
} command_kind_t;

typedef enum {
    CONTAINER_STARTING = 0,
    CONTAINER_RUNNING,
    CONTAINER_STOPPED,
    CONTAINER_KILLED,
    CONTAINER_EXITED
} container_state_t;

typedef struct container_record {
    char id[CONTAINER_ID_LEN];
    pid_t host_pid;
    time_t started_at;
    container_state_t state;
    unsigned long soft_limit_bytes;
    unsigned long hard_limit_bytes;
    int exit_code;
    int exit_signal;
    char log_path[PATH_MAX];
    struct container_record *next;
} container_record_t;

typedef struct {
    char container_id[CONTAINER_ID_LEN];
    size_t length;
    char data[LOG_CHUNK_SIZE];
} log_item_t;

typedef struct {
    log_item_t items[LOG_BUFFER_CAPACITY];
    size_t head;
    size_t tail;
    size_t count;
    int shutting_down;
    pthread_mutex_t mutex;
    pthread_cond_t not_empty;
    pthread_cond_t not_full;
} bounded_buffer_t;

typedef struct {
    command_kind_t kind;
    char container_id[CONTAINER_ID_LEN];
    char rootfs[PATH_MAX];
    char command[CHILD_COMMAND_LEN];
    unsigned long soft_limit_bytes;
    unsigned long hard_limit_bytes;
    int nice_value;
} control_request_t;

typedef struct {
    int status;
    char message[CONTROL_MESSAGE_LEN];
} control_response_t;

typedef struct {
    char id[CONTAINER_ID_LEN];
    char rootfs[PATH_MAX];
    char command[CHILD_COMMAND_LEN];
    int nice_value;
    int log_write_fd;
} child_config_t;

typedef struct {
    int server_fd;
    int monitor_fd;
    int should_stop;
    pthread_t logger_thread;
    bounded_buffer_t log_buffer;
    pthread_mutex_t metadata_lock;
    container_record_t *containers;
} supervisor_ctx_t;

/* Global supervisor context pointer for signal handlers */
static supervisor_ctx_t *g_ctx = NULL;

/* ---------------------------------------------------------------
 * Usage
 * --------------------------------------------------------------- */
static void usage(const char *prog)
{
    fprintf(stderr,
            "Usage:\n"
            "  %s supervisor <base-rootfs>\n"
            "  %s start <id> <container-rootfs> <command> [--soft-mib N] [--hard-mib N] [--nice N]\n"
            "  %s run <id> <container-rootfs> <command> [--soft-mib N] [--hard-mib N] [--nice N]\n"
            "  %s ps\n"
            "  %s logs <id>\n"
            "  %s stop <id>\n",
            prog, prog, prog, prog, prog, prog);
}

static int parse_mib_flag(const char *flag,
                          const char *value,
                          unsigned long *target_bytes)
{
    char *end = NULL;
    unsigned long mib;

    errno = 0;
    mib = strtoul(value, &end, 10);
    if (errno != 0 || end == value || *end != '\0') {
        fprintf(stderr, "Invalid value for %s: %s\n", flag, value);
        return -1;
    }

    if (mib > ULONG_MAX / (1UL << 20)) {
        fprintf(stderr, "Value for %s is too large: %s\n", flag, value);
        return -1;
    }

    *target_bytes = mib * (1UL << 20);
    return 0;
}

static int parse_optional_flags(control_request_t *req,
                                int argc,
                                char *argv[],
                                int start_index)
{
    int i;

    for (i = start_index; i < argc; i += 2) {
        char *end = NULL;
        long nice_value;

        if (i + 1 >= argc) {
            fprintf(stderr, "Missing value for option: %s\n", argv[i]);
            return -1;
        }

        if (strcmp(argv[i], "--soft-mib") == 0) {
            if (parse_mib_flag("--soft-mib", argv[i + 1], &req->soft_limit_bytes) != 0)
                return -1;
            continue;
        }

        if (strcmp(argv[i], "--hard-mib") == 0) {
            if (parse_mib_flag("--hard-mib", argv[i + 1], &req->hard_limit_bytes) != 0)
                return -1;
            continue;
        }

        if (strcmp(argv[i], "--nice") == 0) {
            errno = 0;
            nice_value = strtol(argv[i + 1], &end, 10);
            if (errno != 0 || end == argv[i + 1] || *end != '\0' ||
                nice_value < -20 || nice_value > 19) {
                fprintf(stderr,
                        "Invalid value for --nice (expected -20..19): %s\n",
                        argv[i + 1]);
                return -1;
            }
            req->nice_value = (int)nice_value;
            continue;
        }

        fprintf(stderr, "Unknown option: %s\n", argv[i]);
        return -1;
    }

    if (req->soft_limit_bytes > req->hard_limit_bytes) {
        fprintf(stderr, "Invalid limits: soft limit cannot exceed hard limit\n");
        return -1;
    }

    return 0;
}

static const char *state_to_string(container_state_t state)
{
    switch (state) {
    case CONTAINER_STARTING: return "starting";
    case CONTAINER_RUNNING:  return "running";
    case CONTAINER_STOPPED:  return "stopped";
    case CONTAINER_KILLED:   return "killed";
    case CONTAINER_EXITED:   return "exited";
    default:                 return "unknown";
    }
}

/* ===============================================================
 * Bounded Buffer Implementation
 * =============================================================== */

static int bounded_buffer_init(bounded_buffer_t *buffer)
{
    int rc;

    memset(buffer, 0, sizeof(*buffer));

    rc = pthread_mutex_init(&buffer->mutex, NULL);
    if (rc != 0) return rc;

    rc = pthread_cond_init(&buffer->not_empty, NULL);
    if (rc != 0) {
        pthread_mutex_destroy(&buffer->mutex);
        return rc;
    }

    rc = pthread_cond_init(&buffer->not_full, NULL);
    if (rc != 0) {
        pthread_cond_destroy(&buffer->not_empty);
        pthread_mutex_destroy(&buffer->mutex);
        return rc;
    }

    return 0;
}

static void bounded_buffer_destroy(bounded_buffer_t *buffer)
{
    pthread_cond_destroy(&buffer->not_full);
    pthread_cond_destroy(&buffer->not_empty);
    pthread_mutex_destroy(&buffer->mutex);
}

static void bounded_buffer_begin_shutdown(bounded_buffer_t *buffer)
{
    pthread_mutex_lock(&buffer->mutex);
    buffer->shutting_down = 1;
    pthread_cond_broadcast(&buffer->not_empty);
    pthread_cond_broadcast(&buffer->not_full);
    pthread_mutex_unlock(&buffer->mutex);
}

/*
 * Producer: insert a log item into the bounded buffer.
 * Blocks if full. Returns 0 on success, -1 if shutting down.
 */
int bounded_buffer_push(bounded_buffer_t *buffer, const log_item_t *item)
{
    pthread_mutex_lock(&buffer->mutex);

    /* Wait while buffer is full and not shutting down */
    while (buffer->count == LOG_BUFFER_CAPACITY && !buffer->shutting_down)
        pthread_cond_wait(&buffer->not_full, &buffer->mutex);

    if (buffer->shutting_down) {
        pthread_mutex_unlock(&buffer->mutex);
        return -1;
    }

    /* Insert at tail */
    buffer->items[buffer->tail] = *item;
    buffer->tail = (buffer->tail + 1) % LOG_BUFFER_CAPACITY;
    buffer->count++;

    pthread_cond_signal(&buffer->not_empty);
    pthread_mutex_unlock(&buffer->mutex);
    return 0;
}

/*
 * Consumer: remove a log item from the bounded buffer.
 * Blocks if empty. Returns 0 on success, -1 if shutting down and empty.
 */
int bounded_buffer_pop(bounded_buffer_t *buffer, log_item_t *item)
{
    pthread_mutex_lock(&buffer->mutex);

    /* Wait while buffer is empty */
    while (buffer->count == 0 && !buffer->shutting_down)
        pthread_cond_wait(&buffer->not_empty, &buffer->mutex);

    /* If shutting down and nothing left, signal done */
    if (buffer->count == 0 && buffer->shutting_down) {
        pthread_mutex_unlock(&buffer->mutex);
        return -1;
    }

    /* Remove from head */
    *item = buffer->items[buffer->head];
    buffer->head = (buffer->head + 1) % LOG_BUFFER_CAPACITY;
    buffer->count--;

    pthread_cond_signal(&buffer->not_full);
    pthread_mutex_unlock(&buffer->mutex);
    return 0;
}

/* ===============================================================
 * Logging Thread
 * =============================================================== */

/*
 * Consumer thread: drains the bounded buffer and writes chunks
 * to per-container log files.
 */
void *logging_thread(void *arg)
{
    supervisor_ctx_t *ctx = (supervisor_ctx_t *)arg;
    log_item_t item;

    while (1) {
        if (bounded_buffer_pop(&ctx->log_buffer, &item) != 0)
            break; /* shutting down and buffer is empty */

        /* Build log file path and append the chunk */
        char path[PATH_MAX];
        snprintf(path, sizeof(path), "%s/%s.log", LOG_DIR, item.container_id);

        int fd = open(path, O_WRONLY | O_CREAT | O_APPEND, 0644);
        if (fd < 0) {
            perror("logging_thread: open log file");
            continue;
        }

        ssize_t written = 0;
        ssize_t total = (ssize_t)item.length;
        while (written < total) {
            ssize_t n = write(fd, item.data + written, (size_t)(total - written));
            if (n <= 0) break;
            written += n;
        }
        close(fd);
    }

    return NULL;
}

/* ===============================================================
 * Log reader thread (one per container)
 * Reads from the pipe connected to the container's stdout/stderr
 * and pushes chunks into the bounded buffer.
 * =============================================================== */

typedef struct {
    int read_fd;
    char container_id[CONTAINER_ID_LEN];
    bounded_buffer_t *buffer;
} pipe_reader_args_t;

static void *pipe_reader_thread(void *arg)
{
    pipe_reader_args_t *args = (pipe_reader_args_t *)arg;
    log_item_t item;
    ssize_t n;

    while (1) {
        memset(&item, 0, sizeof(item));
        strncpy(item.container_id, args->container_id, CONTAINER_ID_LEN - 1);

        n = read(args->read_fd, item.data, LOG_CHUNK_SIZE);
        if (n <= 0) break; /* EOF or error: container exited */

        item.length = (size_t)n;
        bounded_buffer_push(args->buffer, &item);
    }

    close(args->read_fd);
    free(args);
    return NULL;
}

/* ===============================================================
 * Container Child Function
 * =============================================================== */

/*
 * Runs inside the cloned namespace.
 * Sets up chroot, mounts /proc, redirects stdio, execs command.
 */
int child_fn(void *arg)
{
    child_config_t *cfg = (child_config_t *)arg;

    /* Redirect stdout and stderr to the logging pipe */
    if (dup2(cfg->log_write_fd, STDOUT_FILENO) < 0 ||
        dup2(cfg->log_write_fd, STDERR_FILENO) < 0) {
        perror("child_fn: dup2");
        return 1;
    }
    close(cfg->log_write_fd);

    /* Set hostname to container ID (UTS namespace) */
    if (sethostname(cfg->id, strlen(cfg->id)) < 0)
        perror("child_fn: sethostname"); /* non-fatal */

    /* chroot into the container's rootfs */
    if (chroot(cfg->rootfs) < 0) {
        perror("child_fn: chroot");
        return 1;
    }
    if (chdir("/") < 0) {
        perror("child_fn: chdir /");
        return 1;
    }

    /* Mount /proc so ps, top, etc. work inside the container */
    if (mount("proc", "/proc", "proc",
              MS_NOSUID | MS_NODEV | MS_NOEXEC, NULL) < 0) {
        /* Not fatal — container may still work without it */
        perror("child_fn: mount /proc");
    }

    /* Apply nice value if requested */
    if (cfg->nice_value != 0)
        nice(cfg->nice_value);

    /* Execute the requested command */
    char *argv_exec[] = { cfg->command, NULL };
    char *envp[] = {
        "PATH=/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin",
        "HOME=/root",
        "TERM=xterm",
        NULL
    };

    execve(cfg->command, argv_exec, envp);
    perror("child_fn: execve");
    return 1;
}

/* ===============================================================
 * Monitor Registration
 * =============================================================== */

int register_with_monitor(int monitor_fd,
                          const char *container_id,
                          pid_t host_pid,
                          unsigned long soft_limit_bytes,
                          unsigned long hard_limit_bytes)
{
    struct monitor_request req;

    memset(&req, 0, sizeof(req));
    req.pid = host_pid;
    req.soft_limit_bytes = soft_limit_bytes;
    req.hard_limit_bytes = hard_limit_bytes;
    strncpy(req.container_id, container_id, sizeof(req.container_id) - 1);

    if (ioctl(monitor_fd, MONITOR_REGISTER, &req) < 0)
        return -1;

    return 0;
}

int unregister_from_monitor(int monitor_fd, const char *container_id, pid_t host_pid)
{
    struct monitor_request req;

    memset(&req, 0, sizeof(req));
    req.pid = host_pid;
    strncpy(req.container_id, container_id, sizeof(req.container_id) - 1);

    if (ioctl(monitor_fd, MONITOR_UNREGISTER, &req) < 0)
        return -1;

    return 0;
}

/* ===============================================================
 * Container Launch Helper
 * =============================================================== */

/*
 * Launches a container using clone() with PID+UTS+mount namespaces.
 * Creates a pipe for logging, spawns a pipe-reader thread, registers
 * the container with the kernel monitor, and records metadata.
 */
static int launch_container(supervisor_ctx_t *ctx,
                            const control_request_t *req,
                            int foreground)
{
    int pipefd[2];
    char *stack;
    pid_t pid;
    container_record_t *rec;
    child_config_t *cfg;

    /* Ensure log directory exists */
    mkdir(LOG_DIR, 0755);

    /* Pipe: container writes here, supervisor reads */
    if (pipe(pipefd) < 0) {
        perror("launch_container: pipe");
        return -1;
    }

    /* Build child config (on heap — child references this before exec) */
    cfg = calloc(1, sizeof(child_config_t));
    if (!cfg) {
        close(pipefd[0]);
        close(pipefd[1]);
        return -1;
    }
    strncpy(cfg->id, req->container_id, CONTAINER_ID_LEN - 1);
    strncpy(cfg->rootfs, req->rootfs, PATH_MAX - 1);
    strncpy(cfg->command, req->command, CHILD_COMMAND_LEN - 1);
    cfg->nice_value = req->nice_value;
    cfg->log_write_fd = pipefd[1]; /* child writes here */

    /* Allocate stack for clone() */
    stack = malloc(STACK_SIZE);
    if (!stack) {
        free(cfg);
        close(pipefd[0]);
        close(pipefd[1]);
        return -1;
    }

    /* Clone with isolated namespaces */
    pid = clone(child_fn,
                stack + STACK_SIZE,  /* stack grows downward */
                CLONE_NEWPID | CLONE_NEWUTS | CLONE_NEWNS | SIGCHLD,
                cfg);

    /* Parent closes the write end; child owns it */
    close(pipefd[1]);

    if (pid < 0) {
        perror("launch_container: clone");
        free(stack);
        free(cfg);
        close(pipefd[0]);
        return -1;
    }

    free(stack); /* stack can be freed after clone() returns in parent */

    /* Register with kernel monitor */
    if (ctx->monitor_fd >= 0) {
        register_with_monitor(ctx->monitor_fd,
                              req->container_id, pid,
                              req->soft_limit_bytes,
                              req->hard_limit_bytes);
    }

    /* Record container metadata */
    rec = calloc(1, sizeof(container_record_t));
    if (!rec) {
        close(pipefd[0]);
        return -1;
    }
    strncpy(rec->id, req->container_id, CONTAINER_ID_LEN - 1);
    rec->host_pid = pid;
    rec->started_at = time(NULL);
    rec->state = CONTAINER_RUNNING;
    rec->soft_limit_bytes = req->soft_limit_bytes;
    rec->hard_limit_bytes = req->hard_limit_bytes;
    snprintf(rec->log_path, PATH_MAX, "%s/%s.log", LOG_DIR, req->container_id);

    pthread_mutex_lock(&ctx->metadata_lock);
    rec->next = ctx->containers;
    ctx->containers = rec;
    pthread_mutex_unlock(&ctx->metadata_lock);

    /* Spawn pipe-reader thread to forward logs into bounded buffer */
    pipe_reader_args_t *reader_args = calloc(1, sizeof(pipe_reader_args_t));
    if (reader_args) {
        reader_args->read_fd = pipefd[0];
        strncpy(reader_args->container_id, req->container_id, CONTAINER_ID_LEN - 1);
        reader_args->buffer = &ctx->log_buffer;

        pthread_t reader_tid;
        pthread_create(&reader_tid, NULL, pipe_reader_thread, reader_args);
        pthread_detach(reader_tid);
    } else {
        close(pipefd[0]);
    }

    if (foreground) {
        /* Wait for the container to finish */
        int wstatus;
        waitpid(pid, &wstatus, 0);

        pthread_mutex_lock(&ctx->metadata_lock);
        if (WIFEXITED(wstatus)) {
            rec->state = CONTAINER_EXITED;
            rec->exit_code = WEXITSTATUS(wstatus);
        } else if (WIFSIGNALED(wstatus)) {
            rec->state = CONTAINER_KILLED;
            rec->exit_signal = WTERMSIG(wstatus);
        }
        pthread_mutex_unlock(&ctx->metadata_lock);

        if (ctx->monitor_fd >= 0)
            unregister_from_monitor(ctx->monitor_fd, req->container_id, pid);
    }

    return 0;
}

/* ===============================================================
 * Signal Handling
 * =============================================================== */

static void sigchld_handler(int sig)
{
    (void)sig;
    int wstatus;
    pid_t pid;

    /* Reap all exited children without blocking */
    while ((pid = waitpid(-1, &wstatus, WNOHANG)) > 0) {
        if (!g_ctx) continue;

        pthread_mutex_lock(&g_ctx->metadata_lock);
        container_record_t *c = g_ctx->containers;
        while (c) {
            if (c->host_pid == pid) {
                if (WIFEXITED(wstatus)) {
                    c->state = CONTAINER_EXITED;
                    c->exit_code = WEXITSTATUS(wstatus);
                } else if (WIFSIGNALED(wstatus)) {
                    c->state = CONTAINER_KILLED;
                    c->exit_signal = WTERMSIG(wstatus);
                }
                break;
            }
            c = c->next;
        }
        pthread_mutex_unlock(&g_ctx->metadata_lock);

        if (g_ctx->monitor_fd >= 0 && c) {
            unregister_from_monitor(g_ctx->monitor_fd, c->id, pid);
        }
    }
}

static void sigterm_handler(int sig)
{
    (void)sig;
    if (g_ctx)
        g_ctx->should_stop = 1;
}

/* ===============================================================
 * Supervisor Event Loop
 * =============================================================== */

/*
 * Handles one accepted client connection.
 * Reads a control_request_t, dispatches, sends control_response_t.
 */
static void handle_client(supervisor_ctx_t *ctx, int client_fd)
{
    control_request_t req;
    control_response_t resp;
    ssize_t n;

    memset(&resp, 0, sizeof(resp));

    n = recv(client_fd, &req, sizeof(req), MSG_WAITALL);
    if (n != (ssize_t)sizeof(req)) {
        resp.status = -1;
        snprintf(resp.message, sizeof(resp.message), "Bad request size");
        send(client_fd, &resp, sizeof(resp), 0);
        return;
    }

    switch (req.kind) {

    case CMD_START:
        if (launch_container(ctx, &req, 0) == 0) {
            resp.status = 0;
            snprintf(resp.message, sizeof(resp.message),
                     "Container %s started", req.container_id);
        } else {
            resp.status = -1;
            snprintf(resp.message, sizeof(resp.message),
                     "Failed to start container %s", req.container_id);
        }
        break;

    case CMD_RUN:
        if (launch_container(ctx, &req, 1) == 0) {
            resp.status = 0;
            snprintf(resp.message, sizeof(resp.message),
                     "Container %s finished", req.container_id);
        } else {
            resp.status = -1;
            snprintf(resp.message, sizeof(resp.message),
                     "Failed to run container %s", req.container_id);
        }
        break;

    case CMD_PS: {
        /* Build a text table of all containers */
        char buf[4096];
        int offset = 0;
        offset += snprintf(buf + offset, sizeof(buf) - offset,
                           "%-16s %-8s %-10s %-12s %-14s %-14s\n",
                           "ID", "PID", "STATE", "EXIT",
                           "SOFT(MiB)", "HARD(MiB)");

        pthread_mutex_lock(&ctx->metadata_lock);
        container_record_t *c = ctx->containers;
        while (c && offset < (int)sizeof(buf) - 128) {
            offset += snprintf(buf + offset, sizeof(buf) - offset,
                               "%-16s %-8d %-10s %-12d %-14lu %-14lu\n",
                               c->id, c->host_pid,
                               state_to_string(c->state),
                               c->exit_code,
                               c->soft_limit_bytes >> 20,
                               c->hard_limit_bytes >> 20);
            c = c->next;
        }
        pthread_mutex_unlock(&ctx->metadata_lock);

        resp.status = 0;
        snprintf(resp.message, sizeof(resp.message), "%s", buf);
        break;
    }

    case CMD_LOGS: {
        /* Read log file and send its contents in the message field */
        char path[PATH_MAX];
        snprintf(path, sizeof(path), "%s/%s.log", LOG_DIR, req.container_id);

        int fd = open(path, O_RDONLY);
        if (fd < 0) {
            resp.status = -1;
            snprintf(resp.message, sizeof(resp.message),
                     "No log found for %s", req.container_id);
        } else {
            resp.status = 0;
            ssize_t r = read(fd, resp.message, sizeof(resp.message) - 1);
            if (r < 0) r = 0;
            resp.message[r] = '\0';
            close(fd);
        }
        break;
    }

    case CMD_STOP: {
        /* Find container and send SIGTERM, then SIGKILL */
        pthread_mutex_lock(&ctx->metadata_lock);
        container_record_t *c = ctx->containers;
        while (c) {
            if (strcmp(c->id, req.container_id) == 0) break;
            c = c->next;
        }
        if (c && c->state == CONTAINER_RUNNING) {
            kill(c->host_pid, SIGTERM);
            c->state = CONTAINER_STOPPED;
            resp.status = 0;
            snprintf(resp.message, sizeof(resp.message),
                     "Container %s stopped", req.container_id);
        } else {
            resp.status = -1;
            snprintf(resp.message, sizeof(resp.message),
                     "Container %s not found or not running", req.container_id);
        }
        pthread_mutex_unlock(&ctx->metadata_lock);

        if (ctx->monitor_fd >= 0 && resp.status == 0) {
            pthread_mutex_lock(&ctx->metadata_lock);
            c = ctx->containers;
            while (c) {
                if (strcmp(c->id, req.container_id) == 0) break;
                c = c->next;
            }
            if (c)
                unregister_from_monitor(ctx->monitor_fd, c->id, c->host_pid);
            pthread_mutex_unlock(&ctx->metadata_lock);
        }
        break;
    }

    default:
        resp.status = -1;
        snprintf(resp.message, sizeof(resp.message), "Unknown command");
        break;
    }

    send(client_fd, &resp, sizeof(resp), 0);
}

/* ===============================================================
 * Supervisor Main
 * =============================================================== */

static int run_supervisor(const char *rootfs)
{
    supervisor_ctx_t ctx;
    int rc;
    struct sockaddr_un addr;

    (void)rootfs; /* base rootfs stored for reference; containers use own rootfs */

    memset(&ctx, 0, sizeof(ctx));
    ctx.server_fd = -1;
    ctx.monitor_fd = -1;
    g_ctx = &ctx;

    /* Init metadata lock */
    rc = pthread_mutex_init(&ctx.metadata_lock, NULL);
    if (rc != 0) { errno = rc; perror("pthread_mutex_init"); return 1; }

    /* Init bounded buffer */
    rc = bounded_buffer_init(&ctx.log_buffer);
    if (rc != 0) { errno = rc; perror("bounded_buffer_init"); return 1; }

    /* Ensure log directory */
    mkdir(LOG_DIR, 0755);

    /* Open kernel monitor device (optional — continue if unavailable) */
    ctx.monitor_fd = open("/dev/container_monitor", O_RDWR);
    if (ctx.monitor_fd < 0)
        fprintf(stderr, "Warning: /dev/container_monitor not available: %s\n",
                strerror(errno));

    /* Create UNIX domain socket control plane */
    unlink(CONTROL_PATH); /* Remove stale socket */
    ctx.server_fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (ctx.server_fd < 0) { perror("socket"); return 1; }

    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, CONTROL_PATH, sizeof(addr.sun_path) - 1);

    if (bind(ctx.server_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("bind");
        return 1;
    }
    if (listen(ctx.server_fd, 10) < 0) {
        perror("listen");
        return 1;
    }

    /* Set non-blocking so we can poll for should_stop */
    int flags = fcntl(ctx.server_fd, F_GETFL, 0);
    fcntl(ctx.server_fd, F_SETFL, flags | O_NONBLOCK);

    /* Install signal handlers */
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = sigchld_handler;
    sa.sa_flags = SA_RESTART | SA_NOCLDSTOP;
    sigaction(SIGCHLD, &sa, NULL);

    sa.sa_handler = sigterm_handler;
    sa.sa_flags = SA_RESTART;
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);

    /* Start logger consumer thread */
    rc = pthread_create(&ctx.logger_thread, NULL, logging_thread, &ctx);
    if (rc != 0) {
        errno = rc;
        perror("pthread_create logger");
        return 1;
    }

    fprintf(stdout, "Supervisor started. Control socket: %s\n", CONTROL_PATH);
    fflush(stdout);

    /* Event loop */
    while (!ctx.should_stop) {
        int client_fd = accept(ctx.server_fd, NULL, NULL);
        if (client_fd < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                usleep(50000); /* 50ms poll */
                continue;
            }
            if (errno == EINTR) continue;
            perror("accept");
            break;
        }
        handle_client(&ctx, client_fd);
        close(client_fd);
    }

    /* Orderly shutdown */
    fprintf(stdout, "Supervisor shutting down...\n");

    /* Stop all running containers */
    pthread_mutex_lock(&ctx.metadata_lock);
    container_record_t *c = ctx.containers;
    while (c) {
        if (c->state == CONTAINER_RUNNING) {
            kill(c->host_pid, SIGTERM);
            c->state = CONTAINER_STOPPED;
        }
        c = c->next;
    }
    pthread_mutex_unlock(&ctx.metadata_lock);

    /* Wait for children */
    while (waitpid(-1, NULL, WNOHANG) > 0)
        ;

    /* Shutdown logger */
    bounded_buffer_begin_shutdown(&ctx.log_buffer);
    pthread_join(ctx.logger_thread, NULL);

    /* Free container records */
    pthread_mutex_lock(&ctx.metadata_lock);
    c = ctx.containers;
    while (c) {
        container_record_t *next = c->next;
        free(c);
        c = next;
    }
    ctx.containers = NULL;
    pthread_mutex_unlock(&ctx.metadata_lock);

    bounded_buffer_destroy(&ctx.log_buffer);
    pthread_mutex_destroy(&ctx.metadata_lock);

    if (ctx.monitor_fd >= 0) close(ctx.monitor_fd);
    close(ctx.server_fd);
    unlink(CONTROL_PATH);

    fprintf(stdout, "Supervisor exited cleanly.\n");
    return 0;
}

/* ===============================================================
 * Client-Side Control Request
 * =============================================================== */

static int send_control_request(const control_request_t *req)
{
    int sock_fd;
    struct sockaddr_un addr;
    control_response_t resp;

    sock_fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (sock_fd < 0) { perror("socket"); return 1; }

    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, CONTROL_PATH, sizeof(addr.sun_path) - 1);

    if (connect(sock_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        fprintf(stderr, "Cannot connect to supervisor at %s: %s\n"
                        "Is the supervisor running? (sudo ./engine supervisor <rootfs>)\n",
                CONTROL_PATH, strerror(errno));
        close(sock_fd);
        return 1;
    }

    if (send(sock_fd, req, sizeof(*req), 0) != (ssize_t)sizeof(*req)) {
        perror("send");
        close(sock_fd);
        return 1;
    }

    ssize_t n = recv(sock_fd, &resp, sizeof(resp), MSG_WAITALL);
    close(sock_fd);

    if (n != (ssize_t)sizeof(resp)) {
        fprintf(stderr, "Truncated response from supervisor\n");
        return 1;
    }

    printf("%s\n", resp.message);
    return (resp.status == 0) ? 0 : 1;
}

/* ===============================================================
 * CLI Command Handlers
 * =============================================================== */

static int cmd_start(int argc, char *argv[])
{
    control_request_t req;

    if (argc < 5) {
        fprintf(stderr,
                "Usage: %s start <id> <container-rootfs> <command> [--soft-mib N] [--hard-mib N] [--nice N]\n",
                argv[0]);
        return 1;
    }

    memset(&req, 0, sizeof(req));
    req.kind = CMD_START;
    strncpy(req.container_id, argv[2], sizeof(req.container_id) - 1);
    strncpy(req.rootfs, argv[3], sizeof(req.rootfs) - 1);
    strncpy(req.command, argv[4], sizeof(req.command) - 1);
    req.soft_limit_bytes = DEFAULT_SOFT_LIMIT;
    req.hard_limit_bytes = DEFAULT_HARD_LIMIT;

    if (parse_optional_flags(&req, argc, argv, 5) != 0)
        return 1;

    return send_control_request(&req);
}

static int cmd_run(int argc, char *argv[])
{
    control_request_t req;

    if (argc < 5) {
        fprintf(stderr,
                "Usage: %s run <id> <container-rootfs> <command> [--soft-mib N] [--hard-mib N] [--nice N]\n",
                argv[0]);
        return 1;
    }

    memset(&req, 0, sizeof(req));
    req.kind = CMD_RUN;
    strncpy(req.container_id, argv[2], sizeof(req.container_id) - 1);
    strncpy(req.rootfs, argv[3], sizeof(req.rootfs) - 1);
    strncpy(req.command, argv[4], sizeof(req.command) - 1);
    req.soft_limit_bytes = DEFAULT_SOFT_LIMIT;
    req.hard_limit_bytes = DEFAULT_HARD_LIMIT;

    if (parse_optional_flags(&req, argc, argv, 5) != 0)
        return 1;

    return send_control_request(&req);
}

static int cmd_ps(void)
{
    control_request_t req;
    memset(&req, 0, sizeof(req));
    req.kind = CMD_PS;
    return send_control_request(&req);
}

static int cmd_logs(int argc, char *argv[])
{
    control_request_t req;

    if (argc < 3) {
        fprintf(stderr, "Usage: %s logs <id>\n", argv[0]);
        return 1;
    }

    memset(&req, 0, sizeof(req));
    req.kind = CMD_LOGS;
    strncpy(req.container_id, argv[2], sizeof(req.container_id) - 1);
    return send_control_request(&req);
}

static int cmd_stop(int argc, char *argv[])
{
    control_request_t req;

    if (argc < 3) {
        fprintf(stderr, "Usage: %s stop <id>\n", argv[0]);
        return 1;
    }

    memset(&req, 0, sizeof(req));
    req.kind = CMD_STOP;
    strncpy(req.container_id, argv[2], sizeof(req.container_id) - 1);
    return send_control_request(&req);
}

/* ===============================================================
 * Main
 * =============================================================== */

int main(int argc, char *argv[])
{
    if (argc < 2) { usage(argv[0]); return 1; }

    if (strcmp(argv[1], "supervisor") == 0) {
        if (argc < 3) {
            fprintf(stderr, "Usage: %s supervisor <base-rootfs>\n", argv[0]);
            return 1;
        }
        return run_supervisor(argv[2]);
    }

    if (strcmp(argv[1], "start") == 0) return cmd_start(argc, argv);
    if (strcmp(argv[1], "run")   == 0) return cmd_run(argc, argv);
    if (strcmp(argv[1], "ps")    == 0) return cmd_ps();
    if (strcmp(argv[1], "logs")  == 0) return cmd_logs(argc, argv);
    if (strcmp(argv[1], "stop")  == 0) return cmd_stop(argc, argv);

    usage(argv[0]);
    return 1;
}
