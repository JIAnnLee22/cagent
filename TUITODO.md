# TUITODO — TUI 自举能力报告页实施计划

> 目标:为 cagent 新增 `/report` 全屏仪表盘,展示自举开发能力评估报告(6 维度完成度条形图 + 分级缺口清单 + 推进顺序)。
> 方案(用户已确认):**A+A = 全屏仪表盘 + 编译期内嵌结构化数据**。
> 数据来源:TODO.md 评估报告(2026-08-16),结构化改写为 C 数据,符合项目"扩展=新增 .c/.h + 重编译"哲学。
> 状态:计划已创建,待用户确认后实施。

## 1. 文件改动清单

| 文件 | 操作 | 内容 |
|---|---|---|
| `src/report/report.h` | 新增 | 报告结构化数据定义与查询接口(纯数据,无渲染) |
| `src/report/report.c` | 新增 | 6 维度/缺口/已完成/推进顺序的静态数据 |
| `src/tui/render.h` | 修改 | `TuiModel` 增加 `bool report_mode; size_t report_scroll;` |
| `src/tui/render.c` | 修改 | `tui_render_screen` 增加 report_mode 分支,绘制仪表盘 |
| `src/tui/tui.c` | 修改 | 报告模式接口 + 键绑定(Esc/Up/Down/PgUp/PgDn/Enter/Ctrl+C) |
| `src/main.c` | 修改 | `/report` 命令、退出回调注册、`/help` 列表、submit 拦截 |
| `CMakeLists.txt` | 修改 | `src/report/report.c` 加入 `cagent_core` |
| `tests/test_tui.c` | 修改 | 报告渲染/滚动/键交互测试 |

## 2. 各文件设计

### 2.1 `src/report/report.h` — 数据模型(纯数据模块,禁止依赖 TUI/terminal)

```c
typedef struct {
    const char* label;   /* 维度名 */
    int         weight;  /* 权重 0-100 */
    int         current; /* 当前完成度 0-100 */
} ReportDimension;

typedef struct {
    int  priority;       /* 0=P0, 1=P1, 2=P2, 3=P3 */
    const char* title;   /* 缺口标题 */
    int  completeness;   /* 当前完成度 0-100 */
    const char* const* items; /* 任务项数组 */
    size_t items_len;
} ReportGap;

int    report_overall_percent(void);       /* 加权总完成度(当前 ≈38) */
size_t report_dimension_count(void);
const ReportDimension* report_dimensions(void);
size_t report_gap_count(void);
const ReportGap* report_gaps(void);
size_t report_done_count(void);
const char* const* report_dones(void);     /* 已具备清单 */
size_t report_roadmap_count(void);
const char* const* report_roadmap(void);   /* 推进顺序 */
const char* report_title(void);            /* "cagent 自举开发能力评估" */
const char* report_conclusion(void);       /* 一句话结论 */
```

### 2.2 数据内容(来源:TODO.md 评估)

- 6 维度:`读-改-建-测闭环`(权重30,当前70)、`安全/审批/回滚`(25,10)、`规划与执行跟踪`(15,10)、`记忆与状态恢复`(15,30)、`工具完备性`(10,40)、`自诊断`(5,0);`report_overall_percent()` 用加权计算而非硬编码 38,数据与公式同步。
- 缺口 7 项,按 P0-P3:审批与安全边界(P0,10%)、git 集成与回滚(P0,0%)、任务规划与执行跟踪(P1,0%)、跨会话项目记忆(P1,20%)、测试结果结构化理解(P2,40%)、并行 tool-call(P2,40%)、自我诊断与 profile(P3,0%);每项带 TODO.md 中列出的任务子项。
- 已具备 8 项(read/grep、write/edit、bash+cmake/ctest、状态机 loop、compaction、session JSONL、subagent、部分自愈)。
- 推进顺序 5 步:P0 审批层→P0 git 三件套→P1 todo/plan→P1 项目记忆→P2/P3。

### 2.3 `render.c` — 仪表盘布局(全屏,复用现有纯文本渲染,无颜色)

```
[header 行,复用 app_update_header]
  自举开发能力评估报告            <- 标题行
  总完成度: ≈38%(加权)            <- 结论行
  已具备:8 项 | 缺口:7 项(P0×2 P1×2 P2×2 P3×1)
  ── 维度完成度 ─────────────────
  读-改-建-测闭环   70% ████████░░░ [30%]
  安全/审批/回滚     10% █░░░░░░░░░ [25%]
  ...(6 行条形图,bar 宽度 = (cols-24) 格,█=U+2588 ░=U+2591)
  ── 缺口清单(按优先级) ─────────
  [P0] 审批与安全边界 (完成度 10%)
    · tool.flags 危险分级未启用
    · 审批流:挂起→diff 预览→批准/拒绝
    ...
  ── 推进顺序 ────────────────────
  1. P0 审批层(38% → ~55%) ...
  [滚动提示行,复用现有 "[scrolling] " 模式]
[status 行:"/report: ↑↓ 滚动, PgUp/PgDn 翻页, Esc 返回"]
[input 行,沿用]
```

实现要点:
- `render_report_screen(m, out, cursor_row, cursor_col)`:先组合全部逻辑行(每行一个定长字符串),按 `report_scroll` 分页取 `middle_rows` 行;总行数 > 可视区时显示滚动提示。
- 条形图字符用字节串 `"\xe2\x96\x88"`(█)/`"\xe2\x96\x91"`(░);复用现有 `utf8_fit_width` 处理列宽。
- 报告行不写入 `m->lines`,退出报告模式后原对话内容不受影响。

### 2.4 `tui.c` — 模式接口与键绑定

新增接口(与 choice mode 对称,复用其回调模式):
```c
void  tui_report_start(Tui* t);
void  tui_report_stop(Tui* t);
bool  tui_report_active(const Tui* t);
void  tui_report_scroll(Tui* t, int delta);      /* 正=向上翻旧内容 */
void  tui_set_report_cancel_callback(Tui* t, TuiCancelCb cb);
```
键绑定(`tui_feed_bytes` 中 `report_mode` 优先于普通输入):
- `Esc`(0x1b) → report_cancel 回调(退出报告)
- `Ctrl+C`/`Ctrl+D` → 同上
- `Up`/`Down`(esc [A/B)→ `tui_report_scroll(±1)`
- `PgUp`/`PgDn`(esc [5~/6~)→ `tui_report_scroll(±rows/2)`
- `Enter`/`\n` → 退出报告模式,不触发 submit(空输入不发消息)
- 其余字符:忽略(不进入 input 缓冲),避免误输入;报告模式下隐藏输入行内容或仅保留 "> "

### 2.5 `main.c` — 命令集成

- `app_command`:`/report` → `tui_report_start(app->tui)`,`/help` 命令列表追加 `/report`。
- `app_submit`:开头判断 `tui_report_active` → 直接 `tui_report_stop` 并 return(防止 Enter 把报告模式输入当作消息提交;报告模式已由 tui.c 拦截,此为双保险)。
- `tui_run`:`tui_set_report_cancel_callback(tui, app_report_cancel)`,`app_report_cancel` = 停止报告模式 + 恢复 header/status。
- 进入报告时 `tui_set_status` 提示键位;退出时恢复 `app_update_header` 与就绪状态。

### 2.6 `tests/test_tui.c` — 新增测试(纯逻辑,非 TTY,不初始化 ncurses)

- `test_report_render`:设 rows/cols,`m.report_mode=true`,渲染后 CHECK 含标题、`总完成度`、6 个维度 label、`%`、块字符、缺口 P0 标题。
- `test_report_scroll`:内容超一屏,滚动后首屏/次屏内容切换正确,滚动提示出现。
- `test_report_keys`:pipe + `tui_new`;`tui_report_start` 后 `tui_feed_bytes("\x1b",1)` 退出;Up/Down 改变 `report_scroll`;Enter 不触发 submit 回调(g_submit_count 不变)。
- `test_report_exit_restores`:退出报告模式后渲染回到对话模式(header/status 恢复)。

## 3. 实施步骤(顺序执行,每步可验收)

1. **report 模块**:新增 `report.h`/`report.c`;`report_overall_percent()` 加权计算与硬编码 38 交叉校验;补 `tests/test_report.c`(可选,纯数据断言)。
2. **render 层**:`render.h` 加字段,`render.c` 加 report_mode 分支与仪表盘渲染。
3. **tui 层**:`tui.c` 报告接口 + 键绑定;保证 choice_mode 与 report_mode 互斥(进入报告时若 choice 激活先 stop)。
4. **main 集成**:`/report` 命令、取消回调、submit 拦截、`/help`。
5. **构建接入**:CMakeLists 加 `src/report/report.c`。
6. **测试**:`test_tui.c` 新增 4 个测试,并加入 `CAGENT_TESTS`(若新增 test_report 则同步注册)。
7. **验证**:`cmake --build build && ctest --test-dir build` 全绿(Debug + ASan 双构建);`clang-tidy` 新文件零告警;`valgrind` 抽查无泄漏;pty 端到端手工验证 `/report` 进入/滚动/退出。

## 4. 验收标准

- [ ] `/report` 进入全屏仪表盘:标题、加权总完成度、6 维度条形图、分级缺口清单、推进顺序全部显示
- [ ] 内容超一屏可滚动,滚动提示正确,退出后对话内容与状态完整恢复
- [ ] Esc/Ctrl+C/Enter 均能退出报告模式,不会误提交消息
- [ ] 报告模式与 `/model` choice 模式互斥不冲突
- [ ] 27+4 个 ctest 全绿(新增 4 个报告测试),Debug + ASan 双构建无泄漏
- [ ] `src/report/` 新增文件 clang-tidy 零告警

## 5. 风险与对策

| 风险 | 对策 |
|---|---|
| 块字符 █/░ 的 wcwidth 在部分终端为 2 列导致条形图错位 | 渲染用现有 `utf8_fit_width` 统一度量;若终端异常仍只影响视觉不崩溃(纯文本渲染) |
| report_mode 与 choice_mode 状态叠加 | 进入任一模式前先 stop 另一个;`tui_render_screen` 分支互斥(if/else) |
| report 数据与 TODO.md 双份维护漂移 | report.c 顶部注释标注数据来源与更新约定(自举时改 C 数据即可);加权完成度由公式计算 |
| Enter 在报告模式误提交输入 | tui.c 报告模式拦截 Enter;main.c app_submit 二次拦截 |
| 报告超长时滚动边界越界 | report_scroll 钳制到 [0, max_scroll],复用现有滚动边界逻辑 |
| TUI 测试在无终端环境失败 | 沿用现有非 TTY 测试模式:pipe + raw mode 不激活,渲染走纯函数 |

## 6. 后续演进(本计划外)

- 报告模式与审批流(TODO.md P0)结合:审批时全屏 diff 预览复用仪表盘布局
- 运行时数据接入:自诊断维度从"硬编码 0%"升级为运行时读取内存/并发指标
- `/report` 支持 `/report <维度>` 深钻详情(缺口子项完整列表)
