# Linux 进程调度

本文根据爱丁堡大学 Volker Seeker 的 *Process Scheduling in Linux* 翻译并按 Linux 6.6 改写，讲 Linux 内核怎样做任务调度。覆盖通用调度框架、调度类、公平调度、软实时调度，以及公平调度与实时调度各自的负载均衡。

结构仍按 Seeker 的笔记来。文中的代码、路径和结构体已经改成 **Linux 6.6.0**，对应本目录下的 `linux-6.6/`。英文原稿 `Process_Scheduling_in_Linux.md` 仍是 3.1 时代的原文，只作对照，不要拿它去对 6.6 源码。

6.6 里调度代码不在单独一个 `kernel/sched.c` 里，拆开了：

| 你要打开的文件 | 里面是什么 |
| --- | --- |
| `linux-6.6/kernel/sched/core.c` | 框架：`schedule()`、`scheduler_tick()`、`try_to_wake_up()` |
| `linux-6.6/kernel/sched/fair.c` | 普通任务：公平调度类（6.6 用 EEVDF） |
| `linux-6.6/kernel/sched/rt.c` | 软实时：`SCHED_FIFO` / `SCHED_RR` |
| `linux-6.6/kernel/sched/deadline.c` | `SCHED_DEADLINE`（比 RT 更高的一类） |
| `linux-6.6/kernel/sched/idle.c` | 空闲任务 |
| `linux-6.6/kernel/sched/stop_task.c` | 停止任务 |
| `linux-6.6/kernel/sched/sched.h` | 内部定义：`struct rq`、`cfs_rq`、`sched_class` |
| `linux-6.6/include/linux/sched.h` | `task_struct`、`sched_entity` |
| `linux-6.6/Documentation/scheduler/` | 调度文档 |

本文以本目录 `linux-6.6/Makefile` 标明的 **6.6.0** 为基准。代码示例围绕当前主题展开，调用链中的箭头表示阅读顺序；`CONFIG_SMP`、组调度和 `CONFIG_SCHED_CORE` 会使源码进入不同分支。

## 术语对照

文中关键概念第一次出现时会标英文名。这里再集中列一份，方便对照源码和英文资料。

| 中文 | 英文 |
| --- | --- |
| 进程调度 / 进程调度器 | process scheduling / process scheduler |
| 任务 | task（本文里指被调度的线程） |
| 进程 / 线程 | process / thread |
| 线程组 ID | thread group ID, TGID |
| 任务描述符 | `task_struct` |
| CPU 密集型 / I/O 密集型 | CPU-bound / I/O-bound |
| 实时任务 / 普通任务 | real-time (RT) task / normal task |
| 优先级 / nice 值 | priority / nice value |
| 调度类 / 调度策略 | scheduling class / scheduling policy |
| 调度框架 | scheduler skeleton |
| 运行队列 | runqueue |
| 抢占 | preemption |
| 上下文切换 | context switch |
| 需要重新调度标志 | `need_resched` |
| 完全公平调度器 | Completely Fair Scheduler, CFS（2.6.23 引入，6.6 更新了选人算法） |
| EEVDF | Earliest Eligible Virtual Deadline First（6.6 公平类用的算法） |
| 虚拟运行时 | virtual runtime, `vruntime` |
| 虚拟截止时间 | virtual deadline, `deadline` |
| 基础时间片 | base slice，`sysctl_sched_base_slice` |
| `SCHED_DEADLINE` | deadline 调度类，排在实时类前面 |
| 红黑树 | red-black tree |
| 公平组调度 | fair group scheduling |
| 调度实体 | scheduling entity, `sched_entity` |
| 软实时 | soft real-time |
| FIFO / 时间片轮转 | `SCHED_FIFO` / `SCHED_RR` (round robin) |
| 实时节流 / 带宽 | real-time throttling / bandwidth |
| 对称多处理 | SMP (symmetric multiprocessing) |
| 非统一内存访问 | NUMA |
| 负载均衡 | load balancing |
| 调度域 / 调度组 | scheduling domain / scheduling group |
| 缓存亲和性 | cache affinity |
| 超线程 | hyper-threading |
| 周期均衡 / 主动均衡 / 新空闲均衡 | periodic balancing / active balancing / newidle balancing |
| 根域 | root domain |
| CPU 优先级 | CPU priority, `cpupri` |
| 推送 / 拉取 | push / pull |

## 目录

1. [进程调度](#1-进程调度)
2. [任务分类](#2-任务分类)
3. [调度类](#3-调度类)
4. [主运行队列](#4-主运行队列)
5. [调度框架](#5-调度框架)
6. [调度算法简史](#6-调度算法简史)
7. [公平调度（CFS 与 EEVDF）](#7-公平调度cfs-与-eevdf)
8. [公平调度实现细节](#8-公平调度实现细节)
9. [软实时调度](#9-软实时调度)
10. [SMP 系统下的负载均衡](#10-smp-系统下的负载均衡)
11. [实时负载均衡](#11-实时负载均衡)
12. [参考资料](#12-参考资料)


<a id="1-进程调度"></a>

# 1. 进程调度

现在的 Linux 内核是一个多任务内核。因此，任何时刻都可以有不止一个进程存在，并且每个进程运行起来就像系统只存在它自己一个进程。进程调度器（process scheduler）负责何时运行哪个进程。这种情况下，调度器要完成下面的任务：

- 按调度策略分配处理器资源；普通任务按权重分享 CPU，实时任务遵循各自的优先级或截止时间规则
- 如果需要的话挑选合适的进程在下一次进程切换后运行，这需要考虑调度类型（scheduling class / policy）以及进程优先级
- 在 SMP 系统中要在多个处理器核心之间均衡进程运行

## 1.2. Linux 的进程和线程

在 Linux 中进程就是一组共享了线程组 ID （TGID）的线程和所需的必要资源，内核并不把进程和线程当成两套不同的调度对象。内核调度的是不同的线程而不是进程。因此名词 “任务（task）” 在本文中表示的是线程。

在 Linux 中 `task_struct`（`include/linux/sched.h`）是用来表示一个特定任务的全部信息的结构体。

<a id="2-任务分类"></a>

# 2. 任务分类

## 2.1. CPU 密集型 vs. I/O 密集型（CPU-bound vs. I/O-bound）

任务要么偏 CPU 密集型，要么偏 I/O 密集型。也就是说，一些线程会使用 CPU 进行大量的运算，而另一些则会花费很多时间等待相对较慢的 I/O 操作完成。在本文， I/O 操作可能是等待用户输入，磁盘或网络访问。

同时这种分类也强烈依赖于任务运行的系统。一个服务器或者 HPC（high performance computing，高性能计算）的负载通常是 CPU 限制任务，而桌面或者移动平台的负载主要是 I/O 限制任务。

Linux 操作系统运行在这些系统之上，也因此设计时就是要应对所有类型的任务。要解决这个问题， Linux 的调度器需要负责及时响应 I/O 限制型任务，提高 CPU 限制型任务的效率。如果任务要运行更长的时间周期，这样他们就能完成更多的工作，但是响应性会受影响。如果每个任务的时间周期短一些，则系统能更快地响应 I/O 事件，但是这又会导致花费更多的时间在运行调度算法在任务切换上，效率会受到影响。这就要求调度器需要有取有舍，并且要在两个需求之间取得平衡。

## 2.2. 实时 vs. 普通（real-time vs. normal）

Linux 根据任务设置的调度策略，把普通任务交给公平类，把 `SCHED_FIFO` / `SCHED_RR` 任务交给 RT 类，把 `SCHED_DEADLINE` 任务交给 deadline 类。RT 类优先于公平类，deadline 类又优先于 RT 类。内核不会因为一个任务“看起来很紧急”就自动把它归入 RT 类。

## 2.3. 任务优先级值

内核内部的优先级值越小，优先级越高；用户接口的 RT 优先级则越大越高。`kernel/sched/core.c` 的 `__normal_prio()` 给出了转换规则。先不考虑优先级继承造成的临时提升：

| 策略 | 用户设置 | 内核内部的普通优先级计算 |
| --- | --- | --- |
| `SCHED_FIFO` / `SCHED_RR` | `sched_priority` 为 1～99，99 最高 | `99 - rt_priority`，即 98～0 |
| 普通策略 | [nice 值][2.1] 为 -20～19，-20 权重最大 | `NICE_TO_PRIO(nice)`，即 100～139 |
| `SCHED_DEADLINE` | runtime、deadline、period 等参数 | `MAX_DL_PRIO - 1`，即 -1 |

`MAX_RT_PRIO` 是 100，但这不表示用户可以申请 100 个 RT 优先级。普通任务也不是按内部优先级严格排队：nice 主要通过权重影响公平调度，具体选人规则见第 7 章。

[2.1]: https://zh.wikipedia.org/wiki/Nice%E5%80%BC

<a id="3-调度类"></a>

# 3. 调度类（scheduling class）

Linux 的进程调度器是模块化的，它使用不同算法和策略调度不同类型任务。一个调度算法的实现被封装在一个所谓的*调度类*（scheduling class）。一个调度类提供接口给主调度器框架，框架就可以根据算法的实现来处理对应的任务。

`sched_class` 定义在 `kernel/sched/sched.h`。下面列出与当前流程有关的钩子，调度框架靠这些函数指针调用各个算法：

```c
struct sched_class {
    void (*enqueue_task) (struct rq *rq, struct task_struct *p, int flags);
    void (*dequeue_task) (struct rq *rq, struct task_struct *p, int flags);
    void (*yield_task)   (struct rq *rq);
    bool (*yield_to_task)(struct rq *rq, struct task_struct *p);

    void (*check_preempt_curr)(struct rq *rq, struct task_struct *p, int flags);

    struct task_struct *(*pick_next_task)(struct rq *rq);

    void (*put_prev_task)(struct rq *rq, struct task_struct *p);
    void (*set_next_task)(struct rq *rq, struct task_struct *p, bool first);
#ifdef CONFIG_SMP
    int (*balance)(struct rq *rq, struct task_struct *prev, struct rq_flags *rf);
    int  (*select_task_rq)(struct task_struct *p, int task_cpu, int flags);
    /* ... */
#endif
    void (*task_tick)(struct rq *rq, struct task_struct *p, int queued);
    /* ... */
};
```

调度类由链接脚本按段排列，遍历使用 `for_each_class()`。优先级从高到低是：

```text
stop_sched_class → dl_sched_class → rt_sched_class → fair_sched_class → idle_sched_class
```

停止类调度每个 CPU 上的[停止任务][3.1]，在调度类中优先级最高；它能在允许调度的时机优先于其他类运行，但不能越过禁止抢占的临界区。空闲类调度每个 CPU 上的空闲任务（idle task，也叫 swapper task），只在没有其他可运行任务时才跑。`dl` 是 `SCHED_DEADLINE`。`rt` 是软实时。`fair` 是普通任务，也就是后面要讲的 EEVDF。

各调度类用 `DEFINE_SCHED_CLASS(fair)` 这种宏定义，实现分别在 `fair.c`、`rt.c`、`deadline.c`、`idle.c`、`stop_task.c`。

[3.1]: https://stackoverflow.com/questions/15399782/what-is-the-use-of-stop-sched-class-in-linux-kernel

<a id="4-主运行队列"></a>

# 4. 主运行队列（runqueue）

主运行队列在每个 CPU 上都有一份，数据结构 `struct rq` 定义在 `kernel/sched/sched.h`。它记录了这个 CPU 上所有可运行的任务，以及负载、时钟、调度域相关的统计。里面和入门最相关的是：

- 一把锁，用来同步当前 CPU 的调度操作（6.6 字段名是 `__lock`）
```c
    raw_spinlock_t      __lock;
```
- 分别指向当前运行的任务、空闲任务、停止任务
```c
    struct task_struct __rcu    *curr;
    struct task_struct      *idle;
    struct task_struct      *stop;
```
- 三个调度类各自的运行队列
```c
    struct cfs_rq       cfs;
    struct rt_rq        rt;
    struct dl_rq        dl;
```

`cfs` 给普通任务，`rt` 给软实时，`dl` 给 `SCHED_DEADLINE`。后面读 `schedule()` 时，说的「这根 CPU 的运行队列」就是这个 `rq`。

<a id="5-调度框架"></a>

# 5. 调度框架（scheduler skeleton）

## 5.1. 调度器入口

`schedule()` 定义在 `kernel/sched/core.c`，是主动睡眠等场景常用的调度入口。抢占还会经过 `preempt_schedule()`、`preempt_schedule_irq()` 等入口，最终也到 `__schedule()`；并不是所有调度都先调用 `schedule()`。

`schedule()` 自己只做外围工作：禁止抢占，再进真正干活的 `__schedule()`。`__schedule()` 选出下一个任务放到 `next`，需要的话做上下文切换（context switch）。如果挑出来的还是现在这个 `prev`，那就什么都不换。

```c
asmlinkage __visible void __sched schedule(void)
{
    struct task_struct *tsk = current;

    sched_submit_work(tsk);
    do {
        preempt_disable();
        __schedule(SM_NONE);
        sched_preempt_enable_no_resched();
    } while (need_resched());
    sched_update_worker(tsk);
}
```

`__schedule()` 的关键骨架如下（省略内存屏障、调试、负载统计及切换后的回调等细节）。请对着 `kernel/sched/core.c` 里的同名函数看。

```c
static void __sched notrace __schedule(unsigned int sched_mode)
{
    struct task_struct *prev, *next;
    unsigned long *switch_count;
    unsigned long prev_state;
    struct rq_flags rf;
    struct rq *rq;
    int cpu;

    cpu = smp_processor_id();
    rq = cpu_rq(cpu);
    prev = rq->curr;

    local_irq_disable();
    rq_lock(rq, &rf);
    update_rq_clock(rq);

    switch_count = &prev->nivcsw;
    prev_state = READ_ONCE(prev->__state);
    if (!(sched_mode & SM_MASK_PREEMPT) && prev_state) {
        if (signal_pending_state(prev_state, prev)) {
            WRITE_ONCE(prev->__state, TASK_RUNNING);
        } else {
            deactivate_task(rq, prev, DEQUEUE_SLEEP | DEQUEUE_NOCLOCK);
        }
        switch_count = &prev->nvcsw;
    }

    next = pick_next_task(rq, prev, &rf);
    clear_tsk_need_resched(prev);
    clear_preempt_need_resched();

    if (likely(prev != next)) {
        rq->nr_switches++;
        RCU_INIT_POINTER(rq->curr, next);
        ++*switch_count;
        /* Also unlocks the rq: */
        rq = context_switch(rq, prev, next, &rf);
    } else {
        rq_unpin_lock(rq, &rf);
        raw_spin_rq_unlock_irq(rq);
    }
}
```

在启用内核抢占的配置下，内核空间里执行的任务也可能在允许抢占的位置被调度出去。`schedule()` 因此先调用 `preempt_disable()`，避免调度自己进行到一半又被抢走。

接着锁住当前 CPU 的运行队列。某一时刻只允许一个线程改这根队列。

然后看 `prev`。如果它不是被抢占、而是自己要睡（`prev->__state` 不是运行态），并且没有等着处理的信号，就调用 `deactivate_task()`，把它从运行队列拿掉。这个函数最终会走到该任务所属调度类的 `dequeue_task()`。下面保留进入调度类的关键调用：

```c
static inline void dequeue_task(struct rq *rq, struct task_struct *p, int flags)
{
    if (!(flags & DEQUEUE_NOCLOCK))
        update_rq_clock(rq);
    p->sched_class->dequeue_task(rq, p, flags);
}
```

当当前 CPU 即将空闲时，公平类的选人路径会调用 `newidle_balance()`，尝试从其他 CPU 拉取任务；它位于公平类的选人流程中。

接下来 `pick_next_task()` 从上到下问各个调度类：谁有可运行的任务。然后清掉 `need_resched` 标志。

`need_resched` 表示需要重新调度。`resched_curr(rq)` 会设置当前任务的相应标志，跨 CPU 时还可能发送重调度 IPI。内核在返回用户态、允许抢占等调度检查点处理它；置位本身不会立即完成上下文切换。

```c
static inline struct task_struct *
__pick_next_task(struct rq *rq, struct task_struct *prev, struct rq_flags *rf)
{
    const struct sched_class *class;
    struct task_struct *p;

    if (likely(!sched_class_above(prev->sched_class, &fair_sched_class) &&
           rq->nr_running == rq->cfs.h_nr_running)) {

        p = pick_next_task_fair(rq, prev, rf);
        if (unlikely(p == RETRY_TASK))
            goto restart;

        if (!p) {
            put_prev_task(rq, prev);
            p = pick_next_task_idle(rq);
        }

        return p;
    }

restart:
    put_prev_task_balance(rq, prev, rf);

    for_each_class(class) {
        p = class->pick_next_task(rq);
        if (p)
            return p;
    }

    BUG();
}
```

`__pick_next_task()` 在 `kernel/sched/core.c`，上面保留了完整控制流。它有一条公平类快捷路径，但 `pick_next_task_fair()` 在释放锁做新空闲均衡期间可能遇到更高类任务入队，此时返回 `RETRY_TASK`，必须跳到 `restart` 重新按类检查。外层 `pick_next_task()` 在启用核心调度时还有处理 SMT 兄弟线程协同选人的分支；这里先读普通路径。

挑出来的若还是 `prev`，就不切换。否则 `context_switch()` 处理地址空间切换或借用，并通过体系结构相关代码切换寄存器和内核栈。若两个线程共享地址空间，不一定需要更换页表。切换收尾会释放运行队列锁；任务以后再次运行、从自己的 `schedule()` 返回时，还会恢复对应的抢占状态。外层循环会再次检查 `need_resched`。

## 5.2. 调用调度器

下面三个常见场景会引起调度相关操作，但只有到了允许切换的位置、并且选中了不同任务，才会发生任务切换。

### 1. 周期性更新当前调度的任务

在周期 tick 工作时，函数 `scheduler_tick()` 会被时钟中断调用；NO_HZ 配置下某些时段可以停 tick。定义在 `kernel/sched/core.c`，更新运行队列时钟、负载，并让当前任务所属的调度类做一次周期性记账。下面是关键代码。

```c
void scheduler_tick(void)
{
    int cpu = smp_processor_id();
    struct rq *rq = cpu_rq(cpu);
    struct task_struct *curr = rq->curr;
    struct rq_flags rf;

    sched_clock_tick();

    rq_lock(rq, &rf);
    update_rq_clock(rq);
    curr->sched_class->task_tick(rq, curr, 0);
    calc_global_load_tick(rq);
    rq_unlock(rq, &rf);

    perf_event_task_tick();

#ifdef CONFIG_SMP
    rq->idle_balance = idle_cpu(cpu);
    trigger_load_balance(rq);
#endif
}
```

你可以看到 `scheduler_tick()` 调用了调度类的钩子 `task_tick()`，它是相应的调度类用来进行周期性任务更新。在其内部，调度类可以决定一个新任务是否需要被调度，以及设置 `need_resched`，请求在合适的检查点重新调度。

在 `scheduler_tick()` 结尾，SMP 分支会检查是否需要触发负载均衡；具体迁移工作通常由后续软中断执行。

### 2. 当前任务需要睡眠

任务等待 I/O 完成等事件时，可以进入睡眠。实际内核代码常用 `include/linux/wait.h` 中的等待宏，例如下面这个用法示意。这里假定 `q` 是已经初始化的 `wait_queue_head_t`，`condition` 是由调用者维护、唤醒方更新的条件：

```c
/* 用法示意，不是调度器函数的源码摘录。 */
ret = wait_event_interruptible(q, condition);
```

条件已经成立时，它直接返回 0。如果需要等待，内部会通过 `prepare_to_wait_event()` 把当前任务加入等待队列、设置 `TASK_INTERRUPTIBLE`，再检查条件，必要时调用 `schedule()`。收到需要中断等待的信号时，返回 `-ERESTARTSYS`，由调用者处理；正常结束等待时，通过 `finish_wait()` 清理等待项并恢复运行态。

等待队列记录“谁在等这个事件”，CPU 的运行队列记录“谁可以在这个 CPU 上运行”，两者用途不同。任务先设置睡眠状态，再进入 `schedule()`；若此时仍需要阻塞，`__schedule()` 会把它从运行队列移走。检查条件、登记等待项和设置状态的顺序关系到是否会丢失唤醒，因此读源码时应以等待宏的实现为准。

### 3. 唤醒睡眠任务

产生事件的代码通常会先更新等待条件，再调用对应等待队列上的 `wake_up()` 等接口。常见的任务唤醒路径最终到达 `kernel/sched/core.c` 的 `try_to_wake_up()`，简称 ttwu。

对于已经离开运行队列、需要重新入队的普通唤醒路径，可以按下面的顺序读。远端 CPU 的入队工作可能延后到目标 CPU 执行，因此不是每次都在同一个调用栈里完成：

```text
try_to_wake_up()
  → 状态匹配，必要时 select_task_rq() 选 CPU
  → ttwu_queue()
  → ttwu_do_activate()
      → activate_task() → enqueue_task() → 调度类 enqueue_task 钩子
      → check_preempt_curr()
      → ttwu_do_wakeup()
```

`ttwu_do_activate()` 的局部摘录如下。这里的 `en_flags` 已由前面的分支准备好：

```c
activate_task(rq, p, en_flags);
check_preempt_curr(rq, p, wake_flags);
ttwu_do_wakeup(p);
```

`activate_task()` 经由 `enqueue_task()` 调用该任务所属调度类的入队钩子，公平类对应 `enqueue_task_fair()`。这与睡眠时的 `deactivate_task()` → `dequeue_task()` 相对应。

随后 `check_preempt_curr()` 检查是否需要抢占。相同调度类交给该类自己的钩子；更高调度类的任务入队则请求重新调度。**这个抢占检查在 `ttwu_do_activate()` 中，不在 `ttwu_do_wakeup()` 中。** 后者在本地 6.6 源码中只做下面两件事：

```c
static inline void ttwu_do_wakeup(struct task_struct *p)
{
    WRITE_ONCE(p->__state, TASK_RUNNING);
    trace_sched_wakeup(p);
}
```

`TASK_RUNNING` 同时包含“正在执行”和“已经就绪等待 CPU”，所以唤醒成功并不表示马上获得 CPU。`try_to_wake_up()` 对仍在运行队列上、或尚未完成切出的任务还有快捷路径，任务已经在运行队列上或正在完成切换时，唤醒路径会走相应的快捷分支。

<a id="6-调度算法简史"></a>

# 6. 调度算法简史

Linux 调度器经历了几次重要演变，6.6 的实现建立在这些基础上：

| 内核阶段 | 调度实现的变化 |
| --- | --- |
| 2.4 时代 | 通过遍历任务、计算 goodness 等方式选择任务，常称为 O(N) 调度器 |
| 2.6 早期 | O(1) 调度器用优先级数组等结构组织任务，并使用交互性启发规则 |
| 2.6.23（2007） | 引入 CFS，以及支撑不同策略实现的模块化调度类框架 |
| 3.14（2014） | 引入 `SCHED_DEADLINE` |
| 6.6（2023） | 公平类引入 EEVDF 选择规则，仍保留 `cfs_rq` 等名称和公平调度基础设施 |

<a id="7-公平调度cfs-与-eevdf"></a>

# 7. 公平调度（CFS 与 EEVDF）

普通任务（`SCHED_NORMAL` / `SCHED_BATCH` / `SCHED_IDLE`）走公平调度类 `fair_sched_class`，实现在 `kernel/sched/fair.c`。这里的 `SCHED_IDLE` 是一种普通任务策略，不是每个 CPU 的 idle task，也不属于 `idle_sched_class`。

CFS 用虚拟运行时间描述任务已经获得的服务。Linux 6.6 在这套记账和组调度基础上引入 **EEVDF**，更新了挑选下一个实体的规则。源码仍沿用 `cfs_rq`、`sched_entity` 等名称。

先考虑一个 CPU、同一公平组内的两个持续可运行任务 A、B，且两者权重相同。理想情况下，它们各获得一半的 CPU 时间。真实 CPU 无法同时执行两者，只能轮流运行；调度器需要记录两者已经获得的服务，避免某一个长期占得过多。如果任务权重不同，目标份额也随权重改变。

## 7.1. 时间片 vs. 虚拟运行时

`sum_exec_runtime` 记录实际运行时间，`vruntime` 则记录按权重折算后的虚拟运行时间。忽略整数舍入时，`calc_delta_fair()` 对应：

\[
\Delta v = \Delta t \times \frac{\mathrm{NICE\_0\_LOAD}}{w}
\]

这里的 \(\Delta t\) 是新增的 CPU 执行时间，\(w\) 是实体的 `load.weight`，`NICE_0_LOAD` 是 nice 0 的参考权重。两种时间在源码中都以纳秒计数，但 `vruntime` 已经过权重折算。nice 0 的任务执行 1 ms，虚拟时间约增加 1 ms；权重更大的任务执行同样长的时间，虚拟时间增加得更少。

CFS 以较小 `vruntime` 的实体为主要选择依据，最左节点因此重要。6.6 的公平类仍使用 `vruntime` 记账，同时在选人时加入资格和虚拟截止时间。

EEVDF 的 `se->slice` 表示一次服务请求的实际时间长度。6.6 的 `place_entity()` 和 `update_deadline()` 都从 `sysctl_sched_base_slice` 给它赋值，并不是预测任务还要运行多久，也不是按当前队列的线程数给每个任务重新分配不同 slice。

归一化基础值是 0.75 ms。默认采用对数缩放，`get_update_sysctl_factor()` 按在线 CPU 数计算缩放因子，并把参与计算的 CPU 数限制到最多 8。因此实际 `sysctl_sched_base_slice` 不一定是 0.75 ms。nice 的作用主要体现在实际时间到虚拟时间的换算中。

## 7.2. 优先级

nice 越小，权重通常越大。同样执行 1 ms，大权重任务的 `vruntime` 增长更慢；要追上同样的虚拟时间进度，它可以获得更多实际 CPU 时间。这使同一公平队列中持续竞争的任务，长期按权重比例分享 CPU。

“增长更慢”描述记账速度，“当前 `vruntime` 更小”描述已经到达的位置，两者不能等同。一个权重很大的任务如果此前已经多跑了，也可能暂时没有 EEVDF 资格。nice 也不具有 RT 优先级那种“高优先级任务可运行就压过低优先级任务”的含义。

## 7.3. 应对 I/O 密集型和 CPU 密集型任务

考虑一个文本编辑器和一个视频编码器。编辑器大部分时间等待输入，编码器则持续计算。希望达到的效果是：编辑器等待时，编码器可以使用 CPU；输入到来后，编辑器有机会及时执行短暂的处理工作。

如果两个同权重任务持续可运行，它们的目标份额才近似各半。编辑器睡眠时并不参加这段时间的 CPU 竞争，睡眠时间不直接等于调度器需要补偿的 50% 服务。它醒来后也不保证立即抢占编码器。

在 6.6 中，`dequeue_entity()` 会调用 `update_entity_lag()` 保存并限制实体相对公平进度的虚拟差额 `vlag`；重新入队时，`place_entity()` 根据当前队列的平均虚拟时间和保存的差额放置它。这样可以保留有限的公平差额，而不让一次长睡眠积累无限补偿。

是否在唤醒时抢占，还要看 `check_preempt_wakeup()`。对于通常的 `SCHED_NORMAL` 竞争路径，它更新当前实体的记账，并检查 `pick_eevdf()` 是否选中唤醒实体。`SCHED_BATCH`、`SCHED_IDLE`、组调度以及正在运行实体的保护条件还会影响结果。

## 7.4. 创建新任务

`cfs_rq->min_vruntime` 是单调前进的虚拟时间基准，不是一个保证随时等于全部实体最小值的实时快照。源码用它作为相对记账的参考。

6.6 的 `place_entity()` 从 `avg_vruntime(cfs_rq)` 取得队列平均虚拟时间，再按保存的 lag 调整实体位置；新任务的位置由平均虚拟时间和保存的 lag 共同决定。新建实体由 `task_fork_fair()` 调用 `place_entity(..., ENQUEUE_INITIAL)` 初始化。默认启用 `PLACE_DEADLINE_INITIAL` 时，这次初始放置会把用于计算首个 deadline 的虚拟 slice 减半；对应代码是 `vslice /= 2`，不是把 `se->slice` 字段改成一半。

## 7.5. 运行队列 - 红黑树（red-black tree）

`cfs_rq->tasks_timeline` 是 `struct rb_root_cached`，包含红黑树根以及最左节点缓存 `rb_leftmost`。两者不是先后替换的名称。6.6 的树仍按实体的 `vruntime` 排序，同时在每个节点的 `min_deadline` 中维护该子树最小的虚拟截止时间，以支持 EEVDF 搜索。

正在运行的实体通常不在这棵树里，而由 `cfs_rq->curr` 单独保存；只要它仍可运行，仍参与公平记账和候选比较。`set_next_entity()` 在选中时把实体从树上摘下，`put_prev_entity()` 在它仍可运行时把它放回。因此“树上的节点数”和“可运行实体数”不是始终相等。

## 7.6. 公平组调度（fair group scheduling）

启用 `CONFIG_FAIR_GROUP_SCHED` 后，调度器可以先在组之间分配 CPU，再在组内任务之间分配。组本身也由 `sched_entity` 表示，它的 `my_q` 指向下一级 `cfs_rq`；任务与组各有自己的实体和记账，不是让所有组内线程共享一个 `vruntime`。

例如，两个同权重组在同一 CPU 上持续竞争，第一个组里有四个同权重任务，第二个组里只有一个任务。忽略配额、节流等限制，两个组各获得约一半 CPU，第一个组内的四个任务再分它们的一半，各约占整个 CPU 的八分之一。

这种分组需要由 cgroup 或相应的自动分组机制建立。单纯 fork 出多个子进程，不会自动形成一个独立、与另一个用户平分 CPU 的调度组。

## 7.7. 6.6 里实际用的 EEVDF

EEVDF 的名称是 Earliest Eligible Virtual Deadline First。它先判断实体有没有资格，再在符合资格的实体中比较虚拟截止时间。先忽略组调度和当前实体的继续运行保护，基本规则是：

1. **资格（eligible）**：实体的 `vruntime` 不大于队列的加权平均虚拟时间 \(V\)，即 \(v_i \le V\)。这表示它没有比公平进度多跑。
2. **虚拟截止时间（deadline）**：在有资格的实体里，选择 deadline 最早的那个。

`avg_vruntime(cfs_rq)` 函数计算平均虚拟时间；`cfs_rq->avg_vruntime` 字段本身保存的是相对 `min_vruntime` 的加权累加量，不能直接拿这个字段当平均值。`entity_eligible()` 使用不做除法的等价比较，减少整数舍入误差。

在 `update_deadline()` 中，一次请求用完后，新的虚拟截止时间按下面的关系计算。忽略整数舍入：

\[
d_i = v_i + r_i \times \frac{\mathrm{NICE\_0\_LOAD}}{w_i}
\]

\(r_i\) 对应 `se->slice`，是实际时间长度；\(v_i\) 和 \(d_i\) 分别对应 `se->vruntime` 和 `se->deadline`。它们不是现实世界中的完成时刻，也不预测程序何时结束。

假设三个同权重实体的 `vruntime` 分别为 4、5、6 个虚拟时间单位，那么 \(V=5\)，A、B 有资格，C 暂时没有资格。若这是一次不受当前实体继续运行保护影响的选择，且 A、B 的 deadline 分别是 7、6，就会选 B。即使 C 的 deadline 更早，也不能跳过资格判断。这个例子说明：资格取决于公平进度，最终选择还要比较 deadline，还要先检查资格，再比较 deadline。

本地 6.6 的默认实现还有 `RUN_TO_PARITY`。在 `__pick_eevdf()` 中，如果当前实体仍有资格，且保留着 `set_next_entity()` 设置的本次请求标记（`curr->vlag == curr->deadline`），会直接让它继续运行。这有助于减少频繁抢占。因此“有一个新任务的 deadline 更早”本身不足以保证它马上抢占当前任务。第 8 章会把这条分支与树上的搜索对应起来。

<a id="8-公平调度实现细节"></a>

# 8. 公平调度实现细节

## 8.1. 数据结构

公平类给每个任务（以及每个任务组）一个 `sched_entity`，挂在 `task_struct` 里。定义在 `include/linux/sched.h`。下面列出与公平记账、EEVDF 和组调度直接相关的字段：

```c
struct sched_entity {
    struct load_weight      load;
    struct rb_node          run_node;
    u64             deadline;
    u64             min_deadline;

    struct list_head        group_node;
    unsigned int            on_rq;

    u64             exec_start;
    u64             sum_exec_runtime;
    u64             prev_sum_exec_runtime;
    u64             vruntime;
    s64             vlag;
    u64             slice;
#ifdef CONFIG_FAIR_GROUP_SCHED
    struct sched_entity     *parent;
    struct cfs_rq           *cfs_rq;
    struct cfs_rq           *my_q;
#endif
};
```

每个 CPU 的 `rq` 里有一个 `cfs` 成员，类型是 `cfs_rq`，定义在 `kernel/sched/sched.h`。入门先看这几个字段：可运行个数、`min_vruntime`、红黑树 `tasks_timeline`、当前实体 `curr`。实体的 `load_weight` 保存权重；`cfs_rq->load` 汇总这一层队列的权重。下面为相关结构。

```c
struct cfs_rq {
    struct load_weight  load;
    unsigned int        nr_running;

    s64         avg_vruntime;
    u64         avg_load;

    u64         exec_clock;
    u64         min_vruntime;

    struct rb_root_cached   tasks_timeline;

    struct sched_entity *curr;
    struct sched_entity *next;
    /* ... 组调度、SMP 负载平均等 ... */
};
```

## 8.2. 时间审计

`vruntime` 记录实体按权重折算的运行进度，当前正在运行的实体也有这个字段。`scheduler_tick()` 会调公平类的 `task_tick()`，也就是 `task_tick_fair()`（`kernel/sched/fair.c`）。下面列出逐层记账的关键操作：


```c
/*
* scheduler tick hitting a task of our scheduling class:
*/
static void task_tick_fair(struct rq *rq, struct task_struct *curr, int queued)
{
    struct cfs_rq *cfs_rq;
    struct sched_entity *se = &curr->se;
    for_each_sched_entity(se) {
        cfs_rq = cfs_rq_of(se);
        entity_tick(cfs_rq, se, queued);
    }

}
```

`task_tick_fair()` 对每个调度实体调一次 `entity_tick()`。6.6 的 `entity_tick()` 主要负责记账，不再调用早期 CFS 实现中的 `check_preempt_tick()`。该不该让出 CPU，改在 `update_curr()` → `update_deadline()` 里判断。

```c
static void
entity_tick(struct cfs_rq *cfs_rq, struct sched_entity *curr, int queued)
{

    update_curr(cfs_rq);

    update_load_avg(cfs_rq, curr, UPDATE_TG);
    update_cfs_group(curr);

#ifdef CONFIG_SCHED_HRTICK

    if (queued) {
        resched_curr(rq_of(cfs_rq));
        return;
    }

    if (!sched_feat(DOUBLE_TICK) &&
            hrtimer_active(&rq_of(cfs_rq)->hrtick_timer))
        return;
#endif
}
```

`update_curr()` 用 `rq_clock_task()` 算出新增执行时间 `delta_exec`，按权重折进 `vruntime`，再检查是否需要更新截止时间。任务时钟会扣除被单独统计的 IRQ、steal 等时间，不宜直接叫普通墙钟时间。这里先看执行时间、vruntime 和 deadline 的更新：

```c
static void update_curr(struct cfs_rq *cfs_rq)
{
    struct sched_entity *curr = cfs_rq->curr;
    u64 now = rq_clock_task(rq_of(cfs_rq));
    u64 delta_exec;

    if (unlikely(!curr))
        return;

    delta_exec = now - curr->exec_start;
    if (unlikely((s64)delta_exec <= 0))
        return;

    curr->exec_start = now;
    curr->sum_exec_runtime += delta_exec;
    curr->vruntime += calc_delta_fair(delta_exec, curr);
    update_deadline(cfs_rq, curr);
    update_min_vruntime(cfs_rq);
}
```

`calc_delta_fair()` 就是按 `load_weight` 加权：权重大（nice 更小）的任务，同样跑 1 ms，`vruntime` 加得少。

`update_deadline()` 是 EEVDF 里「这份请求用完了没有」的判断：

```c
static void update_deadline(struct cfs_rq *cfs_rq, struct sched_entity *se)
{
    if ((s64)(se->vruntime - se->deadline) < 0)
        return;

    se->slice = sysctl_sched_base_slice;

    se->deadline = se->vruntime + calc_delta_fair(se->slice, se);

    if (cfs_rq->nr_running > 1) {
        resched_curr(rq_of(cfs_rq));
        clear_buddies(cfs_rq, se);
    }
}
```

`vruntime` 还没走到 `deadline` 时，这个函数不因请求耗尽而要求重新调度；更高调度类抢占、任务睡眠等仍可能使它离开 CPU。一旦走到或超过，就生成新的 deadline；只有这一层 `cfs_rq->nr_running > 1` 时才设置 `need_resched`。入队、出队时也会调用 `update_curr()`，不只是 tick。

## 8.3. 修改红黑树

任务从运行队列拿掉时走 `dequeue`，唤醒时走 `enqueue`。公平类对应 `enqueue_task_fair()` / `dequeue_task_fair()`，最后落到 `enqueue_entity()` / `dequeue_entity()`。

`enqueue_entity()` 会更新记账，用 `place_entity()` 放置实体，再按需要调用 `__enqueue_entity()` 插入 `tasks_timeline`。若入队对象就是当前实体，则不会把它重复插入树中。下面列出插入公平队列的关键操作：

```c
static void
enqueue_entity(struct cfs_rq *cfs_rq, struct sched_entity *se, int flags)
{
    bool curr = cfs_rq->curr == se;

    if (curr)
        place_entity(cfs_rq, se, flags);

    update_curr(cfs_rq);
    update_load_avg(cfs_rq, se, UPDATE_TG | DO_ATTACH);
    se_update_runnable(se);
    update_cfs_group(se);

    if (!curr)
        place_entity(cfs_rq, se, flags);

    account_entity_enqueue(cfs_rq, se);
    if (!curr)
        __enqueue_entity(cfs_rq, se);
    se->on_rq = 1;
}
```

`__enqueue_entity()` 把节点插进按 `vruntime` 排序的增强红黑树，并维护子树的 `min_deadline`：

```c
static void __enqueue_entity(struct cfs_rq *cfs_rq, struct sched_entity *se)
{
    avg_vruntime_add(cfs_rq, se);
    se->min_deadline = se->deadline;
    rb_add_augmented_cached(&se->run_node, &cfs_rq->tasks_timeline,
                __entity_less, &min_deadline_cb);
}
```

出队对称：先 `update_curr()`，再记下 lag，然后 `__dequeue_entity()` 从树上摘掉。中间那些 `update_load_avg()`、`update_cfs_group()` 是组调度和负载平均，第一遍读可以先跳过。

## 8.4. 挑选下一个可运行的任务

框架的公平类快捷路径直接调用 `pick_next_task_fair(rq, prev, rf)`；遍历调度类时，`pick_next_task` 钩子则是 `__pick_next_task_fair(rq)`，由它调用 `pick_next_task_fair(rq, NULL, NULL)`。组调度时沿层级选实体，每层调用 `pick_next_entity()`，直到得到实际任务。

6.6 的 `pick_next_entity()` 直接使用 EEVDF 选择规则，buddy 只在相应调度特性开启时参与，默认就是 EEVDF：

```c
static struct sched_entity *
pick_next_entity(struct cfs_rq *cfs_rq, struct sched_entity *curr)
{

    if (sched_feat(NEXT_BUDDY) &&
        cfs_rq->next && entity_eligible(cfs_rq, cfs_rq->next))
        return cfs_rq->next;

    return pick_eevdf(cfs_rq);
}
```

`pick_eevdf()` 调用 `__pick_eevdf()`。后者先检查当前实体是否仍可运行、是否 eligible，再处理第 7.7 节说明的 `RUN_TO_PARITY` 继续运行分支；需要比较其他实体时，利用树上的 `min_deadline` 做 \(O(\log n)\) 搜索。当前实体不在树中，因此代码会单独把它纳入比较。`NEXT_BUDDY` 在本地 `features.h` 中默认关闭，开启后也会影响选人顺序。

若 EEVDF 没找到人，会退回最左边（`vruntime` 最小）那个，并打一行错误日志：

```c
static struct sched_entity *pick_eevdf(struct cfs_rq *cfs_rq)
{
    struct sched_entity *se = __pick_eevdf(cfs_rq);

    if (!se) {
        struct sched_entity *left = __pick_first_entity(cfs_rq);
        if (left) {
            pr_err("EEVDF scheduling fail, picking leftmost\n");
            return left;
        }
    }

    return se;
}
```

最左边节点现在从缓存树取：

```c
struct sched_entity *__pick_first_entity(struct cfs_rq *cfs_rq)
{
    struct rb_node *left = rb_first_cached(&cfs_rq->tasks_timeline);

    if (!left)
        return NULL;

    return __node_2_se(left);
}
```

公平类的钩子表在 `fair.c` 末尾的 `DEFINE_SCHED_CLASS(fair)`。对照源码时，从这张表跳到各个函数最省事。

<a id="9-软实时调度"></a>

# 9. 软实时（soft real-time）调度

Linux 调度器支持软实时（RT）任务调度。这意味着内核可以有效的调度有严格时间限制的任务。FIFO/RR 本身不承诺任务一定在某个截止时间前完成；中断、临界区、其他高优先级任务和带宽限制都会影响响应。

对应的调度类是 `kernel/sched/rt.c` 里的 `rt_sched_class`。RT 任务优先级高于公平类。6.6 里它上面还有 `dl_sched_class`（`SCHED_DEADLINE`，`kernel/sched/deadline.c`）。入门先把 FIFO/RR 看完，deadline 类以后再说。

## 9.1. 调度模式

由 RT 调度器处理的任务可以配置成下面两种不同的模式：

- SCHED_FIFO ： FIFO（First In, First Out，先入先出）任务没有轮转时间片；它可以持续运行，直到阻塞、结束、自愿让出 CPU、被更高优先级任务抢占，或受 RT 带宽节流等条件限制。
- SCHED_RR ： RR 任务使用轮转时间片。时间片耗尽时会重新填充；若同一优先级队列还有竞争实体，就把它移到队尾并请求重新调度。没有同级竞争者时，可以继续运行。它也会因阻塞、更高优先级任务或带宽限制而提前离开 CPU。

## 9.2. 优先级

根据优先级的实现内容，RT 调度类也遵循之前 O(1) （复杂度）调度器同样的规则。内核使用多个运行队列，每个队列对应一个优先级。按照这种方式，添加、删除或者寻找最高优先级任务的操作可以在 O(1) 时间内完成。

## 9.3. 实时节流（real-time throttling）

RT 调度器实现里偶尔能看到节流（throttling）和带宽（bandwidth）相关操作。它们是给实时任务加的安全阀：默认 `sched_rt_period_us=1000000`、`sched_rt_runtime_us=950000`，即每个 1 秒周期给 RT 的运行预算通常为 0.95 秒，用来限制 RT 长期独占 CPU。RT 带宽共享、组配置及其他调度类仍会影响实际表现，这不是每个 RT 任务各有 95% 的 CPU，也不是普通任务的无条件时延保证。6.6 里这套机制还在，`rt_rq` 上的 `rt_throttled` / `rt_runtime` 就是干这个的。

## 9.4. 实现细节

### 数据结构

和公平类类似，RT 也有自己的调度实体和运行队列，分别挂在 `task_struct` 和 `rq` 上。

`sched_rt_entity` 在 `include/linux/sched.h`。它有时间片、所在优先级链表，以及组调度相关成员。

```c
struct sched_rt_entity {
    struct list_head        run_list;
    unsigned long           timeout;
    unsigned long           watchdog_stamp;
    unsigned int            time_slice;
    unsigned short          on_rq;
    unsigned short          on_list;

    struct sched_rt_entity      *back;
#ifdef CONFIG_RT_GROUP_SCHED
    struct sched_rt_entity      *parent;
    struct rt_rq            *rt_rq;
    struct rt_rq            *my_q;
#endif
};
```

`rt_rq` 在 `kernel/sched/sched.h`。第一个字段是优先级数组。其余大多和 SMP、组调度有关。

```c
struct rt_rq {
    struct rt_prio_array active;
    unsigned int rt_nr_running;
    unsigned int rr_nr_running;
#if defined CONFIG_SMP || defined CONFIG_RT_GROUP_SCHED
    struct {
        int curr;
#ifdef CONFIG_SMP
        int next;
#endif
    } highest_prio;
#endif
#ifdef CONFIG_SMP
    unsigned int rt_nr_migratory;
    unsigned int rt_nr_total;
    int overloaded;
    struct plist_head pushable_tasks;
#endif
    int rt_queued;
    int rt_throttled;
    u64 rt_time;
    u64 rt_runtime;
    raw_spinlock_t rt_runtime_lock;
#ifdef CONFIG_RT_GROUP_SCHED
    unsigned int rt_nr_boosted;
    struct rq *rq;
    struct task_group *tg;
#endif
};
```

### 时间记账

`scheduler_tick()` 调用 `task_tick_rt()` 更新当前任务的时间片。实现相当简单：一开始就使用 `update_curr_rt()` 更新当前任务的运行时数据和对应的运行队列。随后检查策略，不是 `SCHED_RR` 就直接返回，不做 RR 时间片递减。

RR 任务每次 tick 把 `time_slice` 减 1。减到 0 后，重新设为 `sched_rr_timeslice`；若本层或组调度的上层优先级链表还有其他实体，就把任务重新排到队尾并设置 `need_resched`。这个计数使用 tick，不是每调用一次就扣 1 ms。

```c
static void task_tick_rt(struct rq *rq, struct task_struct *p, int queued)
{
    struct sched_rt_entity *rt_se = &p->rt;

    update_curr_rt(rq);
    update_rt_rq_load_avg(rq_clock_pelt(rq), rq, 1);

    watchdog(rq, p);

    if (p->policy != SCHED_RR)
        return;

    if (--p->rt.time_slice)
        return;

    p->rt.time_slice = sched_rr_timeslice;

    for_each_sched_rt_entity(rt_se) {
        if (rt_se->run_list.prev != rt_se->run_list.next) {
            requeue_task_rt(rq, p, 0);
            resched_curr(rq);
            return;
        }
    }
}
```


### 挑选下一个运行的任务

挑选下一个 RT 任务仍是常数时间。6.6 的 `pick_next_task_rt()` 先调 `pick_task_rt()`，找到任务后再调用 `set_next_task_rt()`。真正逐层找人的是 `_pick_next_task_rt()`。执行起始时间由 `set_next_task_rt()` 设置。

```c
static struct task_struct *pick_next_task_rt(struct rq *rq)
{
    struct task_struct *p = pick_task_rt(rq);

    if (p)
        set_next_task_rt(rq, p, true);

    return p;
}

static struct task_struct *pick_task_rt(struct rq *rq)
{
    struct task_struct *p;

    if (!sched_rt_runnable(rq))
        return NULL;

    p = _pick_next_task_rt(rq);

    return p;
}
```

如果没有可以运行的任务，就返回 NULL，然后其它调度类会寻找可运行的任务。

`pick_next_rt_entity()` 会从运行队列中选出优先级最高的任务作为下一个运行的任务。


```c
static struct task_struct *_pick_next_task_rt(struct rq *rq)
{
    struct sched_rt_entity *rt_se;
    struct rt_rq *rt_rq  = &rq->rt;

    do {
        rt_se = pick_next_rt_entity(rt_rq);
        if (unlikely(!rt_se))
            return NULL;
        rt_rq = group_rt_rq(rt_se);
    } while (rt_rq);

    return rt_task_of(rt_se);
}
```

在 `pick_next_rt_entity()` 中你可以看到如何使用位图来在全部优先级中快速的找出拥有可运行任务的优先级最高的队列。优先级队列本身是一个简单的链表，而下一个可运行的任务可以在它的表头找到。


```c
static struct sched_rt_entity *pick_next_rt_entity(struct rt_rq *rt_rq)
{
    struct rt_prio_array *array = &rt_rq->active;
    struct sched_rt_entity *next = NULL;
    struct list_head *queue;
    int idx;

    idx = sched_find_first_bit(array->bitmap);
    BUG_ON(idx >= MAX_RT_PRIO);

    queue = array->queue + idx;
    if (SCHED_WARN_ON(list_empty(queue)))
        return NULL;
    next = list_entry(queue->next, struct sched_rt_entity, run_list);

    return next;
}
```

在优先级队列中添加、删除任务也是非常的简单。仅仅就是找出正确的队列然后设置或者清掉优先级位图中对应位置。因此我就不在这里提供更进一步的细节了。

<a id="10-smp-系统下的负载均衡"></a>

# 10. SMP 系统下的负载均衡

一个 CPU 的公平调度解决“本地下一步运行谁”，SMP 负载均衡解决“任务放在哪个 CPU 上”。例如 CPU 0 上有三个可运行任务，CPU 1 却空闲，把一个允许迁移的任务交给 CPU 1，通常比继续在 CPU 0 上排队更合适。

迁移也有代价。任务刚用过的数据可能仍在原 CPU 的缓存中，NUMA 系统还涉及内存访问距离；大小核的执行能力也不同。因此 6.6 不会只比较各 CPU 的任务个数。公平类均衡主要在 `kernel/sched/fair.c`，调度域的建立和维护主要在 `kernel/sched/topology.c`。

## 10.1. 调度域和调度组（scheduling group）

**调度域** `sched_domain` 指定一组参与负载比较的 CPU，以及在这组 CPU 上采用的均衡规则。每个 CPU 可以有从小到大的域层级，例如先考虑同核的 SMT 线程，再考虑共享缓存的一组核心，最后考虑更远的 CPU。实际层级由硬件拓扑、配置和 CPU 隔离设置决定，并非每台机器都相同。

域内的 `sched_group` 把 CPU 分成若干组。均衡时先比较组，再在选中的组内寻找合适的源运行队列。它与第 7 章 cgroup 的公平任务组不同：这里分组的对象是 CPU，不是任务。

![](2016-08-04-001245_1178x602_scrot.png)

这张原文示意图用四个逻辑 CPU 展示层级关系。可以把相邻的两个逻辑 CPU 理解为同一物理核心上的 SMT 线程，在较低层比较线程之间的负载，再在较高层比较两个核心。真实 6.6 系统可能合并冗余层级，NUMA 系统也可能有多个距离层级，不应把图中的层数当成固定规则。

## 10.2. 负载均衡

6.6 中需要区分下面几条路径：

| 场景 | 主要入口 | 要完成的工作 |
| --- | --- | --- |
| 周期性检查 | `trigger_load_balance()` → `run_rebalance_domains()` → `rebalance_domains()` | 到期后比较各域，必要时拉取任务 |
| CPU 即将空闲 | `newidle_balance()` | 尽快从别处找一个合适的公平任务 |
| 唤醒、新建或 exec | 公平类的 `select_task_rq_fair()` | 为这个任务选择 CPU |
| 普通拉取未成功且满足主动均衡条件 | `active_load_balance_cpu_stop()` | 让源 CPU 的 stopper 协助推送任务 |

**周期均衡不等于主动均衡。** 周期均衡的正常迁移方向是把任务从较忙的源 CPU 拉向目标 CPU；源码中 `active_balance` 特指需要源 CPU stopper 协助的路径。NO_HZ 下，还可能由一个 CPU 代表停止 tick 的空闲 CPU 执行均衡。

均衡既考虑任务负载、利用率和 CPU 容量，也考虑亲和性、缓存、NUMA 等约束。有些任务即使位于较忙 CPU，也不能迁到当前目标 CPU。找不到可迁移任务，并不表示它们都被“缓存亲和性固定住了”：硬性的 CPU 允许掩码与迁移成本判断是不同限制。

## 10.3. 实现细节

### 数据结构

`sched_domain` 定义在 `include/linux/sched/topology.h`。下面是与本章相关的相关字段：

```c
struct sched_domain {
    struct sched_domain __rcu *parent;
    struct sched_domain __rcu *child;
    struct sched_group *groups;
    unsigned long min_interval;
    unsigned long max_interval;
    unsigned int busy_factor;
    unsigned int imbalance_pct;
    unsigned int cache_nice_tries;
    /* ... */
    int flags;
    int level;
    unsigned long last_balance;
    unsigned int balance_interval;
    unsigned int nr_balance_failed;
    u64 max_newidle_lb_cost;
    /* ... */
    unsigned long span[];
};
```

`min_interval`、`max_interval`、`balance_interval` 的单位是毫秒，`last_balance` 用 jiffies。`get_sd_balance_interval()` 会结合忙闲状态计算实际间隔，并转换成 jiffies。`max_newidle_lb_cost` 记录新空闲均衡的时间开销，供后面判断是否值得继续搜索。

`sched_group` 和 `sched_group_capacity` 则定义在 `kernel/sched/sched.h`。容量相关字段的相关字段如下：

```c
struct sched_group_capacity {
    atomic_t ref;
    unsigned long capacity;
    unsigned long min_capacity;
    unsigned long max_capacity;
    unsigned long next_update;
    int imbalance;
    /* ... */
    unsigned long cpumask[];
};

struct sched_group {
    struct sched_group *next;
    atomic_t ref;
    unsigned int group_weight;
    unsigned int cores;
    struct sched_group_capacity *sgc;
    int asym_prefer_cpu;
    int flags;
    unsigned long cpumask[];
};
```

`sgc` 指向容量统计，`capacity` 表示该组的 CPU 容量。比较负载时需要结合容量，两个 SMT 线程的有效容量取决于具体处理器拓扑和调度域配置。调度域标志的定义可在 `include/linux/sched/sd_flags.h` 查到。

### 周期均衡

`sched_init_smp()` 注册 `SCHED_SOFTIRQ` 的处理函数 `run_rebalance_domains()`。`scheduler_tick()` 末尾调用 `trigger_load_balance()`，后者到期时触发软中断：

```c
void trigger_load_balance(struct rq *rq)
{

    if (unlikely(on_null_domain(rq) || !cpu_active(cpu_of(rq))))
        return;

    if (time_after_eq(jiffies, rq->next_balance))
        raise_softirq(SCHED_SOFTIRQ);

    nohz_balancer_kick(rq);
}
```

软中断处理函数先尝试 NO_HZ 空闲均衡；如果没有由它处理，更新阻塞任务的衰减统计，再执行本 CPU 的常规周期均衡。下面列出软中断处理的主要步骤：

```c
static __latent_entropy void run_rebalance_domains(struct softirq_action *h)
{
    struct rq *this_rq = this_rq();
    enum cpu_idle_type idle = this_rq->idle_balance ?
                        CPU_IDLE : CPU_NOT_IDLE;

    if (nohz_idle_balance(this_rq, idle))
        return;

    update_blocked_averages(this_rq->cpu);
    rebalance_domains(this_rq, idle);
}
```

`rebalance_domains(struct rq *rq, enum cpu_idle_type idle)` 沿 `for_each_domain(cpu, sd)` 遍历域层级，计算间隔并在到期时调用 `load_balance()`。下面是这个函数中的局部摘录，变量已在外层声明；前后的串行化、成本衰减和下一次到期时间维护请在原函数中看：

```c
if (time_after_eq(jiffies, sd->last_balance + interval)) {
    if (load_balance(cpu, rq, sd, idle, &continue_balancing)) {
        idle = idle_cpu(cpu) ? CPU_IDLE : CPU_NOT_IDLE;
        busy = idle != CPU_IDLE && !sched_idle_cpu(cpu);
    }
    sd->last_balance = jiffies;
    interval = get_sd_balance_interval(sd, busy);
}
```

`load_balance()` 的入口签名是：

```c
static int load_balance(int this_cpu, struct rq *this_rq,
                       struct sched_domain *sd, enum cpu_idle_type idle,
                       int *continue_balancing)
```

它把源 CPU、目标 CPU、域、待迁移量等信息放进 `struct lb_env`，按下面的顺序工作：

1. `should_we_balance(&env)` 判断当前 CPU 是否应负责本次均衡。
2. `find_busiest_group(&env)` 比较组的统计，寻找需要搬出任务的组，并确定不均衡量。
3. `find_busiest_queue(&env, group)` 在这个组里选择源运行队列。
4. `detach_tasks(&env)` 从源队列摘出可迁移任务，`attach_tasks(&env)` 把已摘出的任务放到目标队列。

`env.imbalance` 的含义与迁移类型有关，可能按负载、利用率或任务数量计算，它表示这次均衡需要搬运的负载规模，具体单位由均衡类型决定。`detach_tasks()` 通过 `can_migrate_task()` 等检查过滤不适合迁移的任务，实际迁移数可以小于期望值。

如果普通拉取没有搬到任务，且 `need_active_balance(&env)` 判断需要主动均衡，`load_balance()` 可以在源队列设置 `active_balance` 和 `push_cpu`，再用 `stop_one_cpu_nowait()` 安排 `active_load_balance_cpu_stop()`。stopper 在源 CPU 上运行，使原本占用该 CPU 的任务有机会被迁出；这条路径仍必须满足亲和性等迁移约束。

### 新空闲均衡

公平类准备转向 idle 时，会尝试 `newidle_balance(struct rq *this_rq, struct rq_flags *rf)`。它不仅检查本地公平队列，还会考虑待处理的远端唤醒 `ttwu_pending`，避免已有任务即将入队时再做无用搜索。

函数把预计空闲时间 `avg_idle` 与各域的 `max_newidle_lb_cost` 比较。如果均衡开销可能超过空闲时间，就不值得继续往更远的域寻找。需要搜索时，它会暂时释放当前运行队列锁，更新阻塞负载，再遍历调度域；在带有 `SD_BALANCE_NEWIDLE` 的域中，以 `CPU_NEWLY_IDLE` 调用 `load_balance()`。

下面是原函数在重新取得锁之后的一段局部摘录：

```c
if (this_rq->cfs.h_nr_running && !pulled_task)
    pulled_task = 1;

if (this_rq->nr_running != this_rq->cfs.h_nr_running)
    pulled_task = -1;
```

放锁期间可能有任务自行入队，所以返回值不仅表示“这次迁移了多少任务”。对调用方有用的是正负号：

| 返回值 | 调用方应怎样理解 |
| --- | --- |
| 大于 0 | 现在有公平任务，可以重新走公平类选人 |
| 0 | 没找到新的可运行任务 |
| 小于 0 | 出现更高调度类的任务，需要重新按调度类选择 |

`pick_next_task_fair()` 将负值转换为 `RETRY_TASK`，这就是第 5 章不能省去重试分支的原因。其他调用路径也会用这个结果判断是否需要重新选人。

### 为唤醒、新建和 exec 的任务选择 CPU

任务还未进入新的运行队列时，也有一次放置机会。框架使用 `select_task_rq` 钩子，公平类实现是 `select_task_rq_fair()`：

| 场景 | 6.6 框架传入的标志 | 含义 |
| --- | --- | --- |
| `try_to_wake_up()` | `wake_flags | WF_TTWU` | 已存在任务被唤醒 |
| `wake_up_new_task()` | `WF_FORK` | 新任务第一次入队 |
| `sched_exec()` | `WF_EXEC` | 当前任务 exec 后的放置机会 |

`exec()` 替换当前进程的程序映像，不是创建一个新任务。`WF_*` 表示这次选 CPU 的原因，`SD_BALANCE_WAKE`、`SD_BALANCE_FORK`、`SD_BALANCE_EXEC` 则是调度域相关标志；这些标志由框架传入并在公平类内部解释。

`select_task_rq_fair()` 还会结合唤醒亲和性、空闲 CPU、容量和能耗等信息选择目标。能量感知调度有单独的 `find_energy_efficient_cpu()` 路径，并依赖平台能量模型等条件；节能放置由容量、能量模型和平台配置共同决定。


<a id="11-实时负载均衡"></a>

# 11. 实时负载均衡

公平类的负载均衡只搬运公平任务。RT 类在 `kernel/sched/rt.c` 中维护自己的放置、推送和拉取逻辑，目的是减少“高优先级 RT 任务在一个 CPU 上排队，别的合适 CPU 却运行更低优先级工作”的情况。

假设两个 CPU 上分别运行内部优先级 10 和 50 的 RT 任务，而内部优先级 20 的任务排在 CPU 0 上等待。如果它允许在 CPU 1 上运行，迁移过去就能优先于 50 的任务执行。这里数值越小优先级越高。

CPU 亲和性、根域划分、禁止迁移、带宽节流和更高调度类都会限制这种安排。因此“全系统最高的 N 个 RT 任务总在 N 个 CPU 上运行”只能作为忽略这些约束的目标理解，实际结果还受这些约束影响。

## 11.1. 根域（root domain）和 CPU 优先级（CPU priority）

`root_domain` 汇总一组 CPU 的调度状态，定义在 `kernel/sched/sched.h`。系统可以因调度域分区等配置形成多个根域，RT 跨 CPU 均衡在所在根域内寻找机会。与本章有关的相关字段如下：

```c
struct root_domain {
    atomic_t refcount;
    atomic_t rto_count;
    struct rcu_head rcu;
    cpumask_var_t span;
    cpumask_var_t online;
    /* ... 公平类、deadline 类及其他状态 ... */
    cpumask_var_t rto_mask;
    struct cpupri cpupri;
    /* ... */
};
```

`rto_mask` 标记 RT 过载的 CPU，`rto_count` 记录数量。`update_rt_migration()` 在 `rt_nr_total > 1` 且 `rt_nr_migratory` 非零时设置过载状态，因此不仅要有多个 RT 任务，还要有允许在多个 CPU 上运行的任务。`pushable_tasks` 维护其中没有正在运行、可作为推送候选的任务；临时禁止迁移仍需另外检查。

`cpupri` 按优先级等级保存 CPU 掩码，帮助快速寻找可以接收 RT 任务的 CPU。6.6 的定义在 `kernel/sched/cpupri.h`：

```c
struct cpupri_vec {
    atomic_t count;
    cpumask_var_t mask;
};

struct cpupri {
    struct cpupri_vec pri_to_cpu[CPUPRI_NR_PRIORITIES];
    int *cpu_to_pri;
};
```

`count` 是该等级的 CPU 数，`mask` 标出这些 CPU，`cpu_to_pri` 保存反向映射。这里的等级由 `convert_prio()` 转换，需要经过 `convert_prio()` 才能与 `task_struct->prio` 对应；源码使用 `count` 和 CPU 掩码记录各优先级等级的状态。

在 `kernel/sched/cpupri.c` 中，`cpupri_find()` 是下面这个包装函数：

```c
int cpupri_find(struct cpupri *cp, struct task_struct *p,
        struct cpumask *lowest_mask)
{
    return cpupri_find_fitness(cp, p, lowest_mask, NULL);
}
```

`cpupri_find_fitness()` 从较低 CPU 优先级等级开始搜索；`__cpupri_find()` 检查等级是否非空，并把对应 CPU 掩码与任务的 `cpus_mask`、活跃 CPU 掩码相交。传入 `fitness_fn` 时，还会尝试满足额外适配条件。返回的是当时的候选集合，真正迁移前必须重新加锁检查。

`rt.c` 的 `find_lowest_rq()` 根据候选集合选最终 CPU。普通同容量系统用 `cpupri_find()`；容量不对称系统用 `cpupri_find_fitness(..., rt_task_fits_capacity)`。若原 CPU 仍在候选集合里，优先保留它；否则结合调度域距离选择，再退到集合中的其他 CPU。找不到目标就返回 -1。

## 11.2. 推送操作

推送把本队列里尚未运行的 RT 任务搬到更合适的 CPU。6.6 有两条常见触发路径：

- `task_woken_rt()` 在唤醒后判断是否需要立即调用 `push_rt_tasks()`。
- `set_next_task_rt()` 等位置通过 `rt_queue_push_tasks()` 登记平衡回调，由调度框架在后续合适的位置执行。

调度框架在合适的时机登记这个回调；`balance_rt()` 则负责第 11.3 节的拉取：

```c
static inline void rt_queue_push_tasks(struct rq *rq)
{
    if (!has_pushable_tasks(rq))
        return;

    queue_balance_callback(rq, &per_cpu(rt_push_head, rq->cpu), push_rt_tasks);
}
```

`push_rt_tasks()` 每次成功搬走一个任务后继续尝试：

```c
static void push_rt_tasks(struct rq *rq)
{

    while (push_rt_task(rq, false))
        ;
}
```

单次操作的真实签名是 `push_rt_task(struct rq *rq, bool pull)`。第二个参数表示这次推送是否由拉取请求触发。它先检查 `rq->rt.overloaded`，再用 `pick_next_pushable_task()` 取优先级最高的推送候选。

如果候选任务反而比本 CPU 当前任务优先级高，就先 `resched_curr(rq)` 请求本地重新调度。通常的可迁移路径使用 `find_lock_lowest_rq()` 寻找并锁住目标队列，期间可能释放源锁，所以还要重查任务和队列状态。确定可以迁移后的局部摘录如下：

```c
deactivate_task(rq, next_task, 0);
set_task_cpu(next_task, lowest_rq->cpu);
activate_task(lowest_rq, next_task, 0);
resched_curr(lowest_rq);
ret = 1;
```

这里依次从源队列移除、修改任务所属 CPU、加入目标队列，再请求目标 CPU 重新调度。`resched_curr()` 的参数是运行队列 `struct rq *`，不是 `rq->curr`。源代码还处理禁止迁移、重试、任务引用和解锁，完整迁移还包括引用计数、锁和并发状态检查。

## 11.3. 拉取操作

拉取从其他 CPU 上寻找比本地候选更高优先级的 RT 任务。它不要求本 CPU 的 RT 队列已经完全为空：本地只剩较低优先级任务时，也可能值得拉取。`balance_rt()` 是调度类的 `balance` 钩子：

```c
static int balance_rt(struct rq *rq, struct task_struct *p, struct rq_flags *rf)
{
    if (!on_rt_rq(&p->rt) && need_pull_rt_task(rq, p)) {

        rq_unpin_lock(rq, rf);
        pull_rt_task(rq);
        rq_repin_lock(rq, rf);
    }

    return sched_stop_runnable(rq) || sched_dl_runnable(rq) || sched_rt_runnable(rq);
}
```

其中 `p` 是即将被换下的任务。`need_pull_rt_task(rq, p)` 比较本地剩余的最高 RT 优先级与 `p->prio`；配合 `!on_rt_rq(&p->rt)`，在离开的任务不再位于 RT 队列、且本地 RT 优先级下降时尝试拉取。

`pull_rt_task()` 在 6.6 中返回 `void`。它先查看所在根域是否有 RT 过载 CPU。若平台支持且启用了 `RT_PUSH_IPI`，可以转成通知源 CPU 推送；否则沿 `rto_mask` 搜索源 CPU，使用 `pick_highest_pushable_task(src_rq, this_cpu)` 找出允许在本 CPU 运行的最高优先级候选。

在比较本地和源 CPU 当前任务的优先级、处理迁移限制后，成功拉取路径执行以下局部代码：

```c
deactivate_task(src_rq, p, 0);
set_task_cpu(p, this_cpu);
activate_task(this_rq, p, 0);
resched = true;
```

搜索可能继续，以寻找更合适的候选。函数末尾在 `resched` 为真时调用 `resched_curr(this_rq)`。这里挑选的是符合迁移条件的 pushable 任务。

## 11.4. 为唤醒的任务挑选一个运行队列

RT 类的 `select_task_rq_rt()` 在任务入队前选择 CPU。它接收框架传入的 `cpu` 和 `flags`，对 `WF_TTWU` 或 `WF_FORK` 场景做放置判断。下面列出该函数的主要判断：

```c
static int
select_task_rq_rt(struct task_struct *p, int cpu, int flags)
{
    struct task_struct *curr;
    struct rq *rq;
    bool test;

    if (!(flags & (WF_TTWU | WF_FORK)))
        goto out;

    rq = cpu_rq(cpu);

    rcu_read_lock();
    curr = READ_ONCE(rq->curr);

    test = curr &&
           unlikely(rt_task(curr)) &&
           (curr->nr_cpus_allowed < 2 || curr->prio <= p->prio);

    if (test || !rt_task_fits_capacity(p, cpu)) {
        int target = find_lowest_rq(p);

        if (!test && target != -1 && !rt_task_fits_capacity(p, target))
            goto out_unlock;

        if (target != -1 &&
            p->prio < cpu_rq(target)->rt.highest_prio.curr)
            cpu = target;
    }

out_unlock:
    rcu_read_unlock();

out:
    return cpu;
}
```

如果原目标 CPU 正在运行不易迁移的 RT 任务，或该任务的优先级不低于新来的任务，就尝试另找目标；CPU 容量不适配时也会触发搜索。找到 CPU 后还要确认新任务的优先级高于目标队列当前最高 RT 优先级，才更新 `cpu`。

没有满足条件的更好目标时，函数返回原来的 CPU。任务入队后，抢占检查和前面介绍的 push/pull 机制还会继续调整。因此这里的 CPU 选择是一次放置决策，不是对以后运行位置的永久承诺。

<a id="12-参考资料"></a>

# 12. 参考资料

- Robert Love, Linux Kernel Development, 3rd edition, 2010
- Daniel P. Bovet, Marco Cesati, Understanding the Linux Kernel, 3rd edition, Nov 2005
- Tim Jones, Inside the Linux 2.6 Completely Fair Scheduler, 15.12.2009 - <http://www.ibm.com/developerworks/library/l-completely-fair-scheduler/>
- Chandandeep Singh Pabla, Completely Fair Scheduler, 01.08.2009 - <http://www.linuxjournal.com/magazine/completely-fair-scheduler>
- 本目录 `linux-6.6/Documentation/scheduler/`
- CFS 设计背景（文档中的CFS 描述不等同于 6.6 的完整 EEVDF 实现）- <https://www.kernel.org/doc/html/v6.6/scheduler/sched-design-CFS.html>
- 本地 EEVDF 实现与默认特性：`linux-6.6/kernel/sched/fair.c`、`linux-6.6/kernel/sched/features.h`
- EEVDF 合入说明，LWN - <https://lwn.net/Articles/925371/>
- Josh Aas, Understanding the Linux 2.6.8.1 CPU Scheduler, 17.02.2005 - <http://joshaas.net/linux/linux_cpu_scheduler.pdf>
- Jonathan Corbet, Schedulers: the plot thickens, 17.04.2007 - <http://lwn.net/Articles/230574/>
- Jonathan Corbet, SCHED_FIFO and realtime throttling, 01.09.2008 - <http://lwn.net/Articles/296419/>
- Jonathan Corbet, Scheduling domains, 19.04.2004 - <http://lwn.net/Articles/80911/>
- Ankita Garg, Real-Time Linux Kernel Scheduler, 01.08.2009 - <http://www.linuxjournal.com/magazine/real-time-linux-kernel-scheduler?page=0,4>
