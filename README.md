# cagent

轻量级、纯 C（C17）、事件驱动的 AI Coding Agent Runtime。参考 Pi Coding Agent 的架构思想，以原生 Linux/POSIX 能力重新实现：epoll + libcurl multi + 轻量 Agent 状态机，支撑大量并发 SubAgent（100 个 Agent 仅 1 线程、~10MB RSS）。无 Node/TS/Python 运行时依赖，无插件系统——扩展 = 新增 `.c/.h` 模块 + 一行注册后重新编译。

## 构建

```bash
nix develop                    # 完整开发环境（clang/cmake/ninja/curl/yyjson/...）
cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Debug
cmake --build build
ctest --test-dir build         # 27 个测试（Debug + ASan 双构建）
./build/cagent                 # 默认进入 TUI
```

可选：`-DCAGENT_SANITIZERS=ON`（ASan/UBSan）、`-DCAGENT_BUILD_BENCH=ON`（微基准）。

## 配置

默认 provider 为 **OpenCode Go 订阅**（默认使用 OpenAI 兼容端点 `https://opencode.ai/zen/go/v1`；模型表可切换 Anthropic 或 Responses）。

```bash
# API key provider 使用 ~/.config/cagent/auth.json 配置密钥来源
```

配置文件 `~/.config/cagent/config.json`（参考 `config.example.json`）只保存当前选择的模型和思考等级：`model` 使用 `provider/model` 形式，`thinking_level` 可选。模型目录缓存统一保存到 `~/.config/cagent/models.json`，格式为 `{ "provider": ["model", ...] }`；启动时会优先加载缓存并在请求成功后原子更新。鉴权统一保存于 `~/.config/cagent/auth.json`（参考 `auth.example.json`）：

```json
{
  "openai": {
    "type": "api_key",
    "key": "YOUR_OPENAI_API_KEY"
  }
}
```

或 OAuth（OAuth 登录所需的令牌字段由登录流程维护）：

```json
{
  "openai-codex": {
    "type": "oauth",
    "access": "...",
    "refresh": "...",
    "expires": 0,
    "accountId": "..."
  }
}
```

`cagent --login` 会自动创建/更新 OAuth 类型的 `auth.json`；API key 类型可参考 `auth.example.json` 手工创建。配置优先级：CLI > 环境变量 > 配置文件 > 默认值。`auth.json` 等同密码，API key/refresh token 永不写入日志、TUI 或 Session，文件权限为 0600。模型目录不可用时保留 `models.json` 中的列表并记录告警；显式模型缺失时会告警后回退。

连续 coding loop 的 `max_retries` 默认 5，仅在尚未产生任何文本/tool-call delta 时对 429、可重试 5xx 和传输错误做 250ms 起、最高 8s 的非阻塞指数退避；HTTP 连接还启用了低速检测与 TCP keepalive，以便把断线/卡死交给同一重试策略恢复。重试进度会显示在状态栏，可在 `config.json` 覆盖。工具调用不再设置固定轮数上限，长任务由模型上下文窗口与自动 compaction 驱动。

### ChatGPT Plus / Codex OAuth

ChatGPT Plus 通过官方 Codex 后端使用，不消耗 OpenAI API key 余额：

```bash
cagent --login              # 浏览器 OAuth + PKCE
cagent --device-code        # 无浏览器终端的设备码登录
cagent --logout
```

在 `config.json` 中选择 ChatGPT 模型（端点和 Responses 协议自动内置）：

```json
{
  "model": "chatgpt/gpt-5"
}
```

启动时会请求当前 provider、`models.json` 中出现的 provider，以及 auth.json 中已完成 OAuth 登录的 ChatGPT `/models`；ChatGPT 会自动出现在 TUI 的 `/model` 选择器中。模型目录会写入 `models.json`。选择器支持上下键移动、直接输入文字进行模糊过滤、回车确认；确认后的默认模型会以 `provider/model` 形式原子保存到当前配置文件（默认 `config.json`，也支持 `-c settings.json`）。

知名 provider 已内置端点：`opencode-go`、`openai`、`anthropic`、`chatgpt`、`openai-codex`（ChatGPT Codex 别名）。自定义 provider 只在 `~/.config/cagent/custom_provider.json` 中保存 `baseUrl` 和 `type`，API key 不写在这里，而是在 `~/.config/cagent/auth.json` 中用同名 key 匹配：

```json
{
  "my-gateway": {
    "baseUrl": "https://llm.example.com/v1",
    "type": "openai"
  }
}
```

对应的 `auth.json`：

```json
{
  "my-gateway": { "type": "api_key", "key": "YOUR_API_KEY" }
}
```

旧版 `{ "providers": ... }` 和带 `models` 的自定义配置仍可读取；新模型列表统一写入 `models.json`。启动时会请求当前 provider 以及缓存中出现的 provider；请求失败时继续使用缓存。ChatGPT Plus 的模型和额度由 Codex 后端控制，不等价于 API 平台额度。

## 使用

```bash
cagent                     # TUI 交互（Enter 提交，Ctrl+C 取消当前任务，空闲时退出）
cagent -p "fix the build"  # 一次性提示
cagent -r <session-id>     # 恢复会话
cagent -C /path/to/project # 指定工作目录
cagent --model fast        # 按名字选择启动模型；TUI 用 /model 切换 provider/model
cagent -l                  # 纯 REPL（无 TUI）
cagent --login             # ChatGPT Plus/Codex OAuth 登录；TUI 内可用 /login
```

日常 vibe coding 推荐默认 TUI：在项目根目录启动 `cagent -C .`，直接描述目标，审查每次变更预览后批准；Ctrl+C 取消当前回合但保留已持久化历史，随后可用 session id 恢复。

内置工具：`read`/`list`/`find`/`grep`/`write`/`edit`/`bash`/`subagent`/`memory`/`plan`/`test`/`diagnose`/`bench`/`git_checkpoint`/`git_restore_checkpoint`/`git_status`/`git_diff`/`git_commit`/`git_revert`。`list/find` 在 workspace 内提供有界、确定排序的目录探索，无需先批准 bash；`plan` 将步骤、验收条件、状态和失败次数持久化到项目 `.cagent/plan.json`，resume 时自动注入摘要；`memory` 可持久化决策、约束和错误教训，每轮请求有界注入；`test` 失败时会根据 checkpoint 状态给出恢复指令；`diagnose` 返回 agent、scheduler、context、session 和 tool 指标。只读 parallel-safe tool-call 可并行启动并按原顺序回填结果；subagent 支持并行任务数组与每任务模型选择。

`bash`、`write`、`edit`、`test` 以及 git 写操作默认逐次审批：TUI 输入 `/approve` 或 `/reject`；交互式 `-p`/`--plain` 使用 `y/N`；非 TTY/EOF 始终 fail-closed。需要连续执行时可显式输入 `/trust on`（plain 审批提示也接受 `always`），在当前进程内自动批准顶层 Agent 后续副作用工具；TUI 顶栏会持续显示 `[TRUSTED]`，输入 `/trust off` 恢复逐次审批。trusted 状态不写入配置或 session，重启/resume 后默认关闭，也不会授权子代理或绕过“无审批宿主即拒绝”的边界。**项目没有 shell 沙箱，trusted 模式允许模型执行可访问 workspace 外路径的命令，只应在可回滚且受信任的工作区短时开启。**三个交互模式均可用 Ctrl+C 取消运行中的回合，取消后同一 session 可继续提交任务。`write/edit` 显示受限行级预览，bash 显示命令、cwd、超时和启发式风险提示，git 写操作显示 staged/目标摘要，test 显示将执行的构建目录。预览是建议性信息，执行时会重新校验。

工具调用没有固定轮数上限；每次模型请求前都会根据所选模型的 `context_window` 估算上下文压力，接近窗口时在同一 Session 内生成并持久化 compaction 摘要，随后以“摘要 + 最近消息”开启下一次模型请求。Session ID、plan、memory 和恢复链保持连续。失控任务需使用 Ctrl+C 主动取消；trusted 模式仍只应在受信任且可回滚的工作区短时启用。

模型文本按 SSE delta 实时显示；`--resume` 启动后会先回显用户、助手和工具历史。TUI 顶栏显示 session id，`/session` 可输出完整恢复命令。项目规则按 Git 根到当前目录加载，每层优先 `AGENTS.override.md`，其次 `AGENTS.md`/`AGENTS.MD`、`CLAUDE.md`/`CLAUDE.MD`；根目录 `PROGRESS.md` 作为项目记忆，但默认只注入最多 4 KiB 的头尾摘要（完整内容仍可用 `read` 工具按需读取），总输入有 128 KiB 上限，子代理继承同一规则。底部上下文比例估算同时包含 system prompt、消息历史和启用工具 schema，并用于接近窗口时的自动 compaction。

**多协议模型**：OpenCode Go 目录中部分模型走 Anthropic Messages 协议（MiniMax/Qwen 等），部分模型走 Responses API（GPT/Grok 等）。在 `models[]` 中标注 `"protocol": "anthropic"` 或 `"protocol": "responses"`，provider 会自动选择对应端点与工具调用格式。

```json
{"tasks": [
  {"task": "explore the network module", "role": "explore", "model": "fast"},
  {"task": "review the diff", "role": "review"}
]}
```

## 架构

```
Agent(轻量状态机) ── ModelRequest ──> Provider(OpenAI/Anthropic/Responses)
    │                                        │
    ├─ ToolRegistry(read/write/edit/bash/...) │ libcurl multi (共享)
    ├─ Session(JSONL)                         │ epoll Event Loop
    └─ CancelToken(父子传播)                  │ Scheduler(并发上限)
                                              └─ Worker 挂起，不占线程
```

模块：`agent/`(状态机/消息/上下文压缩)、`model/`(SSE/Provider)、`tool/`、`session/`、`runtime/`(event_loop/http/scheduler/process)、`tui/`(ncursesw backend + 纯布局模型，事件驱动)、`util/`(String/Arena/JSON 封装)。详见 `docs/DESIGN.md`。

## 测试

- 27 个 CTest：util/SSE/Agent Loop(mock model)/工具/进程/并发/SubAgent(100 Agent)/TUI 渲染/多模型/Responses Provider/OAuth 存储
- Debug + ASan/UBSan 双构建无泄漏；OAuth 测试不访问真实认证服务；clang-tidy 零告警
- 微基准（`cagent_bench`）：消息序列化 ~1.8 GB/s，SSE 解析 ~53ns/chunk

## 网络

本机有透明代理（fake-ip/TUN）时，cagent 在未设置显式代理的情况下会先通过 libc/NSS `getaddrinfo()` 解析，再用 `CURLOPT_RESOLVE` 将解析地址交给 libcurl，避免 multi + resolver completion 卡在连接超时。显式设置 `http_proxy`/`https_proxy` 时仍由 libcurl 正常使用代理；透明代理场景无需额外网络配置。

```bash
cagent
```

## 已知限制

- `read/write/edit/list/find` 仍执行有界同步文件 I/O；bash/grep/subagent 已异步
- 本轮未引入容器/seccomp/chroot：bash 获批后仍可访问 workspace 外路径；审批和预览不能替代强沙箱
- Responses API 当前只覆盖文本与 function tool，Responses 专有 reasoning/web/file/computer tools 尚未映射
- API 模型 ID 不带 `opencode-go/` 前缀（那是 opencode 内部格式；API 目录 `/zen/go/v1/models` 返回裸 ID）
