# Linux 6.6 调度学习裁剪版

这是从同级目录 `../linux-6.6` 复制出的只读学习材料，原始源码没有修改。

## 保留内容

- `kernel/sched/`：调度核心、CFS/EEVDF、RT、Deadline、idle、PELT、拓扑与调试代码
- `drivers/cpufreq/`：cpufreq 通用框架、governor 以及 ACPI、Intel、AMD、Device Tree 等平台驱动
- `drivers/opp/`：Operating Performance Points（频率/电压档位）通用框架，供 Device Tree 等 cpufreq 驱动使用
- `Documentation/scheduler/`：内核官方调度文档
- `Documentation/admin-guide/pm/`、`Documentation/power/`：cpufreq、pstate 与能耗模型文档
- `include/linux/sched/`、`include/linux/sched.h`、`include/linux/cpufreq.h`：调度器和 cpufreq 主要数据结构、接口
- `include/linux/pm_opp.h`、`energy_model.h`、`amd-pstate.h`、`regulator/consumer.h`：调频相关接口头文件
- `include/uapi/linux/sched.h`：用户态调度接口定义
- `kernel/fork.c`、`kernel/exit.c`：任务创建与退出的关键路径
- `kernel/time/tick-sched.c`：时钟 tick 与调度触发相关代码
- 顶层 `Makefile`、`Kconfig`、`Kbuild`、`README`、许可证与维护者信息，便于定位原始版本

## 使用边界

该目录面向源码阅读和调度流程追踪，不承诺能够独立编译内核；未包含完整架构、驱动、文件系统和构建依赖。

建议阅读顺序：

1. `Documentation/scheduler/sched-design-CFS.rst`
2. `kernel/sched/core.c` 中的 `schedule()` / `__schedule()`
3. `kernel/sched/fair.c` 中的 `pick_task_fair()`、`__pick_eevdf()` 及 vruntime/deadline 更新
4. `kernel/sched/rt.c`、`deadline.c`、`idle.c` 对比不同调度类
5. `kernel/sched/cpufreq_schedutil.c` → `drivers/cpufreq/cpufreq.c` → 一个具体平台驱动，追踪调度器到硬件调频的链路

裁剪日期：2026-09-02。来源目录：`../linux-6.6`。
