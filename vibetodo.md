# cagent Vibe Coding TODO

> 评估基线：`main@062e9cc`，2026-08-17。目标是回答“现在能否用于 vibe coding，以及离低监督自主开发还差什么”。
> 本文以当前源码和实测结果为准；`TODO.md` 基于旧提交 `dd8435a` 的“≈38%”评估已经过时。

## 结论

- **受监督的日常 vibe coding：约 86%，现在已经可用。** 读项目规则、检索、精确修改、构建/结构化测试、逐工具审批、diff 预览、git checkpoint、plan、memory、session resume、并行只读工具和 SubAgent 均已形成闭环。该数字与当前内置六维加权评估一致（`src/report/report.c:11-14`，计算结果四舍五入为 86%）。
- **低监督/无人值守自主开发：保守估计约 60%，暂不应开启。** 最大缺口不是“会不会改代码”，而是 workspace/进程隔离、密钥保护、可审计且绑定范围的授权、抗提示注入以及可靠回滚。
- **剩余工作量：17 个工作流。** P0 5 项决定能否安全自治；P1 7 项决定日常体验是否顺畅；P2 5 项决定能否稳定发布和长期维护。
- **当前建议：** 可以在受信任、可回滚的 Git 工作区中，以默认逐次审批模式使用；不要在含生产密钥的环境中开启长期 `/trust on`，不要把现有审批预览当作强沙箱。

## 已验证基线

| 能力 | 当前状态 | 证据 |
|---|---|---|
| 读—改—建—测闭环 | 已实现 | `README.md:104-110`；`src/tool/{read,write,edit,test}.c` |
| 副作用审批与预览 | 已实现基础版 | `src/tool/tool.h:61-87`；`README.md:108` |
| git 状态、diff、提交、revert、checkpoint | 已实现基础版 | `README.md:106`；`src/tool/git.c` |
| 持久化 plan 与验收状态 | 已实现基础版 | `src/tool/plan.c`；`.cagent/plan.json` 在 resume 时注入 |
| 结构化项目记忆与会话恢复 | 已实现基础版 | `src/tool/memory.c`、`src/session/session.c`、`src/agent/loop.c` |
| 结构化 CTest 结果 | 已实现 | `src/tool/test.c` 使用 JUnit；失败时返回 checkpoint 恢复提示 |
| 并行执行 | 部分实现 | 只读 `PARALLEL_SAFE` tool-call 可并行；副作用/混合批次仍串行（`PROGRESS.md:141`） |
| 自诊断与基准 | 已实现基础版 | `src/tool/{diagnose,bench}.c` |
| 当前 Debug 测试 | **27/27 通过** | 重新配置、构建并运行 `ctest --test-dir build --output-on-failure`，6.73s |
| 当前 ASan/UBSan 测试 | **27/27 通过** | 重新配置、构建并运行 `ctest --test-dir build-asan --output-on-failure`，7.31s，无 sanitizer 报错 |
| CI/发布验证 | 未实现 | 仓库没有 `.github/workflows/`；Nix 仅有 build check（`flake.nix:95-101`） |

## P0：安全自治前置条件（5 项）

### [ ] VIBE-P0-01 统一 workspace 文件边界

**现状：** `list/find` 会 `realpath()` 并拒绝离开 workspace，但 `read/write/edit` 的 `resolve_path()` 接受绝对路径和 `..`；获批后可直接读写工作区外文件。`README.md:153` 也明确当前没有强隔离。

**要做：**
- 为所有文件工具建立同一套 canonical path policy；默认只允许 workspace 内路径。
- 使用目录 fd + `openat2(RESOLVE_BENEATH|RESOLVE_NO_MAGICLINKS)`（不支持时提供可靠 fallback），避免符号链接和检查后替换竞态。
- 对显式的 workspace 外读写单独申请窄范围授权，不复用全局 trusted 状态。
- 覆盖绝对路径、`../`、symlink、bind mount、路径在 preview 与 execute 间变化的测试。

**验收：** 默认模式下任何文件工具都无法越过 workspace；路径或 inode 在批准后漂移时 fail-closed；越界测试在 Debug 和 ASan 中全绿。

### [ ] VIBE-P0-02 隔离 shell、网络和敏感环境变量

**现状：** `bash` 以当前用户权限运行 `/bin/sh -c`，没有容器/seccomp/chroot；子进程继承环境，legacy `*_API_KEY` 仍受支持（`src/main.c:62-67`）。审批只能降低误操作，不能阻止数据外传。

**要做：**
- 子进程默认使用环境变量 allowlist，移除 API key、OAuth token、代理凭据和其他常见 secret。
- 将模型鉴权留在宿主 HTTP 层，不向工具进程暴露；提供显式、可审计的环境变量授权。
- 提供可选但默认推荐的隔离后端（例如 user namespace + mount namespace + seccomp，或 bubblewrap/container profile）。
- 将网络、工作区外路径、设备、进程树和资源上限作为独立能力控制。

**验收：** 恶意命令无法读取宿主密钥或默认访问 workspace 外文件；网络默认策略可配置且可审计；隔离不可用时明确降级并禁止无人值守模式。

### [ ] VIBE-P0-03 把“批准一次”升级为可验证的 Execution Grant

**现状：** 已有逐工具 `/approve`/`/reject` 和进程内 `/trust on`，但批准主要绑定当前 tool-call；preview 是建议性、可截断的信息（`README.md:108`），没有持久化审批审计。`src/report/report.c:27-30` 也记录了能力/路径/步骤范围缺口。

**要做：**
- 分离不可变计划、审批记录、执行状态和审计事件。
- Grant 至少绑定：计划/hash、步骤、工具能力、规范化路径、命令摘要、有效期和调用次数。
- preview 后参数、文件身份、Git HEAD 或依赖范围变化时自动标记 stale 并重新审批。
- 将 `/trust on` 改为可选 scope（只读、测试、指定路径、指定步骤），保留醒目的状态和一键撤销。
- 持久化 approve/reject/stale/cancel/execute/result 事件，但不得写入 secret。

**验收：** 旧审批不能执行新参数或新路径；resume 后不会隐式恢复 trusted；审计日志能还原“谁批准了什么、实际执行了什么、结果如何”。

### [ ] VIBE-P0-04 防止项目指令和长期 memory 污染

**现状：** Git 根到 cwd 的 `AGENTS/CLAUDE/PROGRESS` 会注入模型，memory 每轮有界注入；这提升代码理解，也形成项目内容到高信任上下文的提示注入通道（`README.md:112`、`src/report/report.c:32-35`）。

**要做：**
- 标注 system policy、用户指令、项目规则、工具输出和长期 memory 的来源与信任级别。
- 项目文件不得提升权限、开启 trusted、扩大路径范围或自动写入长期 memory。
- memory 写入要求类型、来源和可选审批；支持查看、删除、隔离可疑记录。
- 增加恶意 README/AGENTS、工具输出注入、跨 session memory poisoning 回归测试。

**验收：** 仓库内文本不能改变宿主安全策略；可疑内容最多影响任务建议，不能获得能力；长期 memory 全部可追溯和清理。

### [ ] VIBE-P0-05 建立失败可逆且不扩大损失的恢复协议

**现状：** clean-worktree checkpoint 和 hard reset 已实现，但 restore 只恢复 tracked 文件并保留 untracked；测试失败只返回恢复指令。完整 staged patch 浏览、结构化 staging 和失败后的交互式恢复仍缺失（`src/report/report.c:22-25`）。

**要做：**
- 在修改前记录 HEAD、工作区状态、授权路径和必要的未跟踪文件清单；脏工作区必须拒绝或显式选择保护策略。
- 将“建议回滚”“执行回滚”“保留失败现场”分开，回滚仍需窄范围审批。
- 回滚前显示将丢失的 tracked/untracked/staged 内容；禁止静默 `reset --hard`。
- 对进程被杀、磁盘满、部分写入、测试超时和 session 恢复后继续回滚做故障测试。

**验收：** 任何自动恢复都不会覆盖用户原有改动；中断后可判定当前处于修改前、修改后或待恢复状态；恢复行为有审计记录。

## P1：顺畅的日常 Vibe Coding（7 项）

### [ ] VIBE-P1-01 计划批准、失败重规划和任务预算

- 在现有 `.cagent/plan.json` 上增加计划版本/hash、依赖范围、步骤授权和最终验收汇总。
- 失败步骤提供“重试 / 修改步骤 / 缩小范围 / 放弃并恢复”的交互式重规划。
- 增加每任务 token、费用、工具轮次、墙钟时间预算；接近上限时预警，超限时挂起。
- **验收：** resume 后能精确继续未完成步骤；旧计划批准不能执行新计划；预算超限不会继续烧 token 或执行副作用。

### [ ] VIBE-P1-02 完整 diff、结构化 staging 与提交体验

- 审批界面支持完整 patch 分页、单文件/单 hunk 批准、二进制和超大文件摘要。
- 新增结构化 `git_add`/unstage，`git_commit` 只提交明确批准的 path/hunk，避免依赖外部预先 staged 状态。
- 提交前展示测试证据、计划完成状态和最终 staged diff；提交后输出 commit id。
- **验收：** 无法提交未批准文件；preview 与实际 staged patch hash 不一致时拒绝；部分 staging 有端到端测试。

### [ ] VIBE-P1-03 Session 浏览、分支与持久化故障可见性

- 增加 session 列表、按项目/时间筛选、标题预览和 TUI 恢复选择器。
- 支持 fork/clone session，明确 cwd、plan、memory、trusted 和审批记录如何继承。
- 将 compaction、JSONL append、磁盘同步失败显示给用户，不再只写日志。
- **验收：** 用户无需复制 session id 即可恢复；磁盘满或损坏 session 不会伪装成成功；fork 不继承 trusted grant。

### [ ] VIBE-P1-04 并行写冲突检测与原子 batch edit

- 为多个文件变更提供 batch edit：先验证全部 old text/path/version，再一次性提交或全部不写。
- 并行副作用按规范化路径和 Git 基线检测冲突；同文件默认串行，不同文件仅在独立 grant 下并行。
- **验收：** 任一子编辑失败时文件集保持原状；symlink/rename 后不会绕过冲突检测；结果顺序稳定。

### [ ] VIBE-P1-05 消除事件循环上的同步阻塞点

- 将 `read/write/edit/list/find` 的大文件/大目录 I/O 移入受限 worker pool 或改为可取消的分片执行（`README.md:152`）。
- 将同步 `getaddrinfo()` 移出主事件循环，并保留透明代理兼容（`PROGRESS.md:147`）。
- 为队列长度、单任务 CPU/I/O 配额和 backpressure 加指标。
- **验收：** 大目录扫描、慢 NFS/NSS 时，TUI、取消和其他 Agent 仍有响应；压力测试无 fd/task 泄漏。

### [ ] VIBE-P1-06 完成设置持久化和首次使用引导

- 原子持久化 TUI `/key`、`/base-url` 与 provider 选择；密钥只写 0600 `auth.json`，运行配置写 `config.json`。
- 首次启动提供 provider、登录/API key、模型、workspace 与安全模式向导，并执行可解释的连接诊断。
- 清楚区分临时 session 设置和永久设置，提供撤销/重置。
- **验收：** 重启后配置一致且不泄密；写入失败明确报错；无配置用户可在 TUI 内完成首轮请求。现有限制见 `PROGRESS.md:145`。

### [ ] VIBE-P1-07 消除 TODO/报告的多份事实源漂移

- 用一份机器可读评估数据生成 `/report`、`TODO.md`/本文件中的完成度表，或明确只保留一个权威来源。
- 将能力项绑定测试名/需求 ID；合并实现后由测试或脚本更新状态，不再手工同步 C 数组与多份 Markdown。
- 清理 `TODO.md` 中已完成但仍标 `[ ]` 的审批、git、plan、memory、test、并行 tool-call、diagnose/bench 项。
- **验收：** CI 能检测报告和实现/测试映射漂移；同一能力不会同时显示 0%、75% 和已完成。

## P2：发布级质量与能力扩展（5 项）

### [ ] VIBE-P2-01 建立 CI、静态分析和跨架构矩阵

- 增加 CI：Debug、Release、ASan/UBSan、clang-tidy、format、Nix build/check。
- 至少覆盖 x86_64-linux 与 aarch64-linux；固定依赖/编译器组合并缓存构建产物。
- **验收：** PR 必须通过 27+ CTest、sanitizer 和静态分析；失败日志保留可定位证据。目前仓库没有 CI workflow。

### [ ] VIBE-P2-02 增加真实终端、真实 Provider 与故障注入测试

- 增加真实 PTY 的审批/拒绝、完整 diff、resize、Ctrl+C、OAuth/device-code UI 回归。
- 提供 opt-in 真实 OpenAI/Anthropic/ChatGPT/OpenCode Go 联调，严格隔离凭据和费用。
- 覆盖磁盘满、只读目录、session 截断、provider 429/5xx/半包/坏 SSE、进程 SIGKILL。
- **验收：** nightly/手动集成套件可重复运行；不会向日志或 artifact 写 secret；故障后 session 可恢复。

### [ ] VIBE-P2-03 补齐 Provider 能力与降级说明

- 为 Responses API 的 reasoning、web/file/computer 等专有能力定义映射或显式“不支持”响应（`README.md:154`）。
- 建立 provider capability negotiation，避免模型选择了宿主不支持的 tool/input 类型后静默失败。
- **验收：** 每个已声明 capability 都有契约测试；不支持能力在请求前被拒绝并给出替代路径。

### [ ] VIBE-P2-04 完善可观测性、回归基线和资源预算

- 在 TUI 显示 compaction、持久化、scheduler/backpressure、token/费用/耗时；retry 已有状态提示，不重复实现。
- 保存 `bench` 历史基线并在 CI 比较明显回退；增加长会话和 100+ SubAgent soak。
- **验收：** 能定位“慢在模型、工具、I/O 还是排队”；性能/内存/fd 回归超过阈值时 CI 报警。

### [ ] VIBE-P2-05 完成版本、许可、安装与发布链路

- 增加 `LICENSE`（`flake.nix:89` 声明 MIT，但仓库当前无许可证文件）、CHANGELOG、`--version` 和统一版本来源。
- 产出可验证的 Release 二进制/包、checksum/SBOM；记录最低 glibc/内核和依赖要求。
- 验证 `cmake --install`、Nix package 及全新 Linux 环境的安装/卸载；明确项目是 Linux/POSIX-only（`CMakeLists.txt:119-122`）。
- **验收：** 从 release artifact 到首次 TUI 请求有自动 smoke test；版本、源码 tag、包元数据一致。

## 推荐顺序与完成门槛

1. **安全自治门槛：** P0-01 → P0-02 → P0-03 → P0-04 → P0-05。完成前只推荐受监督使用。
2. **日常体验门槛：** P1-01、P1-02、P1-03、P1-06 优先；随后处理并发、阻塞 I/O 和事实源。
3. **发布门槛：** 先 P2-01/02，再扩 Provider、可观测性和 release 工程。

可以认为“vibe coding 1.0 完成”的可核验条件：

- [ ] P0 全部完成，并通过越界、secret、提示注入、审批漂移和恢复故障测试。
- [ ] P1 至少完成计划/预算、diff/staging、session UX、设置持久化，连续完成 10 个真实中型任务且无人工修复状态文件。
- [ ] CI 在 Debug + ASan/UBSan + clang-tidy + 两种 Linux 架构持续通过。
- [ ] 真实 provider 与 PTY smoke 稳定，所有凭据和审批事件满足不泄密、可追溯要求。
- [ ] 发布物可在全新环境安装，并完成“启动 → 配置 → 修改 → 测试 → 审批 → 提交 → resume”端到端场景。
