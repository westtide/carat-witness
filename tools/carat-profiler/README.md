# Carat Live Profiler

实时性能分析与算法可视化系统：Carat 进程内探针（C++）→ Unix Domain Socket
二进制事件流 → 独立 Daemon（Go，纯标准库）→ Web Dashboard。

设计文档：

- `docs/profiler_instrumentation.md` —— 源码埋点映射（阶段 ID 与真实函数对照）
- `docs/profiler_protocol.md` —— 事件协议 v1

## 快速开始

```bash
# 1. 构建 carat（含探针）与 daemon
cd build && ninja carat carat_prof_test && cd ..
(cd tools/carat-profiler/server && go build -o carat-profiler .)

# 2. 启动 daemon（默认 127.0.0.1:8765，数据落盘 ./data）
tools/carat-profiler/server/carat-profiler serve \
    --socket /tmp/carat-prof.sock --http 127.0.0.1:8765 --data ./data

# 3. 带 trace 运行 carat（summary 级别）
./build/carat model.btor2 -a ic3 --trace-socket /tmp/carat-prof.sock

# 浏览器打开 http://127.0.0.1:8765
```

或一键演示：`tools/carat-profiler/demo.sh model.btor2 ic3`

## Carat 端参数

| 参数 | 说明 |
| --- | --- |
| `--trace-socket <path>` | daemon 的 Unix socket；启用实时采集 |
| `--trace-file <path>` | 离线模式/镜像：直接写二进制 trace 文件 |
| `--trace-level summary\|detail` | summary（默认）：阶段+迭代+聚合计数+活跃 SAT 心跳；detail：另加逐条 SAT 查询与细粒度 span |

不指定 socket/file 时探针完全关闭，行为与原版一致。

## Dashboard 页面

- **Run**：算法/模型/当前阶段路径/结果/CPU/RSS/峰值内存/事件丢失告警
- **Algorithm Timeline**：阶段甘特图（滚轮缩放、拖动平移；颜色按阶段组）
- **Stage 耗时**：按 wall 时间排序的阶段统计（父阶段含子阶段，非相加关系）
- **SAT 查询**：分类统计、p50/p95/p99（detail 级）、正在执行且持续增长的查询（>10s 红色高亮）
- **算法状态**：帧/深度迭代序列与计数器（obligation、lemma、unsat core、传播、restart…）
- **模型统计**：BTOR2 节点数、COI 裁剪、AIG 规模、数组 selector/slot

## HTTP/WS API

- `GET /api/runs` —— 运行列表（含 daemon 重启前的历史）
- `GET /api/run/<id>/summary` —— 聚合快照（与 WS 推送同构）
- `WS /ws?id=<run_id>` —— 每 250ms 推送快照，run 结束后推送最后一帧即关闭

## 离线分析

```bash
python3 tools/carat-profiler/server/cprof_dump.py data/<run_id>/events.cprof
```

打印阶段耗时、SAT 分类聚合、计数器、结果码、事件丢失数。`--trace-file`
产生的文件与 daemon 落盘文件同格式。

## 数据目录

```
data/<run_id>/metadata.json   # 握手元数据（算法、模型、commit、pid…）
data/<run_id>/events.cprof    # 原始帧流（cprof_dump.py 可直接解析）
data/<run_id>/summary.json    # 结束/断连时的最终快照
```

## 可靠性行为

- daemon 死亡/重启：carat 不受影响（send 限时 500ms，断开后重连，期间事件
  丢弃并计入 `EVENTS_DROPPED`，页面明确标记）
- carat 被 `kill -9`：daemon 判定 disconnected，已收事件全部保留
- 浏览器关闭：不影响采集与落盘
- Summary 级别在 36s 的 KIND 基准上实测开销 ≈ 0%（目标 ≤5%）

## 测试

```bash
cd build && ninja carat_prof_test && ./carat_prof_test   # 环形缓冲/协议/离线往返
```

## 已知边界（第一版）

- 冲突数/决策数/传播数：底层求解器未暴露统计接口，按“未采集”处理
- nVars/nClauses 公式规模：同上
- summary 级别 p50/p95/p99 显示为 0（需要逐查询数据，用 `--trace-level detail`）
- KIND witness 模式（`-w`）下 lazy simple-path 被禁用，KIND_SIMPLEPATH 阶段缺失属预期
