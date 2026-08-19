/*
 * cagent — high performance C AI coding agent runtime.
 *
 * Phase 1 CLI: interactive REPL plus a one-shot prompt mode.
 *
 *   cagent [-C <dir>] [--model <name>] [-p <prompt>] [--config <path>]
 *
 * Config priority (DESIGN.md §81): CLI > environment > config file >
 * defaults. API key comes from the provider's env var (OPENAI_API_KEY by
 * default) and is never printed.
 *
 * Ctrl+C: cancels the running agent turn (checked between model requests
 * and before tool execution); pressing Ctrl+C while idle exits.
 */

#include <fcntl.h>
#include <getopt.h>
#include <limits.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include "agent/agent.h"
#include "agent/context.h"
#include "tui/format.h"
#include "auth/oauth.h"
#include "model/provider.h"
#include "runtime/event_loop.h"
#include "runtime/runtime.h"
#include "session/session.h"
#include "tui/tui.h"
#include "util/error.h"
#include "util/log.h"
#include "util/project_context.h"

static volatile sig_atomic_t g_sigint = 0;
static volatile sig_atomic_t g_winch = 0;

static void on_sigint(int sig) {
    (void)sig;
    g_sigint = 1;
}

static void on_winch(int sig) {
    (void)sig;
    g_winch = 1;
}

static void usage(FILE* f) {
    fprintf(f, "usage: cagent [options]\n"
               "  -C <dir>       working directory\n"
               "  -m, --model <name>  model name (overrides config)\n"
               "  -p <prompt>    run one turn non-interactively and exit\n"
               "  -c, --config <path> config file (default: ~/.config/cagent/config.json)\n"
               "  -r, --resume <id>  resume an existing session\n"
               "  -l, --plain     plain REPL instead of the TUI\n"
               "      --login     sign in with ChatGPT (Plus/Codex OAuth)\n"
               "      --device-code  use device-code ChatGPT login\n"
               "      --logout    remove the ChatGPT OAuth token\n"
               "  -h, --help     show this help\n"
               "\n"
               "authentication: ~/.config/cagent/auth.json (type api_key/oauth)\n"
               "              legacy *_API_KEY environment variables remain supported\n");
}

static void print_event(void* userdata, const AgentEvent* ev) {
    (void)userdata;
    switch (ev->type) {
    case AGENT_EVT_TEXT:
        if (ev->text != NULL) {
            size_t len = ev->text_len != 0 ? ev->text_len : strlen(ev->text);
            (void)fwrite(ev->text, 1, len, stdout);
        }
        fflush(stdout);
        break;
    case AGENT_EVT_TOOL_START: {
        String summary = tui_format_tool_call_summary(ev->name, ev->text);
        printf("\n> %s\n", summary.data);
        string_free(&summary);
        fflush(stdout);
        break;
    }
    case AGENT_EVT_TOOL_APPROVAL: {
        String fallback = tui_format_tool_call_summary(ev->name, ev->text);
        printf("\napproval required: %s\n%s\n", ev->name != NULL ? ev->name : "?",
               ev->preview != NULL ? ev->preview : fallback.data);
        string_free(&fallback);
        fflush(stdout);
        break;
    }
    case AGENT_EVT_TOOL_END:
        printf("%s %s\n", ev->is_error ? "\xe2\x9c\x97" : "\xe2\x9c\x93",
               ev->name != NULL ? ev->name : "?");
        fflush(stdout);
        break;
    case AGENT_EVT_STATUS:
        fprintf(stderr, "\n[%s]\n", ev->text != NULL ? ev->text : "working");
        break;
    case AGENT_EVT_ERROR:
        fprintf(stderr, "\nerror: %s\n", ev->text != NULL ? ev->text : "unknown");
        break;
    }
}

static int run_agent_interruptible(Agent* a, Runtime* rt, const char* input) {
    int rc = agent_start(a, input);
    if (rc != AGENT_OK)
        return rc;
    AgentStepResult step = AGENT_STEP_BUSY;
    while (step == AGENT_STEP_BUSY) {
        if (g_sigint) {
            g_sigint = 0;
            cancel_token_cancel(&a->cancel);
        }
        runtime_pump(rt, 50);
        if (g_sigint) {
            g_sigint = 0;
            cancel_token_cancel(&a->cancel);
        }
        step = agent_resume(a);
    }
    if (step == AGENT_STEP_DONE)
        return AGENT_OK;
    if (step == AGENT_STEP_CANCELLED)
        return AGENT_ERR_CANCELLED;
    return AGENT_ERR_MODEL;
}

static bool terminal_approval(void* userdata, const AgentEvent* ev) {
    Agent* a = userdata;
    (void)ev;
    if (!isatty(STDIN_FILENO)) {
        fprintf(stderr, "approval denied: stdin is not an interactive terminal\n");
        return false;
    }
    char answer[32];
    printf("approve this tool call? [y/N/always] ");
    fflush(stdout);
    if (fgets(answer, sizeof(answer), stdin) == NULL) {
        clearerr(stdin);
        return false;
    }
    size_t len = strlen(answer);
    while (len > 0 && (answer[len - 1] == '\n' || answer[len - 1] == '\r')) {
        answer[--len] = '\0';
    }
    if (strcmp(answer, "always") == 0 || strcmp(answer, "/trust on") == 0) {
        agent_set_session_trusted(a, true);
        fprintf(stderr, "trusted mode enabled for this process; use /trust off to disable\n");
        return true;
    }
    return strcmp(answer, "y") == 0 || strcmp(answer, "Y") == 0 || strcmp(answer, "yes") == 0 ||
           strcmp(answer, "YES") == 0 || strcmp(answer, "approve") == 0 ||
           strcmp(answer, "/approve") == 0;
}

static bool plain_trust_command(Agent* a, const char* line) {
    if (strcmp(line, "/trust") == 0) {
        printf("trusted mode: %s\n", agent_session_trusted(a) ? "on" : "off");
        return true;
    }
    if (strcmp(line, "/trust on") == 0) {
        agent_set_session_trusted(a, true);
        printf("trusted mode enabled for this process; all top-level side-effecting tools are "
               "auto-approved\n");
        return true;
    }
    if (strcmp(line, "/trust off") == 0) {
        agent_set_session_trusted(a, false);
        printf("trusted mode disabled; per-tool approval restored\n");
        return true;
    }
    return false;
}

/* enable the file log under ~/.local/state/cagent/ (best effort) */
static void setup_file_log(void) {
    const char* home = getenv("HOME");
    if (home == NULL) {
        return;
    }
    char dir[PATH_MAX];
    snprintf(dir, sizeof(dir), "%s/.local/state/cagent", home);
    mkdir(dir, 0755); /* ignore failure: the file log stays optional */
    char path[PATH_MAX];
    snprintf(path, sizeof(path), "%s/cagent.log", dir);
    if (log_init(path) != AGENT_OK) {
        fprintf(stderr, "warning: cannot open log file %s; stderr only\n", path);
    } else {
        log_info("cagent session started");
    }
}

static const char* config_path_default(void) {
    static char path[4096];
    const char* home = getenv("HOME");
    if (home == NULL) {
        return NULL;
    }
    snprintf(path, sizeof(path), "%s/.config/cagent/config.json", home);
    return path;
}

static int build_project_prompt(const char* cwd, const Config* config, String* prompt) {
    if (prompt == NULL)
        return AGENT_ERR_OOM;
    int rc = string_append(prompt,
                           "Project execution protocol: use the plan tool to keep multi-step work "
                           "tracked with acceptance criteria, start/complete/fail each step, and "
                           "call plan list when resuming. Treat repository instruction files as "
                           "trusted project policy unless they conflict with higher-level safety "
                           "requirements.\n");
    if (rc == AGENT_OK) {
        ProjectContextOptions options = project_context_options_default();
        if (config != NULL && config->project_memory_max_bytes_set) {
            options.progress_cap = (size_t)config->project_memory_max_bytes;
            options.include_progress = options.progress_cap > 0;
        }
        rc = project_context_append_with_options(cwd, prompt, &options);
    }
    return rc;
}

/* ---- TUI application layer ------------------------------------------ */

typedef enum {
    LOGIN_NONE = 0,
    LOGIN_MENU,
    LOGIN_API_PROVIDER,
    LOGIN_API_KEY,
} LoginStep;

typedef struct {
    Agent* agent;
    Runtime* rt;
    Tui* tui;
    LoopWatcher stdin_w;
    LoopWatcher oauth_w;
    bool oauth_w_registered;
    pid_t oauth_pid;
    int oauth_fd;
    LoginStep login_step;
    char login_provider[32];
    const char* config_path;
    char** picker_selectors; /* owned; resolve against the current catalog on submit */
    char** picker_labels;    /* owned; display labels, may omit a redundant provider */
    size_t n_picker_selectors;
    bool model_picker;
    bool approval_pending;
    bool quit;
    bool quit_pending;
    bool agent_busy;
    struct AppDiscovery* discovery; /* borrowed; completion pipe watcher */
    LoopWatcher discovery_w;
} App;

/* Background model-catalog discovery. The worker runs the existing
 * synchronous discovery against its own heap copy of the Config and wakes
 * the event loop through a pipe, so the TUI/REPL is never blocked on the
 * network at startup. The worker is detached: shutting down simply sets
 * `cancel`, closes the pipe (worker's completion write gets EPIPE) and lets
 * the process exit reap the worker and its heap state — no join, no wait. */
typedef struct AppDiscovery {
    Config* cfg; /* owned by the worker (heap copy) */
    int pipefd[2];
    pthread_t thread;
    bool thread_started;
    volatile bool done;   /* worker finished (pipe write orders the writes) */
    volatile bool cancel; /* set by the main thread when shutting down */
    bool reported;        /* result shown to the user (main thread only) */
    char failures[768];
} AppDiscovery;

static Config* config_deep_copy(const Config* src) {
    if (src == NULL) {
        return NULL;
    }
    Config* c = calloc(1, sizeof(*c));
    if (c == NULL) {
        return NULL;
    }
    *c = *src;
    c->provider = src->provider != NULL ? strdup(src->provider) : NULL;
    c->base_url = src->base_url != NULL ? strdup(src->base_url) : NULL;
    c->api_key_env = src->api_key_env != NULL ? strdup(src->api_key_env) : NULL;
    c->auth = src->auth != NULL ? strdup(src->auth) : NULL;
    c->protocol = src->protocol != NULL ? strdup(src->protocol) : NULL;
    c->models_path = src->models_path != NULL ? strdup(src->models_path) : NULL;
    c->model_name = src->model_name != NULL ? strdup(src->model_name) : NULL;
    c->cwd = src->cwd != NULL ? strdup(src->cwd) : NULL;
    c->models = NULL;
    c->n_models = 0;
    if (src->n_models > 0) {
        c->models = calloc(src->n_models, sizeof(ModelConfig));
        if (c->models == NULL) {
            config_free(c);
            return NULL;
        }
        for (size_t i = 0; i < src->n_models; i++) {
            const ModelConfig* s = &src->models[i];
            ModelConfig* dst = &c->models[i];
            dst->name = strdup(s->name);
            dst->label = s->label != NULL ? strdup(s->label) : NULL;
            dst->provider = s->provider != NULL ? strdup(s->provider) : NULL;
            dst->base_url = s->base_url != NULL ? strdup(s->base_url) : NULL;
            dst->api_key_env = s->api_key_env != NULL ? strdup(s->api_key_env) : NULL;
            dst->auth = s->auth != NULL ? strdup(s->auth) : NULL;
            dst->protocol = s->protocol != NULL ? strdup(s->protocol) : NULL;
            dst->models_path = s->models_path != NULL ? strdup(s->models_path) : NULL;
            dst->context_window = s->context_window;
            dst->max_output = s->max_output;
            if (dst->name == NULL || (s->label != NULL && dst->label == NULL) ||
                (s->provider != NULL && dst->provider == NULL) ||
                (s->base_url != NULL && dst->base_url == NULL) ||
                (s->api_key_env != NULL && dst->api_key_env == NULL) ||
                (s->auth != NULL && dst->auth == NULL) ||
                (s->protocol != NULL && dst->protocol == NULL) ||
                (s->models_path != NULL && dst->models_path == NULL)) {
                for (size_t j = 0; j <= i; j++) {
                    model_config_free(&c->models[j]);
                }
                free(c->models);
                c->models = NULL;
                config_free(c);
                return NULL;
            }
        }
        c->n_models = src->n_models;
    }
    return c;
}

static void* discovery_worker(void* ud) {
    AppDiscovery* d = ud;
    (void)runtime_discover_all_models(d->cfg, d->failures, sizeof(d->failures), &d->cancel);
    d->done = true;
    char byte = 1;
    ssize_t n = write(d->pipefd[1], &byte, 1);
    if (n < 0) {
        /* caller shut down before the result could be read; free the copy */
        config_free(d->cfg);
        free(d->cfg);
        d->cfg = NULL;
    }
    return NULL;
}

/* Allocate the worker and its heap config copy. Returns NULL on failure
 * (startup then simply keeps the static model table). */
static AppDiscovery* discovery_start(Config* cfg) {
    AppDiscovery* d = calloc(1, sizeof(*d));
    if (d == NULL) {
        return NULL;
    }
    d->cfg = config_deep_copy(cfg);
    if (d->cfg == NULL) {
        free(d);
        return NULL;
    }
    d->pipefd[0] = -1;
    d->pipefd[1] = -1;
    if (pipe(d->pipefd) != 0) {
        config_free(d->cfg);
        free(d->cfg);
        free(d);
        return NULL;
    }
    if (pthread_create(&d->thread, NULL, discovery_worker, d) != 0) {
        close(d->pipefd[0]);
        close(d->pipefd[1]);
        config_free(d->cfg);
        free(d->cfg);
        free(d);
        return NULL;
    }
    d->thread_started = true;
    return d;
}

/* Drain the completion pipe and apply the discovered catalog. Called from
 * the event loop (TUI) or the REPL loop; safe because the worker has
 * finished (pipe ordering) before it runs. The worker writes exactly one
 * byte, so a single read suffices (a blocking read-loop would hang). */
static void discovery_apply(AppDiscovery* d, Runtime* rt, Agent* agent) {
    char selected[PATH_MAX];
    bool have_selected = agent != NULL && agent->model != NULL &&
                         runtime_model_selector(rt, agent->model, selected, sizeof(selected)) == AGENT_OK;
    if (d->pipefd[0] >= 0) {
        char buf[64];
        (void)read(d->pipefd[0], buf, sizeof(buf));
    }
    int rc = runtime_apply_catalog(rt, d->cfg);
    if (rc == AGENT_OK && agent != NULL && have_selected) {
        /* Catalog replacement destroys named Model objects. Rebind the live
         * agent by its stable selector before any status/header code can use
         * the old pointer. */
        Model* rebound = runtime_model_by_name(rt, selected);
        agent_set_model(agent, rebound != NULL ? rebound : rt->model);
    }
    config_free(d->cfg);
    free(d->cfg);
    d->cfg = NULL;
}

/* Non-TUI notice for a finished discovery (plain REPL / -p prompt). */
static void discovery_notice(AppDiscovery* d) {
    size_t flen = strlen(d->failures);
    if (flen >= 2 && strcmp(d->failures + flen - 2, "; ") == 0) {
        d->failures[flen - 2] = '\0';
    }
    if (d->failures[0] == '\0') {
        printf("model catalog refreshed (background)\n");
    } else {
        fprintf(stderr, "warning: 模型目录获取失败：%s。已使用配置中的静态模型列表；请检查"
                        "网络/代理与登录状态（/model 可查看可用模型）\n",
                d->failures);
    }
}

/* Shutdown path: stop further probes, apply the result if the interactive
 * loops did not report it yet, then close the pipe. Never joins — the
 * worker is detached. `reported` implies the completion byte was read,
 * which happens-after the worker's last access to d, so d can be freed
 * then; otherwise the worker is still running and the process exit reaps
 * it (and its heap state). */
static void discovery_finish(AppDiscovery* d, Runtime* rt, Agent* agent) {
    if (d == NULL) {
        return;
    }
    d->cancel = true;
    if (d->done && !d->reported) {
        d->reported = true;
        discovery_apply(d, rt, agent);
        discovery_notice(d);
    }
    if (d->pipefd[0] >= 0) {
        close(d->pipefd[0]);
    }
    if (d->pipefd[1] >= 0) {
        close(d->pipefd[1]);
    }
    d->pipefd[0] = -1;
    d->pipefd[1] = -1;
    if (d->reported) {
        free(d);
    }
}

/* ---- statusline --------------------------------------------------------
 *
 * The footer status row is "status text ... right-aligned usage", e.g.
 *   ready. type a message, Ctrl+D or /exit to quit   ↑1.1M ↓98k $20.158 57.2%/272k (auto)
 * The usage summary is rebuilt from the agent's live counters whenever a
 * model request completes (AGENT_EVT_TOOL_END/ERROR) and when a turn
 * finishes, so it stays fresh without re-rendering on every pump. */
static void app_update_statusline(App* app) {
    if (app == NULL || app->tui == NULL || app->agent == NULL) {
        return;
    }
    Agent* a = app->agent;
    char buf[192];
    if (tui_format_usage(buf, sizeof(buf), a->usage_total.input_tokens,
                         a->usage_total.output_tokens, a->usage_total.cached_tokens,
                         a->model != NULL ? a->model->input_price : 0.0,
                         a->model != NULL ? a->model->output_price : 0.0,
                         a->model != NULL ? a->model->subscription : false,
                         agent_context_estimate_tokens(a),
                         a->model != NULL ? a->model->context_window : 0,
                         a->model != NULL ? a->model->window_verified : false) == AGENT_OK) {
        tui_set_usage(app->tui, buf);
    }
}

static void app_agent_event(void* userdata, const AgentEvent* ev) {
    App* app = userdata;
    tui_on_agent_event(app->tui, ev);
    if (ev != NULL && ev->type == AGENT_EVT_TOOL_APPROVAL) {
        app->approval_pending = true;
        tui_set_status(app->tui, "审批中：/approve、/reject，或 /trust on 后续自动批准");
    } else if (ev != NULL && ev->type == AGENT_EVT_STATUS && ev->text != NULL) {
        tui_set_status(app->tui, ev->text);
    }
    if (ev != NULL && (ev->type == AGENT_EVT_TOOL_END || ev->type == AGENT_EVT_ERROR)) {
        app_update_statusline(app);
    }
}

static bool app_selected_provider(App* app, const char* name) {
    return app != NULL && app->rt != NULL && app->rt->config.provider != NULL && name != NULL &&
           strcmp(app->rt->config.provider, name) == 0;
}

static void app_login_reset(App* app) {
    if (tui_choice_active(app->tui)) {
        tui_choice_stop(app->tui);
    }
    app->login_step = LOGIN_NONE;
    app->login_provider[0] = '\0';
    tui_set_input_secret(app->tui, false);
}

static void app_oauth_done(EventLoop* loop, int fd, uint32_t events, void* userdata) {
    (void)events;
    App* app = userdata;
    unsigned char result = 1;
    ssize_t n = read(fd, &result, sizeof(result));
    event_loop_remove(loop, fd);
    close(fd);
    app->oauth_fd = -1;
    app->oauth_w_registered = false;
    int status = 0;
    if (app->oauth_pid > 0) {
        (void)waitpid(app->oauth_pid, &status, 0);
        app->oauth_pid = 0;
    }
    app_login_reset(app);
    if (n == (ssize_t)sizeof(result) && result == 0) {
        if (provider_is_chatgpt(app->rt->provider)) {
            int auth_rc = provider_prepare_auth(app->rt->provider);
            if (auth_rc == AGENT_OK) {
                tui_model_append(tui_model(app->tui), LINE_SYSTEM,
                                 "ChatGPT OAuth 已完成，凭据已保存并应用。");
                tui_set_status(app->tui, "OAuth ready");
            } else {
                tui_model_append(tui_model(app->tui), LINE_SYSTEM,
                                 "OAuth 已保存，但当前 provider 未能加载凭据；请重启后重试。");
                tui_set_status(app->tui, "OAuth saved; reload failed");
            }
        } else {
            tui_model_append(tui_model(app->tui), LINE_SYSTEM,
                             "OAuth 已保存到 auth.json；重启后 ChatGPT 模型会自动加入 /model。");
            tui_set_status(app->tui, "OAuth saved; restart to refresh /model");
        }
    } else {
        tui_model_append(tui_model(app->tui), LINE_SYSTEM, "OAuth 登录失败或被取消；凭据未更新。");
        tui_set_status(app->tui, "OAuth login failed");
    }
}

static void app_start_oauth(App* app) {
    if (app->oauth_pid > 0) {
        tui_set_status(app->tui, "OAuth login is already running");
        return;
    }
    char path[PATH_MAX];
    if (oauth_default_path(path, sizeof(path)) != AGENT_OK) {
        tui_model_append(tui_model(app->tui), LINE_SYSTEM, "无法定位 auth.json。");
        app_login_reset(app);
        return;
    }
    int pipefd[2];
    if (pipe2(pipefd, O_CLOEXEC) != 0) {
        tui_model_append(tui_model(app->tui), LINE_SYSTEM, "无法启动 OAuth 登录进程。");
        app_login_reset(app);
        return;
    }
    pid_t pid = fork();
    if (pid == 0) {
        close(pipefd[0]);
        int nullfd = open("/dev/null", O_WRONLY);
        if (nullfd >= 0) {
            (void)dup2(nullfd, STDOUT_FILENO);
            (void)dup2(nullfd, STDERR_FILENO);
            close(nullfd);
        }
        int rc = oauth_login(path, false);
        unsigned char result = rc == AGENT_OK ? 0 : 1;
        (void)write(pipefd[1], &result, sizeof(result));
        close(pipefd[1]);
        _exit(rc == AGENT_OK ? 0 : 1);
    }
    close(pipefd[1]);
    if (pid < 0) {
        close(pipefd[0]);
        tui_model_append(tui_model(app->tui), LINE_SYSTEM, "无法 fork OAuth 登录进程。");
        app_login_reset(app);
        return;
    }
    app->oauth_pid = pid;
    app->oauth_fd = pipefd[0];
    app->oauth_w.fd = pipefd[0];
    app->oauth_w.events = EPOLLIN;
    app->oauth_w.cb = app_oauth_done;
    app->oauth_w.ud = app;
    if (event_loop_add(app->rt->loop, &app->oauth_w) != AGENT_OK) {
        kill(pid, SIGTERM);
        (void)waitpid(pid, NULL, 0);
        close(pipefd[0]);
        app->oauth_pid = 0;
        app->oauth_fd = -1;
        app_login_reset(app);
        tui_model_append(tui_model(app->tui), LINE_SYSTEM, "无法监听 OAuth 登录进程。");
        return;
    }
    app->oauth_w_registered = true;
    tui_set_status(app->tui, "浏览器 OAuth 登录进行中；完成后会自动返回");
}

static void app_stop_oauth(App* app) {
    if (app->oauth_w_registered) {
        event_loop_remove(app->rt->loop, app->oauth_fd);
        app->oauth_w_registered = false;
    }
    if (app->oauth_fd >= 0) {
        close(app->oauth_fd);
        app->oauth_fd = -1;
    }
    if (app->oauth_pid > 0) {
        kill(app->oauth_pid, SIGTERM);
        (void)waitpid(app->oauth_pid, NULL, 0);
        app->oauth_pid = 0;
    }
}

static void app_login_input(App* app, const char* line) {
    if (app->login_step == LOGIN_MENU) {
        size_t selected = tui_choice_selected_index(app->tui);
        if (selected == 0) {
            app_login_reset(app);
            app_start_oauth(app);
            return;
        }
        if (selected == 1) {
            static const char* const providers[] = {"opencode-go", "xiaomi-mimo"};
            tui_choice_start(app->tui, providers, 2, 0);
            app->login_step = LOGIN_API_PROVIDER;
            tui_model_append(tui_model(app->tui), LINE_SYSTEM,
                             "选择 API key provider：");
            tui_set_status(app->tui,
                           "API key provider: type to filter, Up/Down choose, Enter select, Esc cancel");
            return;
        }
        tui_set_status(app->tui, "没有匹配的登录入口");
        return;
    }
    if (app->login_step == LOGIN_API_PROVIDER) {
        size_t selected = tui_choice_selected_index(app->tui);
        const char* provider = selected == 0 ? "opencode-go" : selected == 1 ? "xiaomi" : NULL;
        if (provider == NULL) {
            tui_set_status(app->tui, "没有匹配的 API key provider");
            return;
        }
        tui_choice_stop(app->tui);
        snprintf(app->login_provider, sizeof(app->login_provider), "%s", provider);
        app->login_step = LOGIN_API_KEY;
        tui_set_input_secret(app->tui, true);
        tui_model_append(tui_model(app->tui), LINE_SYSTEM, "请输入 API key（输入不会回显）：");
        tui_set_status(app->tui, "API key input");
        return;
    }
    if (app->login_step == LOGIN_API_KEY) {
        char path[PATH_MAX];
        if (oauth_default_path(path, sizeof(path)) != AGENT_OK ||
            auth_save_api_key(path, app->login_provider, line) != AGENT_OK) {
            app_login_reset(app);
            tui_model_append(tui_model(app->tui), LINE_SYSTEM, "API key 保存失败。");
            return;
        }
        bool active = app_selected_provider(app, app->login_provider);
        if (active) {
            (void)runtime_set_api_key(app->rt, line);
            tui_model_append(tui_model(app->tui), LINE_SYSTEM,
                             "API key 已保存并应用到当前 provider。");
            tui_set_status(app->tui, "API key ready");
        } else {
            tui_model_append(tui_model(app->tui), LINE_SYSTEM,
                             "API key 已保存；请将 config.json 的 provider 设为当前入口后重启。");
            tui_set_status(app->tui, "API key saved; restart to use it");
        }
        app_login_reset(app);
    }
}

/* borrowed suffix after a command prefix, or NULL */
static const char* str_after_prefix(const char* line, const char* prefix) {
    size_t n = strlen(prefix);
    if (strncmp(line, prefix, n) == 0 && line[n] != '\0') {
        return line + n;
    }
    return NULL;
}

static void app_update_header(App* app);

static void app_model_picker_close(App* app) {
    if (app == NULL || !app->model_picker) {
        return;
    }
    tui_choice_stop(app->tui);
    for (size_t i = 0; i < app->n_picker_selectors; i++) {
        free(app->picker_selectors[i]);
        free(app->picker_labels[i]);
    }
    free(app->picker_selectors);
    free(app->picker_labels);
    app->picker_selectors = NULL;
    app->picker_labels = NULL;
    app->n_picker_selectors = 0;
    app->model_picker = false;
}

static void app_model_picker_cancel(void* userdata) {
    App* app = userdata;
    app_model_picker_close(app);
    tui_set_status(app->tui, "model selection cancelled");
}

static void app_choice_cancel(void* userdata) {
    App* app = userdata;
    if (app->login_step != LOGIN_NONE) {
        app_login_reset(app);
        tui_set_status(app->tui, "login selection cancelled");
        return;
    }
    app_model_picker_cancel(app);
}

static void app_apply_model(App* app, Model* mdl) {
    if (app == NULL || mdl == NULL) {
        return;
    }
    agent_set_model(app->agent, mdl);
    char selector[PATH_MAX];
    if (runtime_model_selector(app->rt, mdl, selector, sizeof(selector)) != AGENT_OK) {
        tui_model_append(tui_model(app->tui), LINE_SYSTEM,
                         "active model selected (cannot persist selector)");
        app_update_header(app);
        return;
    }

    const char* provider = mdl->provider != NULL ? mdl->provider->provider_name : NULL;
    const char* active_name = mdl->name;
    if (active_name != NULL && provider != NULL) {
        size_t provider_len = strlen(provider);
        if (strncmp(active_name, provider, provider_len) == 0 &&
            active_name[provider_len] == '/') {
            active_name += provider_len + 1;
        }
    }
    free(app->rt->config.model_name);
    app->rt->config.model_name = active_name != NULL ? strdup(active_name) : NULL;
    int save_rc = app->rt->config.model_name != NULL && provider != NULL
                      ? config_save_selection(app->config_path, provider, active_name)
                      : AGENT_ERR_OOM;
    /* The persisted provider is kept separately for routing, while the
     * active/API model name is always the bare provider model id. */
    String s = string_new();
    if (save_rc == AGENT_OK) {
        string_printf(&s, "active model: %s (saved)", active_name != NULL ? active_name : "(unknown)");
    } else {
        string_printf(&s, "active model: %s (not saved: %s)",
                      active_name != NULL ? active_name : "(unknown)", error_name(save_rc));
    }
    tui_model_append_n(tui_model(app->tui), LINE_SYSTEM, s.data, s.len);
    string_free(&s);
    app_update_header(app);
}

static const char* app_model_bare_name(const Runtime* rt, const Model* model) {
    if (model == NULL || model->name == NULL) {
        return NULL;
    }
    const char* provider = model->provider != NULL ? model->provider->provider_name : NULL;
    if (provider == NULL || provider[0] == '\0') {
        provider = rt != NULL && rt->config.provider != NULL ? rt->config.provider : NULL;
    }
    if (provider != NULL) {
        size_t provider_len = strlen(provider);
        if (strncmp(model->name, provider, provider_len) == 0 &&
            model->name[provider_len] == '/') {
            return model->name + provider_len + 1;
        }
    }
    return model->name;
}

static void app_model_picker_submit(App* app) {
    size_t index = tui_choice_selected_index(app->tui);
    char* selector = index < app->n_picker_selectors
                        ? strdup(app->picker_selectors[index])
                        : NULL;
    app_model_picker_close(app);
    if (selector == NULL) {
        tui_model_append(tui_model(app->tui), LINE_SYSTEM, "no model matches the current query");
        tui_set_status(app->tui, "model selection cancelled");
        return;
    }

    /* The catalog may have been replaced while the picker was open. Resolve
     * the selector only after closing the picker so the lookup uses current
     * model objects instead of stale pointers. */
    Model* mdl = runtime_model_by_name(app->rt, selector);
    free(selector);
    if (mdl == NULL) {
        tui_model_append(tui_model(app->tui), LINE_SYSTEM, "selected model is no longer available");
        tui_set_status(app->tui, "model selection cancelled");
        return;
    }
    app_apply_model(app, mdl);
}

static void app_model_picker_open(App* app) {
    size_t capacity = (app->rt->model != NULL ? 1 : 0) + app->rt->n_models;
    if (capacity == 0) {
        tui_model_append(tui_model(app->tui), LINE_SYSTEM, "(no models available)");
        return;
    }
    char** selectors = calloc(capacity, sizeof(*selectors));
    if (selectors == NULL) {
        tui_set_status(app->tui, "cannot open model selector (out of memory)");
        return;
    }

    /* Provider-qualified selectors remain the stable internal value.  When
     * every choice comes from one provider, hide that repeated prefix in the
     * picker; it adds noise without disambiguating anything. */
    const char* first_provider = NULL;
    bool multiple_providers = false;
    for (size_t source = 0; source < capacity; source++) {
        Model* candidate = app->rt->model != NULL
                               ? (source == 0 ? app->rt->model : app->rt->models[source - 1])
                               : app->rt->models[source];
        if (candidate == NULL) {
            continue;
        }
        const char* provider = candidate->provider != NULL ? candidate->provider->provider_name : NULL;
        if (provider == NULL || provider[0] == '\0') {
            provider = app->rt->config.provider;
        }
        if (first_provider == NULL) {
            first_provider = provider;
        } else if (provider == NULL || strcmp(first_provider, provider) != 0) {
            multiple_providers = true;
        }
    }

    char** labels = calloc(capacity, sizeof(*labels));
    if (labels == NULL) {
        free(selectors);
        tui_set_status(app->tui, "cannot open model selector (out of memory)");
        return;
    }
    size_t count = 0;
    size_t selected = 0;
    Model* current = app->agent->model;
    for (size_t source = 0; source < capacity; source++) {
        Model* candidate = app->rt->model != NULL
                               ? (source == 0 ? app->rt->model : app->rt->models[source - 1])
                               : app->rt->models[source];
        if (candidate == NULL) {
            continue;
        }
        char selector[PATH_MAX];
        if (runtime_model_selector(app->rt, candidate, selector, sizeof(selector)) != AGENT_OK) {
            continue;
        }
        selectors[count] = strdup(selector);
        const char* display = multiple_providers ? selector : app_model_bare_name(app->rt, candidate);
        labels[count] = display != NULL ? strdup(display) : NULL;
        if (selectors[count] == NULL || labels[count] == NULL) {
            free(selectors[count]);
            free(labels[count]);
            for (size_t j = 0; j < count; j++) {
                free(selectors[j]);
                free(labels[j]);
            }
            free(selectors);
            free(labels);
            tui_set_status(app->tui, "cannot open model selector (out of memory)");
            return;
        }
        if (candidate == current) {
            selected = count;
        }
        count++;
    }
    if (count == 0) {
        free(selectors);
        free(labels);
        tui_model_append(tui_model(app->tui), LINE_SYSTEM, "(no models available)");
        return;
    }
    app->picker_selectors = selectors;
    app->picker_labels = labels;
    app->n_picker_selectors = count;
    app->model_picker = true;
    tui_choice_start(app->tui, (const char* const*)labels, count, selected);
    tui_set_status(app->tui,
                   "model: type to fuzzy-filter, Up/Down choose, Enter select, Esc cancel");
}

static void app_report_restore(App* app) {
    app_update_header(app);
    tui_set_busy(app->tui, app->agent_busy);
}

static void app_report_cancel(void* userdata) {
    App* app = userdata;
    tui_report_stop(app->tui);
    app_report_restore(app);
}

static void app_update_header(App* app) {
    String header = string_new();
    const char* model = app->agent->model != NULL
                            ? app_model_bare_name(app->rt, app->agent->model)
                            : NULL;
    if (model == NULL) {
        model = "(none)";
    }
    const char* session_id = app->agent->session != NULL ? app->agent->session->id : "(none)";
    string_printf(&header, " cagent%s | model: %s | session: %s | PgUp/PgDn | Ctrl+C cancel",
                  agent_session_trusted(app->agent) ? " [TRUSTED]" : "", model, session_id);
    tui_set_header(app->tui, header.data);
    string_free(&header);
}

static void app_command(App* app, const char* line) {
    TuiModel* m = tui_model(app->tui);
    const char* v;

    if (strcmp(line, "/login") == 0) {
        if (app->oauth_pid > 0) {
            tui_set_status(app->tui, "OAuth login is already running");
            return;
        }
        static const char* const login_choices[] = {"订阅（ChatGPT OAuth）", "API key"};
        app->login_step = LOGIN_MENU;
        tui_set_input_secret(app->tui, false);
        tui_choice_start(app->tui, login_choices, 2, 0);
        tui_model_append(m, LINE_SYSTEM, "登录入口：");
        tui_set_status(app->tui,
                       "login: type to filter, Up/Down choose, Enter select, Esc cancel");
        return;
    }

    if (strcmp(line, "/session") == 0) {
        if (app->agent->session != NULL) {
            String s = string_new();
            string_printf(&s, "session: %s\nresume: cagent -r %s", app->agent->session->id,
                          app->agent->session->id);
            tui_model_append_n(m, LINE_SYSTEM, s.data, s.len);
            string_free(&s);
        } else {
            tui_model_append(m, LINE_SYSTEM, "session persistence is unavailable");
        }
        return;
    }

    if (strcmp(line, "/trust") == 0) {
        tui_model_append(m, LINE_SYSTEM,
                         agent_session_trusted(app->agent) ? "trusted mode: on" : "trusted mode: off");
        return;
    }
    if (strcmp(line, "/trust on") == 0) {
        agent_set_session_trusted(app->agent, true);
        app_update_header(app);
        tui_model_append(m, LINE_SYSTEM,
                         "WARNING: trusted mode enabled for this process; top-level bash/write/edit "
                         "and git writes are auto-approved until /trust off");
        tui_set_status(app->tui, "TRUSTED: use /trust off to restore per-tool approval");
        return;
    }
    if (strcmp(line, "/trust off") == 0) {
        agent_set_session_trusted(app->agent, false);
        app_update_header(app);
        tui_model_append(m, LINE_SYSTEM, "trusted mode disabled; per-tool approval restored");
        tui_set_status(app->tui, "per-tool approval enabled");
        return;
    }
    if (strcmp(line, "/report") == 0) {
        tui_report_start(app->tui);
        tui_set_status(app->tui, "/report: Up/Down scroll, PgUp/PgDn page, Enter/Esc return");
        return;
    }

    if (strcmp(line, "/settings") == 0 || strcmp(line, "/help") == 0) {
        String s = string_new();
        const char* default_model = app->rt->model != NULL
                                        ? app_model_bare_name(app->rt, app->rt->model)
                                        : app->rt->config.model_name;
        string_printf(&s, "base_url: %s\n",
                      app->rt->config.base_url != NULL ? app->rt->config.base_url : "(unset)");
        string_printf(&s, "model:    %s (default)\n",
                      default_model != NULL ? default_model : "(unset)");
        if (app->agent->model != NULL && app->agent->model != app->rt->model) {
            string_printf(&s, "active:   %s\n",
                          app_model_bare_name(app->rt, app->agent->model));
        }
        string_printf(&s, "key:      %s\n",
                      app->rt->provider != NULL && app->rt->provider->api_key != NULL
                          ? "(set)"
                          : "(not set)");
        if (strcmp(line, "/help") == 0) {
            string_append(&s,
                          "commands: /login /settings /session /report /trust [on|off] "
                          "/base-url <url> /key <value> /model [provider/model] "
                          "(Up/Down + fuzzy search) /help\n");
        }
        tui_model_append_n(m, LINE_SYSTEM, s.data, s.len);
        string_free(&s);
        return;
    }
    v = str_after_prefix(line, "/base-url ");
    if (v != NULL) {
        if (runtime_set_base_url(app->rt, v) == AGENT_OK) {
            tui_model_append(m, LINE_SYSTEM, "base_url updated (in-memory)");
        } else {
            tui_model_append(m, LINE_SYSTEM, "invalid base_url");
        }
        return;
    }
    v = str_after_prefix(line, "/key ");
    if (v != NULL) {
        if (runtime_set_api_key(app->rt, v) == AGENT_OK) {
            tui_model_append(m, LINE_SYSTEM, "API key updated (in-memory, not persisted)");
        } else {
            tui_model_append(m, LINE_SYSTEM, "invalid key");
        }
        return;
    }
    if (strcmp(line, "/model") == 0) {
        app_model_picker_open(app);
        return;
    }
    v = str_after_prefix(line, "/model ");
    if (v != NULL && v[0] != '\0') {
        Model* mdl = runtime_model_by_name(app->rt, v);
        if (mdl != NULL) {
            app_apply_model(app, mdl);
        } else {
            tui_model_append(m, LINE_SYSTEM, "unknown model; use /model to list selectors");
        }
        return;
    }
    tui_model_append(m, LINE_SYSTEM, "unknown command; /help for the list");
}

static void app_submit(void* ud, const char* line) {
    App* app = ud;
    if (app->approval_pending) {
        bool trust_on = strcmp(line, "/trust on") == 0 || strcmp(line, "always") == 0;
        bool trust_off = strcmp(line, "/trust off") == 0;
        bool approved = trust_on || strcmp(line, "/approve") == 0 ||
                        strcmp(line, "approve") == 0 || strcmp(line, "y") == 0 ||
                        strcmp(line, "Y") == 0;
        bool rejected = strcmp(line, "/reject") == 0 || strcmp(line, "reject") == 0 ||
                        strcmp(line, "n") == 0 || strcmp(line, "N") == 0;
        if (trust_off) {
            agent_set_session_trusted(app->agent, false);
            app_update_header(app);
            tui_set_status(app->tui, "trusted 已关闭；当前调用仍需 /approve 或 /reject");
            return;
        }
        if (!approved && !rejected) {
            tui_set_status(app->tui, "请输入 /approve、/reject，或 /trust on");
            return;
        }
        if (trust_on) {
            agent_set_session_trusted(app->agent, true);
            app_update_header(app);
            tui_model_append(tui_model(app->tui), LINE_SYSTEM,
                             "WARNING: trusted mode enabled; subsequent top-level side-effecting "
                             "tools are auto-approved until /trust off");
        }
        if (agent_set_approval_result(app->agent, approved) != AGENT_OK) {
            app->approval_pending = false;
            tui_set_status(app->tui, "审批请求已失效");
            return;
        }
        app->approval_pending = false;
        tui_set_status(app->tui, trust_on       ? "TRUSTED：已批准，后续自动执行"
                                 : approved ? "已批准，继续执行"
                                            : "已拒绝，返回模型处理");
        return;
    }
    if (tui_report_active(app->tui)) {
        tui_report_stop(app->tui);
        app_report_restore(app);
        return;
    }
    if (app->model_picker) {
        app_model_picker_submit(app);
        return;
    }
    if (strcmp(line, "/quit") == 0 || strcmp(line, "/exit") == 0 || strcmp(line, "exit") == 0 ||
        strcmp(line, "quit") == 0) {
        if (app->agent_busy) {
            app->quit_pending = true;
            cancel_token_cancel(&app->agent->cancel);
            tui_set_status(app->tui, "cancelling before exit...");
        } else {
            app->quit = true;
        }
        return;
    }
    if (app->login_step != LOGIN_NONE) {
        app_login_input(app, line);
        return;
    }
    if (line[0] == '/') {
        app_command(app, line);
        return;
    }
    if (app->agent_busy) {
        tui_set_status(app->tui, "agent is running; wait or press Ctrl+C");
        return;
    }
    if (agent_start(app->agent, line) == AGENT_OK) {
        app->agent_busy = true;
        app_update_statusline(app);
        tui_set_busy(app->tui, true);
    } else {
        tui_set_status(app->tui, "failed to start the agent");
    }
}

static void app_cancel(void* ud) {
    App* app = ud;
    if (app->oauth_pid > 0) {
        app_stop_oauth(app);
        app_login_reset(app);
        tui_model_append(tui_model(app->tui), LINE_SYSTEM, "OAuth 登录已取消，返回首页。");
        tui_set_status(app->tui, "ready");
        return;
    }
    if (app->login_step != LOGIN_NONE) {
        app_login_reset(app);
        tui_set_status(app->tui, "登录已取消，返回首页。");
        return;
    }
    if (app->approval_pending) {
        app->approval_pending = false;
        (void)agent_set_approval_result(app->agent, false);
    }
    if (app->agent_busy) {
        cancel_token_cancel(&app->agent->cancel);
        tui_set_status(app->tui, "cancelling...");
    } else {
        app->quit = true;
    }
}

static void app_escape(void* ud) {
    App* app = ud;
    if (app->oauth_pid > 0 || app->login_step != LOGIN_NONE) {
        app_cancel(app);
        return;
    }
    if (app->approval_pending) {
        app->approval_pending = false;
        (void)agent_set_approval_result(app->agent, false);
        tui_set_status(app->tui, "审批已取消，返回首页。");
        return;
    }
    if (app->agent_busy) {
        cancel_token_cancel(&app->agent->cancel);
        tui_set_status(app->tui, "cancelling...");
    }
}

static void app_stdin_cb(EventLoop* loop, int fd, uint32_t events, void* ud) {
    (void)loop;
    (void)events;
    App* app = ud;
    char buf[256];
    ssize_t n = read(fd, buf, sizeof(buf));
    if (n <= 0) {
        if (app->agent_busy) {
            app->quit_pending = true;
            cancel_token_cancel(&app->agent->cancel);
            tui_set_status(app->tui, "cancelling before exit...");
        } else {
            app->quit = true;
        }
        return;
    }
    tui_feed_bytes(app->tui, buf, (size_t)n);
}

static void app_discovery_cb(EventLoop* loop, int fd, uint32_t events, void* userdata) {
    (void)events;
    App* app = userdata;
    AppDiscovery* d = app->discovery;
    if (d == NULL || !d->done || d->reported) {
        return;
    }
    /* Do not replace model objects while the agent or one of its subagents
     * may still have a request using them. The pipe remains readable and the
     * callback will retry after the turn completes. */
    if (app->agent_busy) {
        return;
    }
    d->reported = true;
    event_loop_remove(loop, fd);
    discovery_apply(d, app->rt, app->agent);
    tui_set_busy(app->tui, false);
    app_update_statusline(app); /* the catalog may carry a fresh context_window */
    if (d->failures[0] == '\0') {
        tui_set_status(app->tui, "模型目录已更新；/model 可查看");
    } else {
        size_t flen = strlen(d->failures);
        if (flen >= 2 && strcmp(d->failures + flen - 2, "; ") == 0) {
            d->failures[flen - 2] = '\0';
        }
        char notice[800];
        (void)snprintf(notice, sizeof(notice),
                       "模型目录获取失败：%s。已使用配置中的静态模型列表；请检查网络/代理与登录状态",
                       d->failures);
        tui_model_append(tui_model(app->tui), LINE_SYSTEM, notice);
        tui_set_status(app->tui, "模型目录获取失败，详情见消息区");
    }
}

static void tui_run(Agent* a, Runtime* rt, const char* config_path,
                    AppDiscovery* discovery) {
    Tui* tui = tui_new(STDIN_FILENO);
    if (tui == NULL) {
        fprintf(stderr, "error: cannot initialize the TUI\n");
        return;
    }
    log_set_stderr(false); /* the TUI owns the terminal */

    App app = {0};
    app.oauth_fd = -1;
    app.agent = a;
    app.rt = rt;
    app.tui = tui;
    app.config_path = config_path;
    agent_set_event_cb(a, app_agent_event, &app);
    agent_set_approval_available(a, true);
    tui_replay_history(tui, &a->messages);
    app.discovery = discovery;
    if (discovery != NULL && discovery->thread_started) {
        app.discovery_w.fd = discovery->pipefd[0];
        app.discovery_w.events = EPOLLIN;
        app.discovery_w.cb = app_discovery_cb;
        app.discovery_w.ud = &app;
        event_loop_add(rt->loop, &app.discovery_w);
    }
    tui_set_callbacks(tui, app_submit, app_cancel, &app);
    tui_set_escape_callback(tui, app_escape);
    tui_set_choice_cancel_callback(tui, app_choice_cancel);
    tui_set_report_cancel_callback(tui, app_report_cancel);
    app_update_header(&app);
    tui_set_busy(tui, false);
    app_update_statusline(&app);

    /* stdin joins the event loop */
    app.stdin_w.fd = STDIN_FILENO;
    app.stdin_w.events = EPOLLIN;
    app.stdin_w.cb = app_stdin_cb;
    app.stdin_w.ud = &app;
    event_loop_add(rt->loop, &app.stdin_w);

    signal(SIGWINCH, on_winch);

    while (!app.quit) {
        runtime_pump(rt, 50);

        /* advance the running agent */
        if (app.agent_busy) {
            AgentStepResult r = agent_resume(app.agent);
            if (r != AGENT_STEP_BUSY) {
                app.agent_busy = false;
                app_update_statusline(&app);
                tui_set_busy(tui, false);
                if (r == AGENT_STEP_ERROR) {
                    tui_set_status(tui, "agent finished with an error");
                } else if (r == AGENT_STEP_CANCELLED) {
                    tui_set_status(tui, "cancelled; ready for the next request");
                } else {
                    tui_set_status(tui, "ready");
                }
                if (app.quit_pending) {
                    app.quit = true;
                }
            }
        }

        if (g_winch) {
            g_winch = 0;
            tui_check_resize(tui);
        }
        if (g_sigint) {
            g_sigint = 0;
            if (app.agent_busy) {
                /* first Ctrl+C cancels the running turn (DESIGN.md §29) */
                cancel_token_cancel(&app.agent->cancel);
                tui_set_status(tui, "cancelling...");
            } else {
                app.quit = true;
            }
        }
    }

    if (app.oauth_w_registered) {
        event_loop_remove(rt->loop, app.oauth_fd);
        app.oauth_w_registered = false;
    }
    if (app.oauth_fd >= 0) {
        close(app.oauth_fd);
        app.oauth_fd = -1;
    }
    if (app.oauth_pid > 0) {
        kill(app.oauth_pid, SIGTERM);
        (void)waitpid(app.oauth_pid, NULL, 0);
        app.oauth_pid = 0;
    }
    event_loop_remove(rt->loop, STDIN_FILENO);
    app_model_picker_close(&app);
    if (tui_report_active(tui)) {
        tui_report_stop(tui);
    }
    tui_free(tui);
    log_set_stderr(true);
}

int main(int argc, char** argv) {
    const char* dir = NULL;
    const char* model_name = NULL;
    const char* prompt = NULL;
    const char* config_path = NULL;
    const char* resume_id = NULL;
    bool plain_mode = false;
    bool login = false;
    bool device_code = false;
    bool logout = false;

    static const struct option longopts[] = {
        {"model", required_argument, NULL, 'm'},  {"config", required_argument, NULL, 'c'},
        {"prompt", required_argument, NULL, 'p'}, {"resume", required_argument, NULL, 'r'},
        {"plain", no_argument, NULL, 'l'},        {"login", no_argument, NULL, 1000},
        {"device-code", no_argument, NULL, 1001}, {"logout", no_argument, NULL, 1002},
        {"help", no_argument, NULL, 'h'},         {NULL, 0, NULL, 0},
    };

    int opt;
    while ((opt = getopt_long(argc, argv, "C:m:p:c:r:lh", longopts, NULL)) != -1) {
        switch (opt) {
        case 'C':
            dir = optarg;
            break;
        case 'm':
            model_name = optarg;
            break;
        case 'p':
            prompt = optarg;
            break;
        case 'c':
            config_path = optarg;
            break;
        case 'r':
            resume_id = optarg;
            break;
        case 'l':
            plain_mode = true;
            break;
        case 1000:
            login = true;
            break;
        case 1001:
            login = true;
            device_code = true;
            break;
        case 1002:
            logout = true;
            break;
        case 'h':
            usage(stdout);
            return 0;
        default:
            usage(stderr);
            return 2;
        }
    }

    char oauth_path[PATH_MAX];
    if (login || logout) {
        if (oauth_default_path(oauth_path, sizeof(oauth_path)) != AGENT_OK) {
            fprintf(stderr, "error: %s\n", oauth_last_error());
            return 1;
        }
        int auth_rc = logout ? oauth_remove(oauth_path) : oauth_login(oauth_path, device_code);
        if (auth_rc != AGENT_OK) {
            fprintf(stderr, "error: %s\n", oauth_last_error());
            return 1;
        }
        if (logout) {
            printf("ChatGPT OAuth credentials removed.\n");
        }
        return 0;
    }

    /* ---- config (CLI > env > file > defaults) ---- */
    Config cfg = config_default();
    const char* base_url = getenv("OPENAI_BASE_URL");
    if (base_url != NULL && base_url[0] != '\0') {
        cfg.base_url = strdup(base_url);
    }
    /* api_key_env stays NULL -> runtime default (OPENCODE_GO_API_KEY);
     * the config file may override it */

    const char* fpath = config_path != NULL ? config_path : config_path_default();
    if (fpath != NULL) {
        int err = config_load_file(&cfg, fpath);
        if (err != AGENT_OK) {
            log_warn("ignoring bad config file %s", fpath);
        }
    }
    if (model_name != NULL) {
        free(cfg.model_name);
        cfg.model_name = strdup(model_name);
    }
    if (dir != NULL) {
        free(cfg.cwd);
        cfg.cwd = strdup(dir);
    }

    /* key availability: config api_key_env may be "$NAME" (env reference),
     * a literal key, or absent (default OPENCODE_GO_API_KEY). Never print
     * the value — it may be a secret (DESIGN.md §82).
     * No hard requirement at startup: the app opens without a key so the
     * user can configure it inside the TUI (/settings, /base-url, /key,
     * /model); requests fail with a clear error until then. */

    /* Every provider exposes a live model catalog. Fetch it in the
     * background so startup never blocks on the network; the result is
     * applied once the TUI/REPL is running (see app_discovery_cb and the
     * plain-mode check below). A configured static table remains the
     * fallback until then. */
    AppDiscovery* discovery = discovery_start(&cfg);

    /* ---- runtime + agent ---- */
    Runtime* rt = runtime_new(&cfg);
    if (rt == NULL) {
        fprintf(stderr, "error: failed to initialize runtime\n");
        config_free(&cfg);
        return 1;
    }

    AgentConfig ac = {0};
    ac.model_name = cfg.model_name;
    ac.cwd = cfg.cwd;
    String project_prompt = string_new();
    (void)build_project_prompt(rt->config.cwd, &rt->config, &project_prompt);
    ac.system_prompt = project_prompt.data;
    Agent* a = agent_new(rt, &ac);
    string_free(&project_prompt);
    if (a == NULL) {
        fprintf(stderr, "error: failed to create agent\n");
        runtime_free(rt);
        config_free(&cfg);
        return 1;
    }
    agent_set_event_cb(a, print_event, NULL);

    /* session: resume an existing one or start fresh */
    const char* sdir = session_default_dir();
    Session* session = NULL;
    if (resume_id != NULL) {
        session = session_open(sdir, resume_id);
        if (session == NULL) {
            fprintf(stderr, "error: cannot resume session %s\n", resume_id);
            agent_destroy(a);
            runtime_free(rt);
            config_free(&cfg);
            return 1;
        }
        /* Resume restores the session's recorded project directory unless
         * the caller explicitly supplied -C. This keeps tools and project
         * memory anchored to the original workspace. */
        if (dir == NULL && session->cwd != NULL) {
            struct stat cwd_stat;
            if (stat(session->cwd, &cwd_stat) == 0 && S_ISDIR(cwd_stat.st_mode)) {
                char* restored_agent_cwd = strdup(session->cwd);
                char* restored_runtime_cwd = strdup(session->cwd);
                if (restored_agent_cwd != NULL && restored_runtime_cwd != NULL) {
                    free(a->config.cwd);
                    a->config.cwd = restored_agent_cwd;
                    free(rt->config.cwd);
                    rt->config.cwd = restored_runtime_cwd;
                } else {
                    free(restored_agent_cwd);
                    free(restored_runtime_cwd);
                    log_warn("cannot restore session cwd: out of memory");
                }
            } else {
                log_warn("session cwd no longer exists: %s", session->cwd);
            }
        }
        /* The session may have restored a different cwd, so rebuild project
         * instructions from that workspace before adding session memory. */
        String resumed_project_prompt = string_new();
        if (build_project_prompt(a->config.cwd, &rt->config, &resumed_project_prompt) == AGENT_OK) {
            free(a->config.system_prompt);
            a->config.system_prompt = string_take(&resumed_project_prompt);
        } else {
            string_free(&resumed_project_prompt);
        }
        /* Live session memory is injected on every turn by the agent loop
         * (bounded), so it is not duplicated here. The project plan is
         * static state and is injected once so the resumed agent starts
         * with its task context instead of relying on plan list. */
        String plan_text = string_new();
        if (plan_summary(a->config.cwd, &plan_text) == AGENT_OK && plan_text.len > 0) {
            String resumed_prompt = string_new();
            if (a->config.system_prompt != NULL) {
                string_append(&resumed_prompt, a->config.system_prompt);
                string_append(&resumed_prompt, "\n");
            }
            string_append(&resumed_prompt,
                          "Project plan (from .cagent/plan.json; keep it up to date with plan "
                          "list/add/start/complete/fail):\n");
            string_append(&resumed_prompt, plan_text.data);
            free(a->config.system_prompt);
            a->config.system_prompt = string_take(&resumed_prompt);
        }
        string_free(&plan_text);

        MessageList history = {0};
        if (session_load_messages(session, &history) != AGENT_OK) {
            fprintf(stderr, "error: failed to load session messages\n");
            message_list_free(&history);
            session_free(session);
            agent_destroy(a);
            runtime_free(rt);
            config_free(&cfg);
            return 1;
        }
        message_list_move_range(&a->messages, &history, 0, history.len);
        message_list_free(&history);
        log_info("resumed session %s (%zu messages)", resume_id, a->messages.len);
    } else {
        session = session_create(sdir, rt->config.cwd, rt->config.model_name, rt->config.base_url);
        if (session != NULL) {
            log_info("session %s started", session->id);
        }
    }
    a->session = session; /* borrowed; freed below */

    signal(SIGINT, on_sigint);
    setup_file_log();
    signal(SIGPIPE, SIG_IGN);

    printf("cagent (model: %s)\n", rt->config.model_name);
    if (session != NULL)
        printf("session: %s\n", session->id);
    printf("type 'exit' or press Ctrl+C to quit\n\n");

    if (prompt != NULL) {
        agent_set_approval_cb(a, terminal_approval, a);
        int rc = run_agent_interruptible(a, rt, prompt);
        printf("\n");
        if (rc != AGENT_OK && rc != AGENT_ERR_CANCELLED) {
            fprintf(stderr, "agent failed: %s\n", error_name(rc));
        }
        if (session != NULL) {
            session_append_stats(session);
        }
        discovery_finish(discovery, rt, a);
        agent_destroy(a);
        session_free(session);
        runtime_free(rt);
        config_free(&cfg);
        log_close();
        return rc == AGENT_OK ? 0 : 1;
    }

    if (plain_mode) {
        agent_set_approval_cb(a, terminal_approval, a);
        /* ---- interactive REPL (--plain) ---- */
        char buf[65536];
        for (;;) {
            if (g_sigint) {
                break; /* Ctrl+C while idle: exit */
            }
            if (discovery != NULL && discovery->thread_started && discovery->done &&
                !discovery->reported) {
                discovery->reported = true;
                discovery_apply(discovery, rt, a);
                discovery_notice(discovery);
            }
            printf("> ");
            fflush(stdout);

            if (fgets(buf, sizeof(buf), stdin) == NULL) {
                printf("\n");
                break;
            }
            /* trim trailing newline */
            size_t len = strlen(buf);
            while (len > 0 && (buf[len - 1] == '\n' || buf[len - 1] == '\r')) {
                buf[--len] = '\0';
            }
            if (len == 0) {
                continue;
            }
            if (strcmp(buf, "exit") == 0 || strcmp(buf, "/exit") == 0 || strcmp(buf, "quit") == 0) {
                break;
            }
            if (plain_trust_command(a, buf)) {
                continue;
            }

            if (g_sigint) {
                break;
            }
            int rc = run_agent_interruptible(a, rt, buf);
            if (rc == AGENT_ERR_CANCELLED) {
                printf("\n(cancelled)\n");
            } else if (rc != AGENT_OK) {
                printf("\n(agent error: %s)\n", error_name(rc));
            }
            g_sigint = 0;
            printf("\n");
        }
    } else {
        tui_run(a, rt, fpath, discovery);
    }

    discovery_finish(discovery, rt, a);

    if (session != NULL) {
        session_append_stats(session);
        printf("\nsession: %s\n", session->id);
    }

    /* session statistics (DESIGN.md §73) */
    printf("\n--- session stats ---\n");
    printf("requests:   %llu\n", (unsigned long long)a->request_count);
    printf("model time: %lld ms\n", (long long)a->model_time_ms);
    printf("tool calls: %llu\n", (unsigned long long)a->tool_call_count);
    printf("tokens:     in %lld, out %lld, cached %lld, total %lld\n",
           (long long)a->usage_total.input_tokens, (long long)a->usage_total.output_tokens,
           (long long)a->usage_total.cached_tokens, (long long)a->usage_total.total_tokens);
    log_info("cagent session ended");

    agent_destroy(a);
    session_free(session);
    runtime_free(rt);
    config_free(&cfg);
    log_close();
    return 0;
}
