/*
 * runtime/process.c — asynchronous child process execution with capture.
 */

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/timerfd.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "runtime/process.h"

#define PROCESS_KILL_GRACE_MS 2000

extern char** environ;

static bool child_env_allowed(const char* entry) {
    const char* equals = strchr(entry, '=');
    size_t name_len = equals != NULL ? (size_t)(equals - entry) : strlen(entry);
    static const char* exact[] = {"PATH", "HOME", "USER", "LOGNAME", "SHELL",
                                  "TMPDIR", "TZ", "TERM", "COLORTERM", "PWD"};
    for (size_t i = 0; i < sizeof(exact) / sizeof(exact[0]); i++) {
        if (strlen(exact[i]) == name_len && strncmp(entry, exact[i], name_len) == 0) {
            return true;
        }
    }
    return (name_len >= 4 && strncmp(entry, "LANG", 4) == 0) ||
           (name_len >= 3 && strncmp(entry, "LC_", 3) == 0);
}

static char** build_child_env(void) {
    size_t count = 0;
    if (environ != NULL) {
        for (char** p = environ; *p != NULL; p++) {
            if (child_env_allowed(*p)) {
                count++;
            }
        }
    }
    char** env = calloc(count + 1, sizeof(*env));
    if (env == NULL) {
        return NULL;
    }
    size_t i = 0;
    if (environ != NULL) {
        for (char** p = environ; *p != NULL; p++) {
            if (child_env_allowed(*p)) {
                env[i++] = *p;
            }
        }
    }
    return env;
}

struct ProcessTask {
    EventLoop* loop; /* borrowed */
    pid_t pid;
    int out_fd;
    int err_fd;
    int timer_fd;
    LoopWatcher out_watcher;
    LoopWatcher err_watcher;
    LoopWatcher timer_watcher;
    bool out_open;
    bool err_open;
    bool child_reaped;
    bool terminating;
    bool kill_sent;
    bool result_taken;
    int wait_status;
    int error;
    size_t output_cap;
    ProcessResult result;
};

static bool process_group_may_be_alive(const ProcessTask* task) {
    return task != NULL && task->pid > 0 &&
           (!task->child_reaped || task->out_open || task->err_open);
}

static void signal_process_group(ProcessTask* task, int sig) {
    if (task == NULL || task->pid <= 0) {
        return;
    }
    /* The original shell may already be reaped while a background child in
     * its process group still owns stdout/stderr. The group remains signalable
     * through the original pgid even though its leader has exited. */
    if (kill(-task->pid, sig) != 0 && !task->child_reaped) {
        (void)kill(task->pid, sig);
    }
}

static int arm_timer(ProcessTask* task, int64_t timeout_ms) {
    if (task == NULL || task->timer_fd < 0) {
        return AGENT_ERR_PROCESS;
    }
    struct itimerspec spec = {0};
    if (timeout_ms > 0) {
        spec.it_value.tv_sec = timeout_ms / 1000;
        spec.it_value.tv_nsec = (timeout_ms % 1000) * 1000000;
        if (spec.it_value.tv_sec == 0 && spec.it_value.tv_nsec == 0) {
            spec.it_value.tv_nsec = 1;
        }
    }
    return timerfd_settime(task->timer_fd, 0, &spec, NULL) == 0 ? AGENT_OK : AGENT_ERR_PROCESS;
}

static void close_stream(ProcessTask* task, bool stdout_stream) {
    int* fd = stdout_stream ? &task->out_fd : &task->err_fd;
    bool* open = stdout_stream ? &task->out_open : &task->err_open;
    if (!*open) {
        return;
    }
    event_loop_remove(task->loop, *fd);
    close(*fd);
    *fd = -1;
    *open = false;
}

static void append_capped(ProcessTask* task, const char* data, size_t n) {
    if (task->result.output_capped) {
        return;
    }
    if (task->output_cap > 0) {
        size_t room =
            task->result.out.len < task->output_cap ? task->output_cap - task->result.out.len : 0;
        if (n > room) {
            n = room;
            task->result.output_capped = true;
        }
    }
    if (n > 0 && string_append_n(&task->result.out, data, n) != AGENT_OK) {
        task->error = AGENT_ERR_OOM;
        task->result.output_capped = true; /* keep draining without growing */
    }
}

static void drain_stream(ProcessTask* task, bool stdout_stream) {
    int fd = stdout_stream ? task->out_fd : task->err_fd;
    bool open = stdout_stream ? task->out_open : task->err_open;
    if (!open) {
        return;
    }

    char buf[4096];
    for (;;) {
        ssize_t n = read(fd, buf, sizeof(buf));
        if (n > 0) {
            append_capped(task, buf, (size_t)n);
            continue;
        }
        if (n == 0) {
            close_stream(task, stdout_stream);
            return;
        }
        if (errno == EINTR) {
            continue;
        }
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            return;
        }
        close_stream(task, stdout_stream);
        return;
    }
}

static void pipe_cb(EventLoop* loop, int fd, uint32_t events, void* ud) {
    (void)loop;
    ProcessTask* task = ud;
    if (task == NULL) {
        return;
    }
    bool stdout_stream = fd == task->out_fd;
    if (!stdout_stream && fd != task->err_fd) {
        return;
    }
    if ((events & (EPOLLIN | EPOLLHUP | EPOLLERR)) != 0) {
        drain_stream(task, stdout_stream);
    }
}

static void begin_termination(ProcessTask* task, bool timed_out) {
    if (!process_group_may_be_alive(task) || task->terminating) {
        return;
    }
    task->terminating = true;
    if (timed_out) {
        task->result.timed_out = true;
    }
    signal_process_group(task, SIGTERM);
    if (arm_timer(task, PROCESS_KILL_GRACE_MS) != AGENT_OK) {
        signal_process_group(task, SIGKILL);
        task->kill_sent = true;
    }
}

static void timer_cb(EventLoop* loop, int fd, uint32_t events, void* ud) {
    (void)loop;
    (void)events;
    ProcessTask* task = ud;
    if (task == NULL || fd != task->timer_fd) {
        return;
    }
    uint64_t expirations;
    while (read(fd, &expirations, sizeof(expirations)) < 0 && errno == EINTR) {
    }
    if (!task->terminating) {
        begin_termination(task, true);
    } else if (process_group_may_be_alive(task) && !task->kill_sent) {
        signal_process_group(task, SIGKILL);
        task->kill_sent = true;
        (void)arm_timer(task, 0);
    }
}

static int set_nonblocking(int fd) {
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags < 0 || fcntl(fd, F_SETFL, flags | O_NONBLOCK) != 0) {
        return AGENT_ERR_PROCESS;
    }
    return AGENT_OK;
}

int process_start(EventLoop* loop, const char* cwd, char* const argv[], int64_t timeout_ms,
                  size_t output_cap, ProcessTask** task_out) {
    if (loop == NULL || argv == NULL || argv[0] == NULL || task_out == NULL) {
        return AGENT_ERR_PROCESS;
    }
    *task_out = NULL;

    char** child_env = build_child_env();
    if (child_env == NULL) {
        return AGENT_ERR_OOM;
    }

    ProcessTask* task = calloc(1, sizeof(ProcessTask));
    if (task == NULL) {
        free(child_env);
        return AGENT_ERR_OOM;
    }
    task->loop = loop;
    task->pid = -1;
    task->out_fd = -1;
    task->err_fd = -1;
    task->timer_fd = -1;
    task->wait_status = 0;
    task->output_cap = output_cap;
    task->result.exit_code = -2;
    task->result.out = string_new();

    int out_pipe[2] = {-1, -1};
    int err_pipe[2] = {-1, -1};
    if (pipe2(out_pipe, O_CLOEXEC) != 0 || pipe2(err_pipe, O_CLOEXEC) != 0) {
        if (out_pipe[0] >= 0) {
            close(out_pipe[0]);
            close(out_pipe[1]);
        }
        if (err_pipe[0] >= 0) {
            close(err_pipe[0]);
            close(err_pipe[1]);
        }
        process_task_free(task);
        free(child_env);
        return AGENT_ERR_PROCESS;
    }

    /* Fork happens on the EventLoop thread before this function returns.
     * The child performs only process setup and exec; no worker thread is
     * involved, so Agent state remains single-thread-owned. */
    pid_t pid = fork();
    if (pid == 0) {
        (void)setpgid(0, 0);
        (void)dup2(out_pipe[1], STDOUT_FILENO);
        (void)dup2(err_pipe[1], STDERR_FILENO);
        close(out_pipe[0]);
        close(out_pipe[1]);
        close(err_pipe[0]);
        close(err_pipe[1]);
        if (cwd != NULL && chdir(cwd) != 0) {
            static const char message[] = "cagent: unable to enter working directory\n";
            (void)write(STDERR_FILENO, message, sizeof(message) - 1);
            _exit(126);
        }
        execve(argv[0], argv, child_env);
        _exit(127);
    }

    free(child_env);
    close(out_pipe[1]);
    close(err_pipe[1]);
    if (pid < 0) {
        close(out_pipe[0]);
        close(err_pipe[0]);
        process_task_free(task);
        return AGENT_ERR_PROCESS;
    }

    task->pid = pid;
    task->out_fd = out_pipe[0];
    task->err_fd = err_pipe[0];
    task->out_open = true;
    task->err_open = true;

    if (set_nonblocking(task->out_fd) != AGENT_OK || set_nonblocking(task->err_fd) != AGENT_OK) {
        process_task_free(task);
        return AGENT_ERR_PROCESS;
    }

    task->timer_fd = timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK | TFD_CLOEXEC);
    if (task->timer_fd < 0) {
        process_task_free(task);
        return AGENT_ERR_PROCESS;
    }

    task->out_watcher = (LoopWatcher){
        .fd = task->out_fd,
        .events = EPOLLIN | EPOLLHUP | EPOLLERR,
        .cb = pipe_cb,
        .ud = task,
    };
    task->err_watcher = (LoopWatcher){
        .fd = task->err_fd,
        .events = EPOLLIN | EPOLLHUP | EPOLLERR,
        .cb = pipe_cb,
        .ud = task,
    };
    task->timer_watcher = (LoopWatcher){
        .fd = task->timer_fd,
        .events = EPOLLIN,
        .cb = timer_cb,
        .ud = task,
    };

    if (event_loop_add(loop, &task->out_watcher) != AGENT_OK ||
        event_loop_add(loop, &task->err_watcher) != AGENT_OK ||
        event_loop_add(loop, &task->timer_watcher) != AGENT_OK ||
        arm_timer(task, timeout_ms) != AGENT_OK) {
        process_task_free(task);
        return AGENT_ERR_PROCESS;
    }

    *task_out = task;
    (void)event_loop_wakeup(loop);
    return AGENT_OK;
}

int process_poll(ProcessTask* task, ProcessResult* result, bool* done) {
    if (task == NULL || result == NULL || done == NULL || task->result_taken) {
        return AGENT_ERR_PROCESS;
    }
    *done = false;

    if (!task->child_reaped) {
        int status = 0;
        pid_t rc;
        do {
            rc = waitpid(task->pid, &status, WNOHANG);
        } while (rc < 0 && errno == EINTR);
        if (rc == task->pid) {
            task->child_reaped = true;
            task->wait_status = status;
        } else if (rc < 0 && errno == ECHILD) {
            task->child_reaped = true;
            task->wait_status = 0;
        } else if (rc < 0) {
            task->error = AGENT_ERR_PROCESS;
        }
    }

    /* A reaped child may have left unread bytes in the pipes. */
    if (task->child_reaped) {
        drain_stream(task, true);
        drain_stream(task, false);
    }

    if (!task->child_reaped || task->out_open || task->err_open) {
        return task->error != AGENT_OK ? task->error : AGENT_OK;
    }

    if (task->timer_fd >= 0) {
        event_loop_remove(task->loop, task->timer_fd);
        close(task->timer_fd);
        task->timer_fd = -1;
    }

    if (WIFEXITED(task->wait_status)) {
        task->result.exit_code = WEXITSTATUS(task->wait_status);
    } else if (WIFSIGNALED(task->wait_status)) {
        task->result.exit_code = 128 + WTERMSIG(task->wait_status);
    } else {
        task->result.exit_code = -2;
    }

    *result = task->result; /* move */
    task->result.out = string_new();
    task->result_taken = true;
    *done = true;
    return task->error != AGENT_OK ? task->error : AGENT_OK;
}

void process_cancel(ProcessTask* task) {
    begin_termination(task, false);
    if (task != NULL && task->loop != NULL) {
        (void)event_loop_wakeup(task->loop);
    }
}

void process_task_free(ProcessTask* task) {
    if (task == NULL) {
        return;
    }

    /* Kill descendants before closing our pipe ends. A reaped shell can
     * leave background children in its process group holding those pipes. */
    if (process_group_may_be_alive(task)) {
        signal_process_group(task, SIGKILL);
    }
    if (task->out_open) {
        close_stream(task, true);
    }
    if (task->err_open) {
        close_stream(task, false);
    }
    if (task->timer_fd >= 0) {
        event_loop_remove(task->loop, task->timer_fd);
        close(task->timer_fd);
        task->timer_fd = -1;
    }

    if (task->pid > 0 && !task->child_reaped) {
        int status = 0;
        pid_t rc;
        do {
            rc = waitpid(task->pid, &status, 0);
        } while (rc < 0 && errno == EINTR);
        if (rc == task->pid) {
            task->child_reaped = true;
            task->wait_status = status;
        }
    }

    if (!task->result_taken) {
        process_result_free(&task->result);
    }
    free(task);
}

int process_run(const char* cwd, char* const argv[], int64_t timeout_ms, size_t output_cap,
                ProcessResult* result) {
    if (result == NULL) {
        return AGENT_ERR_PROCESS;
    }
    memset(result, 0, sizeof(*result));
    result->exit_code = -2;

    EventLoop* loop = event_loop_new();
    if (loop == NULL) {
        return AGENT_ERR_IO;
    }
    ProcessTask* task = NULL;
    int rc = process_start(loop, cwd, argv, timeout_ms, output_cap, &task);
    if (rc != AGENT_OK) {
        event_loop_free(loop);
        return rc;
    }

    bool done = false;
    while (!done) {
        (void)event_loop_wait(loop, 50);
        rc = process_poll(task, result, &done);
        if (rc != AGENT_OK) {
            break;
        }
    }

    process_task_free(task);
    event_loop_free(loop);
    return rc;
}

void process_result_free(ProcessResult* result) {
    if (result == NULL) {
        return;
    }
    string_free(&result->out);
    result->exit_code = -2;
}
