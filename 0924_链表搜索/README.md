# 链表搜索与 PMC 统计（0924 课1-2 开发题）

按作业要求实现：单链表顺序查找（**不使用 HASH 加速**），用 `perf_event_open()` 统计链表搜索的
指令数、周期数、访存次数和 L1 Cache 命中率。

## 构建与运行

```bash
make
./linked_list_pmc            # 默认 262144 个节点、4096 次查询
./linked_list_pmc 1048576 8192   # 指定节点数、查询次数
```

需要 Linux（perf_event_open 是 Linux 接口），Windows 下请在虚拟机或 WSL2 中运行。

## 程序做了什么

- 单链表节点：`struct node { int key; int pad; struct node *next; }`，共 16 字节；
  默认 262144 个节点 ≈ 4 MB，远大于 L1/L2，能稳定产生 Cache 缺失。
- 查找：`while (p != NULL && p->key != key) p = p->next;`，纯顺序查找，O(N)。
- 查询目标由 LCG 伪随机生成，范围取 `N + N/8`，约 1/9 的查询目标不在链表中（同时覆盖命中与未命中）。
- 统计只覆盖“查找循环”，前后分别对计数器 reset/enable 和 disable/read；预热阶段不计入。

## 统计的事件

| 指标 | perf 事件 / 计算方式 |
| --- | --- |
| 指令数 | `PERF_TYPE_HARDWARE` + `PERF_COUNT_HW_INSTRUCTIONS` |
| 周期数 | `PERF_TYPE_HARDWARE` + `PERF_COUNT_HW_CPU_CYCLES` |
| 访存次数 | `PERF_TYPE_HARDWARE` + `PERF_COUNT_HW_CACHE_REFERENCES`（另输出 cache-misses） |
| L1D 访问 | `PERF_TYPE_HW_CACHE` + `L1D \| READ<<8 \| ACCESS<<16` |
| L1D 缺失 | `PERF_TYPE_HW_CACHE` + `L1D \| READ<<8 \| MISS<<16` |
| L1 命中率 | `(L1D 访问 - L1D 缺失) / L1D 访问 × 100%` |
| CPI | `cycles / instructions` |

设置 `exclude_kernel = 1`、`exclude_hv = 1`，只统计被测进程用户态；
`read_format` 使用 `PERF_FORMAT_TOTAL_TIME_ENABLED | PERF_FORMAT_TOTAL_TIME_RUNNING`，
当同时打开的事件数超过硬件计数器数（多路复用）时，按 `enabled / running` 缩放计数值。

## 预期输出（示例格式）

```
==== perf_event_open (pid-wide, exclude_kernel=1) ====
instructions        :           12345678901
cycles              :           34567890123
CPI                 :                  2.800
cache references    :            2345678901   (访存次数)
cache misses        :             456789012
L1D loads           :            2222222222
L1D load misses     :             111111111
L1 hit rate         :                 95.00%
cycles/query        :              8437.50
found / queries     :               3640 / 4096
```

具体数值随 CPU、内存、编译选项和负载变化，运行一次即可得到本机结果。

## 结果解读

顺序查找的平均比较次数是 N/2，每次比较都要解引用一个节点：链表节点在内存中不连续，
访问模式是典型的“指针追逐”，Cache 行利用率低，L1 命中率有限、CPI 偏高（常见于 2~4 以上）。
若改用 HASH（本作业禁用），平均查找可降到 O(1)，L1 命中率与 CPI 会明显改善——
这正是作业要求“禁用 HASH”以便观察访存行为的原因。

## 已知环境限制（VMware 虚拟机）

在本机 Ubuntu 24.04 + VMware Workstation 17.5（Intel Alder Lake 主机，`vpmc.enable=TRUE`、
`vpmc.numCounters=8`）实测：内核虽然暴露了 `cpu_core`/`cpu_atom` PMU，但 `perf stat` 与本程序
读出的 `instructions`、`cycles`、`cache-references` 均为 **0** —— 该虚拟 PMU 并未真正计数。
因此需要物理 Linux 机器（或支持 PMU 直通的服务器/云主机）才能得到真实指标；
WSL2 与普通容器通常也不支持硬件 PMC 事件。

## 常见问题

- `perf_event_open failed: Permission denied`：`perf_event_paranoid` 过高，可临时
  `sudo sysctl kernel.perf_event_paranoid=2`（或使用 root 运行）。
- 虚拟机中事件不可用：需要向客户机暴露 PMU（KVM 用 `-cpu host`，VMware 需开启虚拟 CPU 性能计数器）。
- `No such file or directory`：内核未启用 PMU 或 `CONFIG_PERF_EVENTS`，换一台物理机/云主机运行。
