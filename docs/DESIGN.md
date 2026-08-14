# cagent 设计文档

> 轻量级、原生、高性能、可长期自行扩展的 AI Coding Agent Runtime（纯 C17，Linux/POSIX）。

本文档对应需求分析（模块边界）、最终架构（数据流/控制流）、Phase 1 核心数据结构设计三个部分，是后续开发的基线。设计决策遵循：**正确性 > 架构简单 > 稳定性 > 可维护性 > 性能 > 功能数量**；不做插件系统，禁止"一 Agent 一线程/一进程"。

---

## 1. 模块边界划分

| 模块 | 归属 | 职责 | 禁止知道 |
|---|---|---|---|
| `agent/` | Core | Agent 状态机、Message 上下文、Agent Loop、Compaction | Provider 特有格式、HTTP、TUI、Shell 实现 |
| `model/` | Provider | Provider 抽象（OpenAI Compatible 第一版）、Model 配置、SSE 流解析、统一 Usage | Agent、Tool 具体实现 |
| `tool/` | Tool | 工具注册表与工具实现（read/write/edit/bash/grep/subagent） | Model 格式、TUI |
| `session/` | 数据层 | Session 持久化（JSONL）、元数据、usage 统计 | Provider 格式 |
| `runtime/` | Runtime | Event Loop、Scheduler、Worker Pool、Process 管理、取消传播 | 具体工具语义 |
| `tui/` | UI | 终端渲染、输入，只通过 Event/State/Command 与 Core 通信 | Agent 内部逻辑 |
| `util/` | 基础 | String/Buffer/Arena/Vector/HashMap/JSON 封装/日志 | 业务语义 |

核心（Agent/Message/Model/Tool/Session/Event/Runtime）不感知 Git/ADB/Gradle/LSP/Search/Web 等具体业务能力；这些能力以普通 C 模块加入 `tool/` 并注册即可。

---

## 2. 最终架构

```
                        ┌──────────────────────┐
                        │       Runtime        │
                        │   owns: config/state │
                        └───┬────────┬─────────┘
              borrows       │        │
        ┌───────────────────┼────────┼───────────────────┐
        │                   │        │                   │
   ┌────▼────┐      ┌───────▼──┐ ┌───▼───────┐   ┌───────▼──────┐
   │ Provider│      │ Scheduler│ │Event Loop │   │ Worker Pool │
   │(curl m) │      │ (N active│ │  (epoll)  │   │ (fixed N)   │
   └────┬────┘      │  agents) │ └───┬───────┘   └───────┬──────┘
        │           └───────┬──┘     │                   │
        │                   │        │             CPU 任务/结果
        │             ┌─────▼─────┐  │                   │
        │             │   Agent   │◄─┘  Agent 状态机:    │
        │             │  (state)  │     挂起/唤醒，不占线程
        │             └─────┬─────┘                   eventfd 唤醒
        │                   │  borrows
        │      ┌────────────┼─────────────┐
        │      │            │             │
        │  ┌───▼───┐   ┌────▼────┐   ┌────▼────┐
        │  │ Tool  │   │ Session │   │ Cancel  │
        │  │Registry│   │ (JSONL) │   │ Token   │
        │  └───┬───┘   └─────────┘   └─────────┘
        │      │  read/write/edit/bash/grep/subagent...
        └───┬──┘
   ┌────────┴───────────┐
   │ libcurl multi 句柄  │  ← 所有 Agent 共享一个 HTTP Runtime
   │ A1 A2 A3 ... A100   │
   └────────────────────┘
```

**数据流（一次 Agent 迭代）**：`User → Agent(AGENT_WAIT_MODEL) → ModelRequest → Provider 序列化 → libcurl multi(异步) → SSE 分片 → Provider ModelEvent → Agent 累积 text/reasoning/tool_calls → AGENT_WAIT_TOOL → ToolRegistry 查找/校验 → Tool 执行（bash 等异步走 Event Loop）→ ToolResult 入 MessageList → 下一轮迭代`。只有“无 tool_call 且终止原因正常”才进入 `DONE`；输出达到 token 上限时自动续跑（每段连续截断最多 3 次；模型完成一次正常响应或工具调用后重新计数），仍被连续截断或收到不可恢复的 incomplete/content-filter 终止时进入 `ERROR`，不能伪装成 ready。

**控制流**：Event Loop 是唯一可变状态的属主；Agent 只持有状态快照和 borrowed 指针。Worker Pool 完成 CPU 任务后经线程安全队列 + `eventfd` 唤醒 Event Loop 处理结果，绝不直接修改 Agent 状态。取消沿 `CancelToken` 父子链传播（Agent → 子 Agent → HTTP → 进程）。

**并发模型（Phase 4 目标）**：几十~上百 Agent 共享 `libcurl multi + epoll + 固定 Worker Pool`，Agent 等待 IO 时挂起在调度器，不占线程、不占进程。

---

## 3. Phase 1 数据结构设计

> Ownership 约定：标注 **owned** = 该字段/返回值的释放责任属于本结构体的 destroy 函数或调用方；**borrowed** = 引用他人所有，禁止 free；**static** = 编译期常量。所有 `char*` 默认 NUL 结尾。

### 3.1 util/string.h — String（动态字符串）

```c
typedef struct {
    char *data;  /* owned; 有效时恒 NUL 结尾 */
    size_t len;  /* 不含 NUL */
    size_t cap;  /* 不含 NUL 的已分配容量 */
} String;

String  string_new(void);
void    string_free(String *s);          /* 释放 data，归零 */
int     string_reserve(String *s, size_t extra);
int     string_append(String *s, const char *src);          /* borrowed src */
int     string_append_n(String *s, const char *src, size_t n);
int     string_append_char(String *s, char c);
int     string_printf(String *s, const char *fmt, ...);
char   *string_take(String *s);          /* 移交 data 所有权，s 置空；调用方 free() */
void    string_clear(String *s);
```

- `int` 返回值：`AGENT_OK=0` 成功，`AGENT_ERR_OOM` 失败（见 §3.9）。
- 追加后内容可能被 realloc 移动：**借用的指针不得跨 append 调用持有**。

### 3.2 util/buffer.h — Buffer（字节缓冲，可含二进制）

```c
typedef struct {
    uint8_t *data;  /* owned; 不保证 NUL 结尾 */
    size_t len;
    size_t cap;
} Buffer;

Buffer  buffer_new(void);
void    buffer_free(Buffer *b);
int     buffer_reserve(Buffer *b, size_t extra);
int     buffer_append(Buffer *b, const void *src, size_t n);
void    buffer_clear(Buffer *b);
```

用途：HTTP 响应体、SSE 分片累积、Tool Result 二进制输出。`buffer_append` 成功后旧指针失效。

### 3.3 util/arena.h — Arena（短生命周期批量分配）

```c
typedef struct Arena Arena;  /* 内部为块链表 */

Arena *arena_new(size_t block_size);
void   arena_destroy(Arena *a);
void  *arena_alloc(Arena *a, size_t size);        /* owned by arena；8 字节对齐 */
void  *arena_alloc_zero(Arena *a, size_t size);
char  *arena_strdup(Arena *a, const char *s);     /* owned by arena */
void   arena_reset(Arena *a);                     /* 释放全部块，可复用 */
```

生命周期契约：**一次 LLM Request 的数据（JSON、临时串、SSE 解析缓冲）用 Request Arena，请求结束 `arena_reset()`**；Agent/Session/Message 历史等长期对象禁止放入短生命周期 Arena。Arena 不提供单对象释放。

### 3.4 agent/message.h — Message 与 MessageList

```c
typedef enum { MSG_SYSTEM, MSG_USER, MSG_ASSISTANT, MSG_TOOL } MessageRole;

typedef struct {
    char *id;        /* owned; 模型返回的 tool_call_id */
    char *name;      /* owned; 工具名 */
    char *arguments; /* owned; 原始 JSON 参数字符串 */
    char *result;    /* owned; 工具输出（MSG_TOOL 时填充） */
    bool  is_error;  /* 工具是否报错 */
} ToolCall;

typedef struct {
    ToolCall *items;  /* owned */
    size_t len, cap;
} ToolCallList;

typedef struct {
    MessageRole  role;
    char        *content;      /* owned; system/user 文本，或 MSG_TOOL 的 result */
    char        *reasoning;    /* owned; assistant 推理文本，可 NULL */
    char        *tool_call_id; /* owned; 仅 MSG_TOOL */
    ToolCallList tool_calls;   /* owned; 仅 MSG_ASSISTANT */
    Usage        usage;        /* 仅 assistant；无则为 0 */
} Message;

typedef struct {
    Message *items;  /* owned */
    size_t len, cap;
} MessageList;

Message *message_new(MessageRole role);
void     message_free(Message *m);            /* 深度释放 */
int      message_set_content(Message *m, const char *text);  /* 复制 */
int      message_list_append(MessageList *list, Message *m); /* 接管 m 所有权 */
void     message_list_free(MessageList *list);
```

### 3.5 tool/tool.h — Tool / ToolResult / ToolContext

```c
typedef struct ToolResult {
    char *content;  /* owned by tool，调用后归调用方；失败时置错误说明 */
    bool  is_error;
} ToolResult;

typedef struct ToolContext {
    struct Agent  *agent;     /* borrowed; 可 NULL（单元测试） */
    struct Runtime *runtime;  /* borrowed */
    const char     *cwd;      /* borrowed; 工作目录，工具默认基准 */
    struct CancelToken *cancel; /* borrowed */
} ToolContext;

typedef struct Tool {
    const char *name;          /* static; 注册表键 */
    const char *description;   /* static */
    const char *input_schema;  /* static; JSON Schema 字符串 */
    uint32_t    flags;         /* 预留: 需审批/危险级别（Phase 2+） */
    int (*execute)(struct ToolContext *ctx, const char *arguments,
                   struct ToolResult *result);
} Tool;
```

`execute` 契约：`arguments` 为 borrowed（仅本次调用有效）；`result->content` 由工具 `malloc` 分配，**调用方负责 `free(result->content)`**；返回 `AGENT_OK` 或 `AGENT_ERR_*`。工具失败本身不导致 Agent 失败——错误作为 Tool Result 返回模型决策。

### 3.6 tool/registry.h — ToolRegistry

```c
typedef struct ToolRegistry {
    Tool  **tools;    /* owned 数组，元素 borrowed（静态工具） */
    bool   *enabled;  /* owned; 与 tools 平行 */
    size_t  len, cap;
} ToolRegistry;

ToolRegistry *tool_registry_new(void);
void tool_registry_free(ToolRegistry *reg);
int  tool_registry_register(ToolRegistry *reg, Tool *tool);   /* borrowed tool */
Tool *tool_registry_find(const ToolRegistry *reg, const char *name);
void tool_registry_set_enabled(ToolRegistry *reg, const char *name, bool on);
size_t tool_registry_count(const ToolRegistry *reg);
/* 生成 OpenAI 格式 tools 数组 JSON（写入 String，arena 或调用方所有） */
int tool_registry_schema_json(const ToolRegistry *reg, String *out);
```

### 3.7 model/model.h — Model / Provider / ModelRequest / ModelEvent

```c
typedef struct ModelOps {
    int (*request)(struct Model *model, struct ModelRequest *req);
    int (*cancel)(struct Model *model);
    void (*destroy)(struct Model *model);   /* 释放 model->priv */
} ModelOps;

typedef struct Model {
    ModelOps     *ops;            /* borrowed; 静态 vtable */
    struct Provider *provider;    /* borrowed */
    char         *name;           /* owned; 如 gpt-4.1-mini */
    int64_t       context_window; /* 模型上下文上限 */
    int64_t       max_output;     /* 输出保留 */
    void         *priv;           /* owned; provider 实现私有 */
} Model;

typedef struct Provider {
    char *base_url;  /* owned; 如 https://api.openai.com/v1 */
    char *api_key;   /* owned; 来自环境变量，禁止日志/TUI/session */
    char *api_key_env; /* owned; 环境变量名，如 OPENAI_API_KEY */
} Provider;

typedef struct ModelRequest {
    uint64_t        id;         /* 每次请求唯一 */
    struct Model   *model;      /* borrowed */
    MessageList    *messages;   /* borrowed; Agent 拥有 */
    struct ToolRegistry *tools; /* borrowed; 可 NULL */
    struct Agent   *agent;      /* borrowed; 回调目标 */
    int64_t         max_tokens;
    double          temperature;
    bool            stream;     /* 第一版恒 true */
} ModelRequest;

typedef enum {
    MODEL_EVENT_TEXT_DELTA,        /* assistant 文本增量 */
    MODEL_EVENT_REASONING_DELTA,   /* 推理增量 */
    MODEL_EVENT_TOOL_CALL_START,   /* 携带 index/id/name */
    MODEL_EVENT_TOOL_CALL_DELTA,   /* 携带 index + arguments 增量 */
    MODEL_EVENT_TOOL_CALL_END,     /* 携带 index，arguments 完整 */
    MODEL_EVENT_USAGE,             /* 统一 Usage */
    MODEL_EVENT_DONE,
    MODEL_EVENT_ERROR              /* 携带 code + message */
} ModelEventType;

typedef enum {
    MODEL_STOP_UNKNOWN = 0,        /* 兼容只发送通用 [DONE] 的网关 */
    MODEL_STOP_COMPLETE,
    MODEL_STOP_TOOL_CALLS,
    MODEL_STOP_MAX_TOKENS,
    MODEL_STOP_CONTENT_FILTER,
    MODEL_STOP_INCOMPLETE
} ModelStopReason;

typedef struct {
    ModelEventType type;
    uint64_t request_id;
    union {
        struct { const char *data; size_t len; } text;      /* borrowed; 仅本次回调有效 */
        struct { size_t index; const char *id; const char *name; } tool_start; /* borrowed */
        struct { size_t index; const char *delta; size_t len; } tool_delta;    /* borrowed */
        struct { size_t index; } tool_end;
        struct { Usage usage; } usage;                      /* 值拷贝 */
        struct { ModelStopReason reason; } done;             /* 值拷贝 */
        struct { int code; const char *message; } error;    /* message borrowed */
    } u;
} ModelEvent;
```

**回调约定**：Provider 通过回调把 `ModelEvent` 推给 Agent；事件内的 borrowed 字符串**仅在回调返回前有效**，Agent 需要留存时必须复制。Agent Core 不接触 Provider 特有的 SSE JSON——OpenAI `finish_reason`、Anthropic `stop_reason` 和 Responses `incomplete_details.reason` 均在 Provider 内归一化为 `ModelStopReason`。兼容网关省略原因时保留 `UNKNOWN`；Agent 仅在 usage 输出 token 同时达到本次请求上限时使用截断兜底。

### 3.8 agent/agent.h — Agent / AgentState / CancelToken

```c
typedef enum {
    AGENT_READY,
    AGENT_WAIT_MODEL,   /* 等待模型响应（异步 IO 挂起点） */
    AGENT_WAIT_TOOL,    /* 等待工具结果 */
    AGENT_WAIT_CHILD,   /* 等待子 Agent（Phase 5） */
    AGENT_WAIT_USER,
    AGENT_DONE,
    AGENT_ERROR,
    AGENT_CANCELLED
} AgentState;

typedef struct CancelToken {
    bool                  cancelled;  /* Event Loop 线程读写（Phase 1 同步原型无锁） */
    struct CancelToken   *parent;     /* borrowed; 取消向子传播 */
    uint64_t              generation; /* 防 ABA / 复用计数 */
} CancelToken;

typedef struct AgentConfig {   /* 值语义；创建时复制 */
    const char *system_prompt; /* borrowed 或 owned（见 agent_set_system） */
    const char *model_name;    /* borrowed; 查表选择 Model */
    const char *cwd;           /* borrowed; 工作目录 */
} AgentConfig;

typedef struct Agent {
    uint64_t        id;
    struct Runtime *runtime;   /* borrowed */
    struct Model   *model;     /* borrowed; 可共享 */
    MessageList     messages;  /* owned */
    struct ToolRegistry *tools; /* borrowed */
    struct Session *session;   /* borrowed; 可 NULL */
    CancelToken     cancel;    /* owned; parent 由 spawn 时链接 */
    AgentState      state;
    struct Agent   *parent;    /* borrowed; 可 NULL */
    struct AgentConfig config; /* owned 副本 */
    /* 调度器簿记（Phase 4）：next/pending 链 */
} Agent;

Agent *agent_new(struct Runtime *rt, const AgentConfig *cfg);
void   agent_destroy(Agent *a);           /* 深度释放 messages/config */
int    agent_run(Agent *a, const char *user_input); /* 同步原型入口（Phase 1） */
```

### 3.9 错误码（util/error.h）

```c
typedef enum {
    AGENT_OK = 0,
    AGENT_ERR_OOM,
    AGENT_ERR_IO,
    AGENT_ERR_JSON,
    AGENT_ERR_HTTP,
    AGENT_ERR_MODEL,
    AGENT_ERR_TOOL,
    AGENT_ERR_PROCESS,
    AGENT_ERR_CANCELLED
} AgentError;
```

底层禁止 `exit(1)`；错误向上冒泡，由上层决定 retry / 报告 / 取消。

### 3.10 Runtime（Phase 1 同步原型）

```c
typedef struct Runtime {
    Provider      provider;   /* owned; openai 兼容 */
    Model        *models;     /* owned; 配置的模型表（按名查找） */
    ToolRegistry *tools;      /* owned */
    struct Config config;     /* owned（见 §4） */
    /* Phase 4 追加: EventLoop *loop; WorkerPool *pool; Scheduler *sched; */
} Runtime;
```

Agent 只持有 `Runtime*`（borrowed），Runtime 由 main 创建并拥有。

### 3.10 Context Compaction（Phase 3/4 实现）

当 `context_needs_compact()` 判定估算 token 加上输出 reserve 超过窗口时，Agent 不在请求前同步调用模型，也不先删除历史。`context_compaction_prepare()` 计算「保留 system + 最近 10 条、压缩中间段」的范围，并生成最多 256KiB 的临时 transcript `MessageList`。

摘要是一次普通的异步 `ModelOps.request()`，但 `ModelRequest.is_compaction=true`、`tools=NULL`、request id 独立；Agent 保持 `AGENT_WAIT_MODEL`，由已有 Event Loop pump 和 `agent_resume()` 驱动。摘要 callback 只写 loop state 的临时 `String`，不得直接修改正式会话或重入主请求。成功且非空时，先插入摘要再删除旧范围；插入 OOM 时原范围保持不变。

摘要请求错误、空响应、构造失败或取消时使用 role-histogram 确定性 fallback。fallback 不是安全失败：它保留 system 与最近消息，并记录省略数量。成功的 LLM 摘要和 fallback 都通过 append-only JSONL `compaction` 事务持久化 `{start,count,summary}`；resume 加载时按事务顺序重建压缩视图，不把摘要简单追加到对话末尾。每个 compaction 形成同一 Session 内的新上下文周期，下一次请求直接携带“摘要 + 最近消息”；不会创建割裂 plan/memory/审计链的新 Session。工具调用不设固定轮数上限，由 context window 和重复 compaction 推进，失控执行由用户取消与副作用审批边界控制。取消始终按独立 request id 处理，不能取消同一 Model 上的其他 Agent 请求。

### 3.11 Responses API Provider

Responses 请求把 `MSG_SYSTEM` 和 `system_prompt` 合并到顶层 `instructions`，把用户/助手文本编码为 `input` message items，把 assistant tool call 编码为 `function_call`，把工具结果编码为 `function_call_output`。当前只声明 function tools，并使用 `max_output_tokens`、`stream` 和 delta `include`。

Provider 归一化 `response.output_text.delta`、`response.function_call_arguments.delta`、`response.output_item.added`、usage、`response.completed`、`response.incomplete`、`response.failed` 和 `error`。`response.completed`/`response.incomplete` 是带语义的终止事件，`incomplete_details.reason=max_output_tokens` 映射为 `MODEL_STOP_MAX_TOKENS`；`[DONE]` 仅作未知原因的网关兼容 fallback，必须通过 done guard 防止重复 DONE/tool-end。函数调用的 `call_id` 是跨轮次的权威 ID，不能用 `item_id` 替代。

不在当前 ModelRequest 能力范围内的 Responses 专有功能（reasoning 配置、web/file/computer tools、图片/文件 input、previous_response_id、非函数 tool_choice、结构化 output schema）明确保持未映射，并通过确定性错误/fallback 处理，不伪装成已支持。

### 3.12 ncursesw TUI Backend

TUI 采用 backend-only 迁移：`TuiModel`、`tui_render_screen()` 和 Agent/EventLoop 事件协议不依赖 ncurses；`src/tui/terminal.c` 负责 ncursesw 初始化、raw mode、尺寸、光标和绘制，非 TTY 时不初始化 curses，因此纯布局和 pipe 单测保持可运行。

真实 TUI 通过 CMake `FindCurses` 链接 wide-character ncurses，并在 Nix devShell/package 的 `ncurses` 依赖中声明 terminfo 运行时前提。布局固定为 header、可滚动的中间消息 viewport、status/input 两行 footer；新消息默认跟随底部，Up/Down/PageUp/PageDown 改变 viewport 偏移。输入仍由 EventLoop 读取并交给现有状态机；后续可单独改进 UTF-8/wcwidth 与脏区刷新，不能把 ncurses 依赖本身视作这些功能已经完成。

---

## 4. 配置与 CLI（Phase 1 范围 + 多模型 + 多协议）
- **三协议 Provider + 统一认证**（DESIGN.md §7）：OpenAI Compatible（chat/completions）+ Anthropic Messages（/v1/messages，tool_use 块、tool_result 消息）+ OpenAI Responses（/responses，input items、output delta、function call）；鉴权统一来自 `~/.config/cagent/auth.json`，通过 `"type": "api_key"|"oauth"` 区分。内置 provider 为 `opencode-go`、`openai`、`anthropic`、`chatgpt`；自定义 provider 放在 `~/.config/cagent/providers.json`。`models[]` 每项可设 `provider` 和 `protocol`；label 可作 `--model`/`/model` 选择别名。
- 默认 provider：OpenCode Go 订阅（内置 endpoint `https://opencode.ai/zen/go/v1`）；内置 provider 还包括 OpenAI、Anthropic、ChatGPT Codex。自定义 provider 在 `~/.config/cagent/providers.json` 中声明 `base_url`/`protocol`/`models_path`。
- 多模型表（DESIGN.md §7）：`config.json` 的 `models[]` 每项可含 `name`/`label`/`provider`/`base_url`/`protocol`/`context_window`/`max_output`；鉴权统一由 `~/.config/cagent/auth.json` 提供；`runtime_model_by_name()` 按名字或 label 查找。

- 配置优先级：**CLI > 环境变量 > `~/.config/cagent/config.json` > 默认值**。
- 鉴权统一使用 `~/.config/cagent/auth.json`（原子写入、0600）；API key 和 OAuth 令牌禁止进入日志、TUI、Session 和错误文本。
- `cagent --login` 使用授权码 + PKCE；`--device-code` 使用官方设备码流程；OAuth 当前支持 ChatGPT Codex provider。
- 所有 provider 每次启动通过其模型目录 endpoint 实时获取模型，不持久化缓存；服务端不可达时保留已有静态配置作为降级。
- CLI：`cagent`（默认）、`cagent -C <dir>`、`cagent --model <name>`、`cagent --resume <id>`（Phase 3）、`-p "<prompt>"`（后续）。第一版用 `getopt_long`，不引入 argp 依赖。

## 5. 关键设计决策记录
| 决策 | 理由 | 违反时的影响 |
|---|---|---|
| Agent 是轻量状态机，不绑定线程/进程 | 大量并发 SubAgent 的核心前提 | 资源占用随 Agent 数线性爆炸 |
| Event Loop 独占可变状态 | 从架构上消除锁 | 需要全对象加锁，复杂度爆炸 |
| libcurl multi + epoll（Phase 4） | 单进程支撑上百并发流 | 每 Agent 一线程的退化 |
| Agent Core 只消费归一化 ModelEvent | Provider 可替换（Anthropic/Gemini） | Provider 格式泄漏进 Core |
| 工具静态注册（编译期） | 扩展即加 .c/.h + 一行注册 | 插件系统复杂度 |
| String/Arena/统一错误码先行 | 所有模块的公共地基 | 到处裸 malloc/exit，无法审计 |
| yyjson 封装在 util/json.h | 未来可换实现 | 全局耦合第三方 API |

## 5.1 Nix 构建环境已知问题与处理（已实测验证）

| 问题 | 根因 | 处理 |
|---|---|---|
| nixpkgs 的 `yyjson.pc` 不可用（libdir 前缀重复拼接） | nixpkgs 打包 bug | `find_package(yyjson CONFIG)` 用官方 CMake config |
| imported target 的 include 不进 compile_commands.json | clang-wrapper 把 NIX_CFLAGS_COMPILE 注入编译器，CMake 把这些目录记为**隐式 include** 并静默去重显式同名目录 | 用 `-isystem<dir>` 编译选项注入（路径来自 find_package，绕过隐式去重；非 Nix 环境缺失的 -isystem 目录被编译器静默忽略，无害） |
| clang-tidy 找不到 glibc 头 | clang-tools 的裸二进制没有 cc-wrapper 的 include 注入 | devShell 设置 `C_INCLUDE_PATH=${pkgs.glibc.dev}/include`（clang driver 尊重该变量） |
| clang-tidy 的 `-clang-analyzer-security.insecureAPI.DeprecatedOrUnsafeBufferHandling` 无法按精确名禁用 | clang-tidy 怪癖（glibc 无 Annex K，该组纯噪音） | 不启用 `clang-analyzer-security.*` 组，只精确启用 core/deadcode/nullability/unix |
| Nix 2.34 的 indented string 中 `\${` 转义失效 | Nix 版本行为变化 | shellHook 避免 `${var:+...}` 语法，改用 if/else 拼接 |
| CMake 4.x 无 install() 规则时不生成 install target | 行为变化 | 项目始终提供 `install(TARGETS ...)` |
| devShell 编译器默认是 gcc，与 clangd/clang-tidy 不一致 | CMake 默认优先 GNU | shellHook `export CC=clang`（nix build 不受影响，仍用 stdenv） |
| 严格 C17 下 strdup 等 POSIX 函数未声明 | `CMAKE_C_EXTENSIONS OFF` 不带 GNU 扩展 | 全局 `_POSIX_C_SOURCE=200809L`（项目本就 POSIX-only） |
| yyjson 0.12 API 与旧版不同 | 版本演进 | `yyjson_get_int64`→`yyjson_get_sint`（且仅对 int 有效，real 需经 `yyjson_get_real`）；`yyjson_mut_write` 返回值用 `free()` 释放（无 `yyjson_free`）；`yyjson_mut_obj/arr` 创建的值必须 `yyjson_mut_doc_set_root` 才会被写出 |
| clang 21 对未初始化 const 数组告警（`-Wdefault-const-init-var-unsafe`） | 新警告 | 测试代码去掉 const（或显式初始化） |
| `&(ModelOps){...}` 复合字面量赋给 ops 指针 | 复合字面量在语句块结束后悬空（C 经典陷阱） | vtable 用 `static const` 全局 |
| `data: [DONE]` 无法识别 | `"[DONE]"` 是 6 字节不是 5 | 长度判断用 `value_len == 6` |
| SSE 收到 `[DONE]` 后继续 feed 报错 | 尾部空行是正常流的一部分 | done 后丢弃输入并返回 OK |
| clang-analyzer `unix.Malloc` 对 registry 扩容路径误报 "size zero" | analyzer 无法跟踪 cap>=8 不变量 | 行内 `NOLINTNEXTLINE(clang-analyzer-unix.Malloc)` + 40 工具扩容测试（ASan 验证） |
| realloc 双指针分配（tools+enabled）第二个失败时第一个悬空 | 经典 realloc 误用 | 逐个 realloc 并立即更新指针；`SIZE_MAX` 溢出检查 |
| `message_list_append` 浅拷贝后源对象泄漏；列表元素被 `message_free` 误释放（元素属于数组，不能 free） | 生命周期语义混淆 | append 接管所有权（拷贝进数组后 `free` 壳，失败不接管）；拆 `message_deinit`（仅释放内部）与 `message_free`（含对象本身）；**append 后禁止再用源指针**（用 `message_list_last`） |
| 工具的 `json_obj_get_str` 结果在 `json_doc_free` 后悬空（路径乱码） | yyjson 字符串内联存储在文档内 | 文档延迟到工具函数结束再释放；错误路径同样补 `json_doc_free` |
| 模型错误消息乱码 | `ctx.error_msg` borrowed 指针指向 provider 局部 String，request 返回后悬空 | `LoopCtx` 内 `char error_msg[512]` 固定缓冲复制 |
| `posix_spawnattr_setcwd_np` 在 glibc 2.35+ 移除 | glibc API 演进 | 改用 `posix_spawn_file_actions_addchdir_np` |
| 结构体与代码不同步（`error_msg` 指针 vs 数组）导致 snprintf 崩溃 | patch 工具部分失败 | 编译失败时禁止继续开发（先修复再前进）；构建后必须 `ctest` 验证 |
| `posix_spawnattr_setpgroup(0)` 不新建进程组（POSIX 语义=继承父组），`kill(-pid)` 报 No such process | POSIX 标准行为 | fork + 子进程 `setpgid(0,0)` 后 exec（Phase 1 单线程安全；Phase 4 需 pthread_atfork 或竞态处理）；组 kill 失败退化单进程 kill |
| `message_list_move_range` 复用 append（会 free 壳）导致 free 数组内部地址 | append 的接管契约与 move 语义冲突 | 拆 `message_list_append_copy`（结构拷贝不接管）+ append = copy+free；move_range 用 copy 后清零源 |
| compaction 把摘要追加到末尾而非中间 | 语义错误 | `message_list_insert`（指定位置插入，同样先拷贝再 realloc） |
| `estimate + reserve > window` 在小窗口下必然触发 | reserve 未钳制 | reserve = min(OUTPUT_RESERVE, window/2) |
| HTTP/1.0 close-delimited 响应（无 Content-Length）下 curl multi 偶发 CURLE_ABORTED_BY_CALLBACK | curl 需读到 EOF 判定完成，multi 事件驱动下边界不稳 | 服务端发送 Content-Length（OpenAI 等生产服务均如此）；测试 server 已修正 |
| `WRITEFUNCTION` 无 return（清理脚本误删）导致返回值随机 → 偶发 WRITE_ERROR/ABORT | C 函数无 return 是 UB | 编译失败/改动后必须完整 `ctest` 回归 |
| 请求完成后 curl 不再回调 `CURL_POLL_REMOVE`，SocketWatcher 泄漏 | curl_easy_cleanup 后 socket 回调停止 | HttpRequest 自管 watcher 列表（创建入列、REMOVE 出列、finish 兜底清理） |
| fake-ip/TUN 下 libcurl multi 的 resolver completion 30s 超时（curl CLI 的 libc/NSS 路径正常） | multi + resolver 后端与 TUN 组合的事件唤醒不可靠 | 无显式代理时由 HTTP runtime 用 libc `getaddrinfo()` 预解析并设置 `CURLOPT_RESOLVE`；显式代理环境保持 libcurl 默认行为 |
| curl 回调中调 `curl_multi_socket_action` 重入（socket 回调内再 http_pump） | libcurl multi 不可重入 | socket 回调只做 action；完成消息由顶层 http_pump 统一处理 |
| SocketWatcher 挂在 HttpRequest 上：请求完成时误清理连接池共享 watcher（double free） | watcher 是 socket 级资源，同一 keep-alive 连接被多请求共享 | watcher 管理移到 HttpRuntime 级（socket→watcher 列表，REMOVE 释放，http_free 兜底） |
| `agent_spawn` 的子 agent 用 runtime 默认 model 而非继承父的（mock 测试环境暴露） | agent_new 从 runtime 取 model | spawn 时 `child->model = parent->model` |
| subagent 工具循环只 pump 不 resume 子 agent（子 agent 永远停在 WAIT_MODEL） | agent_start 只做首次推进 | 循环内先 `agent_resume(child)` 再 `runtime_pump` |
| Ctrl+C（信号路径）只退出不取消运行中的 agent | 主循环只处理了空闲退出 | 忙时 SIGINT → `cancel_token_cancel` + "cancelling..." 状态；空闲时退出（DESIGN.md §29 语义） |
| pty 测试中 script 命令的 \003 时序不可靠（信号在 raw mode 前到达） | script 立即灌入输入 | 用 python pty.fork 做可控交互验证（输入/渲染/取消/退出全链路） |
| fake-ip 透明代理下 c-ares 直连外部 nameserver 超时（curl CLI 的 glibc 路径正常） | 环境网络 | HTTP runtime 无显式代理时预解析并设置 `CURLOPT_RESOLVE`；用户无需设置代理；解析失败才回退 libcurl 默认行为 |
| OpenCode Go 模型 ID 带 `opencode-go/` 前缀被 API 拒绝（401） | 前缀是 opencode 内部 TUI 格式 | 配置用 API 目录 `/zen/go/v1/models` 返回的裸 ID |

## 6. 路线图（对应需求 §84-§90）
- **Phase 1（当前）**：Flake+CMake 骨架 → util（String/Arena/Buffer/Vector/HashMap/JSON/log）→ libcurl 同步请求 → SSE Parser（任意分片）→ OpenAI Compatible Provider + Mock Model → Message → Tool Registry → Agent Loop → read/write/bash 工具 → 简单 CLI。验收：`User → LLM → Tool → Result → LLM → Final` 可实际跑通。
- **Phase 2**：edit/grep、工作目录语义、进程管理强化（超时/取消/SIGCHLD 无僵尸）、usage、日志文件。
- **Phase 3**：Session JSONL 持久化、resume、context limit、LLM compaction。
- **Phase 4**：libcurl multi + epoll Event Loop + 非阻塞 pipe + Worker Pool + Scheduler（max_concurrent_agents）。
- **Phase 5**：SubAgent（spawn/cancel/collect）、subagent 工具、并发测试（10/20/50/100）、资源指标记录。
- **Phase 6（已完成基础迁移）**：TUI（ncursesw backend + 纯 TuiModel/布局层，EventLoop 驱动）；UTF-8/wcwidth 与脏区刷新仍是后续优化。
- **Responses API（已完成基础实现）**：新增 `/responses` provider、归一化文本/函数调用/usage/error 事件和本地 SSE 测试；Responses 专有能力仍未映射。
- **ChatGPT OAuth（已完成基础实现）**：官方 Codex issuer 授权码/PKCE、loopback 回调、设备码、刷新令牌和 ChatGPT Codex Responses endpoint；OAuth 端点与请求头属于官方 Codex 产品协议，需关注未来变更。
- **通用模型发现（已完成基础实现）**：所有内置和自定义 provider 每次启动实时请求 `/models`（ChatGPT 使用 Codex 专用 query/header），兼容 `models[]` 与标准 OpenAI `data[]` 响应，解析 id/slug/display_name/context_window；不保存目录缓存。
- **Phase 7（已完成测量）**：valgrind 全量干净；微基准（bench/bench.c，Release）：消息序列化 **1.8 GB/s**（20 条消息 1.3µs/轮、200 条 13µs/轮）、SSE 解析 **53ns/chunk**（含 parser 创建+销毁）。相对 LLM 秒级延迟可忽略 → **无需优化**，符合 DESIGN.md §93（C 的 overhead 应微秒级，价值在内存/并发/IO 控制而非推理加速）。
