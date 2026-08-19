# cagent 项目进度总览

> 最后更新：2026-08-17。本文件是压缩/新会话后的恢复入口：先读本文件，再按需读 `docs/DESIGN.md`（架构基线）与 `README.md`（使用说明）。

## 项目定位

轻量级、纯 C（C17）、事件驱动的 AI Coding Agent Runtime。参考 Pi Coding Agent 的架构思想，用原生 Linux/POSIX 重新实现。无 Node/TS/Python 运行时依赖、无插件系统——扩展 = 新增 `.c/.h` 模块 + 一行注册重编译。

**核心验证过的能力**：100 个并发 SubAgent 仅 1 线程 / ~10MB RSS / 9 fds（单进程内共享 libcurl multi + epoll，Agent 挂起不占线程）。

## 完成状态（Phase 1-7 + 三协议 Provider + 日常 vibe coding 体验）

| 阶段 | 交付 | 验证 |
|---|---|---|
| 骨架 | Nix Flake devShell（clang/cmake/ninja/curl/yyjson）、CMake+Ninja、clang 统一工具链 | 全链路可构建 |
| util | String/Buffer/Arena/Vector/HashMap/JSON 封装/日志/错误码 | 单测全绿 |
| Model | SSE 解析（任意分片）、三协议终止原因归一化、OpenAI Provider、Tool Registry | 本地 server e2e |
| Agent | 消息模型、状态机 Loop、输出截断有界自动续跑、实时文本 delta、read/list/find/grep/write/edit/bash/subagent 工具、Runtime/CLI | mock 驱动闭环 |
| 异步 | epoll Event Loop + libcurl multi + Scheduler、Agent/Tool 挂起恢复、异步 ProcessTask | 慢工具不阻塞其他 Agent、超时/取消/回收验证 |
| SubAgent | agent_spawn 父子链、并行 subagent 工具、取消传播 | 100 Agent = 1 线程 |
| TUI | ncursesw 后端 + 纯 TuiModel/布局层 + EventLoop 事件驱动界面（Core 零耦合） | pty 启动/退出 + 既有 TUI 单测 |
| 优化 | valgrind 全净、微基准 1.8GB/s 序列化 | 无需优化 |
| **多模型/认证** | models[] 表（name/label/provider/base_url/protocol）、按名/label 查找、统一 auth.json（api_key/oauth）、内置/自定义 provider 实时发现 | test_models/test_oauth |
| **Anthropic Provider** | Messages API（tool_use/tool_result/SSE 流映射）、多协议 | test_anthropic + 真实联调 |
| **LLM Context Compaction** | 异步独立摘要请求、request_id 取消、失败 fallback、原子替换、JSONL compaction 事务恢复 | test_context/test_agent_loop/test_session + ASan |
| **Responses API Provider** | `/responses` SSE 文本/函数调用/usage/error、request_id 取消、三协议分派 | test_responses + 全量 ctest |

## 当前架构速览

```
Agent(轻量状态机, agent_start/agent_resume/agent_run)
  ├─ ModelRequest {messages: MessageList*} → Provider 各自序列化 wire format
  │    ├─ openai.c    → POST {base}/chat/completions (OpenAI 格式)
  │    ├─ anthropic.c → POST {base}/v1/messages (Messages 协议)
  │    └─ responses.c → POST {base}/responses (Responses 格式)
  ├─ ToolRegistry (read/write/edit + async bash/grep/subagent)
  │    └─ ToolTask(start/poll/cancel/destroy) → ProcessTask(nonblocking pipes + timerfd)
  ├─ Session (JSONL 持久化, --resume)
  ├─ CancelToken (父子链传播；Model request_id 精确取消)
  └─ Runtime: EventLoop(epoll) + HttpRuntime(libcurl multi) + Scheduler(max_concurrent)
```

关键文件：
- `src/agent/loop.c` — Agent 状态机（挂起点：WAIT_MODEL/WAIT_TOOL）
- `src/runtime/http.c` — libcurl multi over epoll（socket_cb 注册、http_pump 驱动）
- `src/runtime/process.c` — EventLoop 异步子进程（非阻塞 pipe、timerfd 超时、进程组取消/回收）
- `src/model/openai.c` / `src/model/anthropic.c` / `src/model/responses.c` — 三协议 Provider + request_id 级在途请求取消
- `src/tool/subagent.c` — 并行子 Agent（异步 poll，无嵌套 runtime_pump）
- `src/tui/` — TUI（terminal/render/tui）

## 配置与真实联调（OpenCode Go）

默认 provider：OpenCode Go 订阅，`https://opencode.ai/zen/go/v1`。鉴权优先放在权限 0600 的 `~/.config/cagent/auth.json`；`api_key_env` 仅保留旧配置兼容。

**网络**：HTTP runtime 在未设置显式代理时，使用 libc/NSS `getaddrinfo()` 预解析主机，并通过 `CURLOPT_RESOLVE` 将全部地址交给 libcurl，绕过 multi + resolver completion 在 fake-ip TUN 下的 30 秒卡死；设置显式 `http_proxy/https_proxy/all_proxy` 时保持 libcurl 原有代理行为。用户不需要为透明代理配置代理环境变量。已用当前 OpenCode Go 配置完成直连真实 API 联调。

**模型 ID 不带 `opencode-go/` 前缀**（API 目录 `/zen/go/v1/models` 返回裸 ID，前缀会 401）。

已实测（全部真实 API + 工具闭环）：
- OpenAI 协议：`deepseek-v4-flash`、`kimi-k3`、`glm-5.1`
- Anthropic 协议：`minimax-m3`、`qwen3.7-max`（`models[]` 标 `"protocol": "anthropic"`）

```bash
./build/cagent --model minimax -p "..."   # label 或模型名
./build/cagent                             # TUI（/settings /key /model /models /help）
```

## 本次进展：footer 状态栏 + 长会话无缝自动压缩（2026-08-17）

已完成：
- footer 状态行右侧新增 Claude Code 风格用量显示（右对齐、纯文本、可测试）：`↑1.1M ↓98k (hit 63.6%) $0.224 57.2%/128k (auto)`。`↑/↓` 为本会话累计输入/输出 token，`(hit %)` 为缓存命中占输入比（三 provider 已解析：OpenAI `prompt_tokens_details.cached_tokens`、Anthropic `cache_read_input_tokens`、Responses `input_tokens_details.cached_tokens`），`$` 为按模型单价折算费用，`57.2%/128k` 为上下文占用百分比/窗口大小，`(auto)` 表示自动压缩全程自动进行——长会话无需用户介入。
- `struct Model` 新增可选计费元数据：`input_price`/`output_price`（每 1M token 的美元价）、`subscription`（订阅制模型显示 `(sub)` 替代 `$`）。配置顶层与 `models[]` 条目均支持 `price_in`/`price_out`/`subscription` 键，模型切换（/model）自动跟随新模型的单价。
- 用量刷新时机：模型请求完成（AGENT_EVT_TOOL_END/ERROR）、整轮结束、agent 启动与启动时——不随 50ms pump 重绘。
- `tui_format_usage()` 为纯函数（k/M 缩放、`2.0M→2M`、`<1 美分保留 4 位小数`、未知段自动隐藏、截断返回错误）；`test_tui` 新增格式化与右对齐渲染两组用例。
- 长会话底座（本轮前已就位）：compaction 事务与消息同存 Session JSONL，resume 后自动恢复“摘要 + 最近消息”视图；tool 调用边界内安全压缩（`message_list_tool_safe_prefix_count`），截断上限按“连续截断段”计数。
- **context_window 跟随模型下发目录**：默认模型（rt->model）在目录刷新后原地回填下发值（裸名或 provider/裸名匹配），footer 与 compaction 阈值不再停留在本地默认 128k；目录应用完成时刷新一次状态栏。`test_models` 新增回归：目录应用后默认模型 context_window/max_output/计费字段更新、目录缺失的模型保持原值。
- **窗口值来源权威性标记**：footer 未经验证的本地默认窗口显示为 `~128k`（目录/`models[]`/顶层配置下发过的值不带 `~`）。顺带修复顶层 `context_window`/`max_output` 配置键从未被 JSON 解析的隐藏 bug（此前只有 `models[]` 条目和目录下发生效），新增 `Config.context_window_set` 标记。优先级：目录下发 > 显式配置 > 本地默认。
- **崩溃修复：`&app`/`app` 传参错误**——`app_discovery_cb` 中 `app` 是指针却传了 `&app`（`App**` 当 `App*`），把 App 结构当 Agent 解引用导致 discovery 成功后 `context_estimate_tokens` 段错误（coredump 三连崩）。已改为传 `app`；`tui_run` 中 `app` 是值，两处 `&app` 保持不变。
- **“stream ended before [DONE]”误报修复**：部分网关（如 opencode-go）HTTP 200 正常结束但省略终止哨兵（`[DONE]`/`message_stop`/terminal event），此前三个 provider 一律报错，导致回答完整却显示 “agent finished with an error”。现改为：流内已有终止证据（`finish_reason`/`stop_reason` 已解析，或 usage 已到达）时按正常结束收束（关闭未结 tool call 后 emit_done）；仅有零散 delta 无任何终止证据时仍报错（真断流）。新测试覆盖三条路径：无 `[DONE]`+finish_reason=正常、无 `[DONE]`+仅 usage=正常、仅 partial delta=报错。

## 本次进展：TUI 输入区光标可见（2026-08-17）

已完成：
- 渲染器本就为输入行计算 `cursor_row/cursor_col`（空输入在 `> ` 后、按 UTF-8 显示宽度、secret 输入按掩码 bullet 计数、长输入钳制在列宽内），但 ncurses 后端一直以 `curs_set(0)` 隐藏光标。
- `terminal_draw_screen` 在把物理光标移到输入行时调用 `curs_set(1)`，位置无效（极小终端）时恢复隐藏；`hide/show/restore` 同步维护可见性状态，避免重复 `curs_set` 调用。
- pty smoke 验证：绘制一屏后捕获到 `\x1b[?25h`（光标显示）序列，光标确实出现在输入区。
- `test_tui` 新增 `test_input_cursor_placement`：空输入/行中 ASCII/多字节 CJK（按 wcwidth 自洽断言）/超长钳制/secret 掩码五种光标列位置。

## 本次进展：输出截断不会误报完成（2026-08-17）

已完成：
- `ModelEvent` 新增统一 `ModelStopReason`；OpenAI Chat 解析 `finish_reason`，Anthropic 解析 `message_delta.delta.stop_reason`，Responses 处理 `response.incomplete`/`incomplete_details.reason`。
- `max_tokens`/`max_output_tokens` 截断不再走“无 tool call 即 DONE”：Agent 显示续跑状态并自动继续同一用户任务，每段连续截断最多 3 次；完成一次正常响应或工具调用后重新计数，仍连续截断时明确进入 `AGENT_ERROR`。
- 兼容网关未提供终止原因时，仅当 usage 输出 token 达到本次请求上限才按截断处理；content filter 和其他 incomplete 终止明确报错。
- 截断期间产生的半截 tool call 会被丢弃并要求模型从头生成，绝不进入工具执行路径。
- `test_agent_loop` 覆盖空正文截断自动续跑、连续 3 次上限、工具进展后计数重置和半截写工具不执行；三组 Provider e2e 覆盖各自的截断原因映射。
- 移除固定 `max_tool_rounds`：工具链长度由模型 `context_window` 与自动 compaction 驱动；压缩事务在同一 Session 内持久化，下一请求直接使用“摘要 + 最近消息”继续任务。

## 历史进展：顺畅 vibe coding（2026-08-16）

已完成：
- 模型文本 delta 从 Provider 直达 AgentEvent；TUI 将 delta 合并为连续助手消息，工具调用前的说明也实时可见。
- `--resume` 在新 TUI 初始化时回放用户、助手、工具调用与结果标记，不再出现“模型记得但屏幕为空”。
- Tool 增加只读 approval preview：write/edit 显示有界行级变更，bash 显示命令/cwd/超时/风险提示，git 写操作显示 staged 或目标摘要。
- 交互式 `-p`/`--plain` 支持同步 y/N 审批；非 TTY/EOF 保持 fail-closed。
- 新增 workspace 内 `list/find`；加载 Git 根到 cwd 的 AGENTS/CLAUDE 指令与根 PROGRESS.md，并修复 OpenAI Chat 丢弃 system_prompt；子代理继承项目规则。
- OpenAI/Anthropic 对 2xx 干净 EOF 缺终止事件返回明确错误；Anthropic 兼容 `[DONE]`；零 delta 瞬时错误默认以非阻塞指数退避重试 5 次并显示状态；早期版本曾设置单用户回合 24 个工具轮次，现已改为上下文驱动。
- 模型发现只探测当前及显式配置 provider，等待上限从 35 秒降至 8 秒；目录缺少显式模型时先告警再回退，并正确归一化 TUI 持久化的 `provider/model` 选择器。
- TUI 顶栏与 `/session` 暴露恢复命令；`-p`/`--plain` 的同步驱动检查 SIGINT，取消只终止当前回合，后续可在同一 session 继续。
## 历史进展：长阻塞工具异步化（2026-08-14）

已完成：
- `Tool` 增加可选异步 `start()`，返回 owned `ToolTask`；统一 `poll/cancel/destroy` 生命周期。同步工具接口继续兼容。
- `ProcessTask` 将 stdout/stderr 非阻塞 pipe 与 timeout timerfd 注册到共享 EventLoop；`process_run()` 保留为同步兼容包装。
- `bash`、`grep`、`subagent` 已迁移到异步任务：慢命令期间其他 Agent 可继续完成；grep 保留 rg→grep fallback；subagent 不再在工具内部嵌套 `runtime_pump()`。
- Agent 在 `AGENT_WAIT_TOOL` 挂起/恢复，多个 tool call 仍按原语义串行执行；修复追加 Tool Result 后 `MessageList` realloc 导致的指针失效及多 tool-call 游标错误。
- Model 取消改为 `cancel(model, request_id)`；OpenAI/Anthropic 按模型维护在途 Transfer 表，取消/销毁不会误伤共享同一 Model 的其他 Agent，也不会在 Agent 销毁后留下回调 UAF。

实现注意事项：
- **不要在 EventLoop 回调里 free `ProcessTask`/watcher**；回调只更新状态和 drain fd，释放必须在后续 `poll/destroy` 阶段。
- `ToolContext` 只在 `execute/start` 调用期间有效；异步任务必须复制后续需要的 cwd/参数等数据。
- Tool Result 追加可能 realloc `MessageList`；跨 append 不得保留 `Message*`/`ToolCall*`，用稳定索引重新获取。
- Model 是多 Agent 共享对象；取消必须始终带 request_id，禁止恢复为“取消整个 model 当前请求”的单槽状态。
- `subagent` 只能由外层 runtime pump 驱动；禁止在 tool poll/execute 内再次调用 `runtime_pump()`，否则会重入 parent 的同一个 tool call。
- 当前 `fork+exec` 发生在单线程 EventLoop 上；将来若引入 Worker Pool，必须先重新评估 fork 后 async-signal-safe 约束（或改用可靠的 spawn/setpgid 方案）。

## 本次进展：LLM Context Compaction（2026-08-14）

已完成：
- 在 Agent 的 `AGENT_WAIT_MODEL` 状态内增加独立摘要请求；摘要与正常请求使用不同 request_id，摘要完成前不会启动正常请求，也不会在模型回调中重入 Agent。
- `context_compaction_prepare()` 只生成有界的 transcript prompt，不修改 live MessageList；摘要成功后才把摘要插入中间并删除旧范围。
- 摘要请求错误、空响应、构造失败或取消时保留确定性 role-histogram fallback；插入先于删除，OOM 不会丢失原历史。
- Session JSONL 新增 append-only `compaction` 事务，加载时按 `start/count` 重建压缩后的视图，避免把摘要错误追加到对话末尾。
- mock model、异步暂停/继续、fallback、取消和 session round-trip 测试已覆盖。

## 本次进展：Responses API 与 ncursesw TUI（2026-08-14）

已完成：
- 新增 Responses API provider：请求序列化为 `input`/`instructions`/`function_call_output`，支持文本 delta、函数调用参数 delta、usage、completed/failed/error、HTTP 错误和 request_id 取消。
- `models[].protocol` 和顶层 `protocol` 支持 `responses`；Responses provider 保持与其他 Provider 相同的 ModelEvent/异步传输契约。
- TUI 终端后端切换为 ncursesw；保留 `TuiModel`、纯布局渲染和 Runtime/EventLoop，界面固定为 header + 中间消息 viewport + status/input footer，支持自动跟随最新消息以及 Up/Down、PageUp/PageDown 滚动；非 TTY 测试不初始化 ncurses。
- 新增 Responses 序列化/SSE/HTTP 错误测试，并完成 ncursesw pseudo-terminal 启动/退出 smoke 验证。
- 新增统一鉴权文件 `~/.config/cagent/auth.json`：`type: "api_key"|"oauth"`；ChatGPT OAuth 支持浏览器 PKCE、设备码、0600 原子 token 存储和自动 refresh。
- 当前 provider 与 `models[]` 显式引用的 provider 启动时实时请求 `/models`，兼容标准 `data[]` 和 Codex `models[]`，不使用模型缓存。

## 测试与质量

- **27/27 ctest**：Debug 与 ASan/UBSan 双构建全绿；覆盖流式 AgentEvent、历史回放、审批预览、list/find、项目指令、EOF 守卫、重试、长工具链与上下文压缩
- 本次变更涉及的生产源文件 clang-tidy 零告警
- valgrind 历史抽查全净；pty 端到端验证（TUI 渲染/取消/设置命令/key 不泄漏）
- 微基准（`-DCAGENT_BUILD_BENCH=ON` → `cagent_bench`）：序列化 1.8GB/s、SSE 53ns/chunk

## 关键坑与修复记录（防止重蹈覆辙）

1. **yyjson_mut_obj_add_str 借用字符串不拷贝**（0.12 文档明示）——动态字符串 free 后 doc 内引用悬空（valgrind 抓出）。`util/json.c` 的 `json_builder_obj_add_str`/`arr_add_str` 已统一用 `yyjson_mut_strcpy` 拷贝。
2. curl multi 集成：socket 回调内**禁止再调 http_pump**（不可重入）；timer 到期由 epoll 超时覆盖（timer_cb 不 wakeup，否则事件洪泛饿死 socket 数据）。
3. **WRITEFUNCTION 无 return = UB**（返回值随机致偶发 abort）——清理代码时务必保留 `return n`。
4. SocketWatcher 是 **socket 级**资源（keep-alive 共享），挂 HttpRuntime 而非请求；finish_request 不碰 watcher。
5. `posix_spawnattr_setpgroup(0)` 不新建进程组（继承父组）——进程组 kill 需 fork+setpgid。
6. `message_list_append` 接管所有权（free 壳）；`move_range` 用 `append_copy`（避免 free 数组内部地址）。
7. Anthropic 端点：base_url 已含 `/v1` 时拼 `/messages`（否则 `/v1/messages`），双 v1 会 404。
8. `--model <label>` 需在 runtime_new 中解析为 API 名（label 直发会 401）。
9. Nix 环境：nixpkgs 的 yyjson.pc 有 bug（用 CMake config）；clang-tidy 需 `C_INCLUDE_PATH` 注入 glibc 头；CMake imported target 的 include 被 NIX_CFLAGS_COMPILE 隐式去重（用 `-isystem<dir>` 编译选项注入）。
10. Nix 2.34 indented string 中 `\${` 转义失效（shellHook 避免 `${var:+...}` 语法）。
11. 异步工具结果追加会移动 `MessageList`；用 `tool_message_index + tool_index` 重取，禁止跨 append 持有元素地址。
12. Model 实例会被并发 Agent 共享；Provider 的 active Transfer 必须是集合并按 request_id 取消，不能只存一个“当前请求”。
13. `ProcessTask` watcher 由任务拥有；timer/pipe 回调不销毁任务，超时先 SIGTERM、2 秒 grace 后 SIGKILL，最终统一 waitpid。
14. subagent 工具内部嵌套 `runtime_pump` 会重入 parent；异步 ToolTask 只能 poll 子 Agent，由外层 pump 驱动 I/O。

## 已知限制（后续方向）

- `read/write/edit/list/find` 仍执行有界同步文件 I/O；当前没有 Worker Pool/io_uring。`bash/grep/subagent` 已异步，不再长期阻塞 EventLoop
- 同一 assistant 消息中全部标记为 `PARALLEL_SAFE` 的 tool call 可并行；副作用/混合批次保持串行，尚无写冲突自动合并
- LLM Context Compaction 已实现；摘要输入有 256KiB 上限，失败时退回确定性摘要，append-only session 不物理删除旧 JSONL 行
- Responses API 当前只覆盖文本与 function tool；Responses 专有 reasoning/tool（web_search、computer_use 等）和非文本 input 尚未映射
- ChatGPT OAuth 依赖官方 Codex 后端协议；OAuth client_id、端点、`/models` 响应字段或请求头未来变化时需要同步更新；模型发现失败时仅能回退手工配置
- 单模型表在内存中；TUI `/key`/`/base-url` 设置不持久化（写回 config 未实现）
- 尚无容器/seccomp/chroot 强沙箱；bash 获批后仍拥有当前用户权限，审批预览仅用于人工判断
- HTTP 预解析是同步 `getaddrinfo()`，极慢/失效的系统 NSS 仍可能阻塞单线程 EventLoop；当前失败时回退 libcurl 默认解析路径

## 常用命令

```bash
nix develop -c cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Debug
nix develop -c cmake --build build
nix develop -c ctest --test-dir build
cmake -B build-asan -G Ninja -DCMAKE_BUILD_TYPE=Debug -DCAGENT_SANITIZERS=ON
cagent --login  # ChatGPT Plus/Codex OAuth
./build/cagent -p "hi"  # 透明代理场景无需设置 http(s)_proxy
```

## 恢复建议（新会话）

1. `git log --oneline | head -8` 看最近提交
2. 读本文件 + `docs/DESIGN.md`（架构基线，含所有决策记录）
3. `ctest --test-dir build` 确认基线
4. 后续方向优先级：Responses 专有能力/真实 OpenCode Go 联调 > 设置持久化 > 可选的文件工具 Worker Pool/并行 tool-call
