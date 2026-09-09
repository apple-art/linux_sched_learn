### sugov_update_single_perf

开头借助hook，获取sg_cpu.



获取cpu的最大能力。capacity。

```c
max_cap = arch_scale_cpu_capacity(sg_cpu->cpu);
```



下面则是更新、获取公共的参数。

```c
sugov_update_single_common(sg_cpu, time, max_cap, flags)
```

具体来说有

```c
	sugov_iowait_boost(sg_cpu, time, flags);
	sg_cpu->last_update = time;// 获得上次更新的时间

	ignore_dl_rate_limit(sg_cpu);// 如果是DL任务，忽略更新间隔的limit

	if (!sugov_should_update_freq(sg_cpu->sg_policy, time))// 检测更新间隔是否满足需求。
		return false;

	sugov_get_util(sg_cpu);// 读取能力。
	sugov_iowait_apply(sg_cpu, time, max_cap);
```





接下来这段代码，是检测的一种场景。某个进程某个片段变轻，后面可能里面又会有新的重任务，这里就不要立马把频率降低，否则性能来回摆动会有卡顿。

```c
	if (!uclamp_rq_is_capped(cpu_rq(sg_cpu->cpu)) &&
	    sugov_cpu_is_busy(sg_cpu) && sg_cpu->util < prev_util)
		sg_cpu->util = prev_util;
```



这个是调用驱动了，后调用哪个驱动，由驱动层决定。

```c
	cpufreq_driver_adjust_perf(sg_cpu->cpu, map_util_perf(sg_cpu->bw_dl),
				   map_util_perf(sg_cpu->util), max_cap);
```





### sugov_update_shared



先加个锁，避免多个cpu同时修改一个policy。

```
raw_spin_lock(&sg_policy->update_lock);
```



更新触发本次调用的 CPU 的 I/O boost、时间和 deadline 状态。

```
sugov_iowait_boost(sg_cpu, time, flags);
sg_cpu->last_update = time;

ignore_dl_rate_limit(sg_cpu);
```



检测是否能更新CPU

```
sugov_should_update_freq(sg_policy, time)
```



这个是重点，会依次遍历同簇下的每个CPU，然后选择其中负载最重的util。

```
sugov_next_freq_shared
```



之后就执行驱动层的更新了。fast_switch现在都会支持的。非常老的设备不支持

```
cpufreq_driver_fast_switch(sg_policy->policy, next_f);
```



# IO类任务



## 产生/积累boost：sugov_iowait_boost

这个函数可以把它理解成一个很小的“**I/O 唤醒 boost 状态机**”。

它不直接算频率；它只负责更新 `sg_cpu->iowait_boost` 这个值。之后 `sugov_iowait_apply()` 会把这个值转换成参与调频决策的 `util`。

先记住两个状态：

- `iowait_boost`：当前累计到多大的 boost，单位是 capacity，不是 MHz。
- `iowait_boost_pending`：这轮 I/O boost 已经记过一次，不能在同一轮里反复翻倍。

### 1. 先判断：这次更新是不是由 I/O 唤醒引起

```c
bool set_iowait_boost = flags & SCHED_CPUFREQ_IOWAIT;
```

如果任务是“等待 I/O 完成后被唤醒”，调度器会带上 `SCHED_CPUFREQ_IOWAIT`。

所以这里的含义是：

```text
set_iowait_boost = true
```

代表：“这次该考虑给 CPU 加一个 I/O wait boost。”

注意，它不是说“这个任务正在做 I/O”，而是说“它刚结束 I/O wait、重新需要 CPU”。

### 2. 检测是否已经过期需要重置：sugov_iowait_reset

```c
s64 delta_ns = time - sg_cpu->last_update;
```

计算距离上一次调频请求过去了多久，单位是纳秒。

```c
if (delta_ns <= TICK_NSEC)
    return false;
```

若时间还没超过一个 tick，认为之前的 I/O burst 还在持续，**不 reset**，返回 `false`。外层函数会继续考虑把已有 boost 翻倍。

```c
sg_cpu->iowait_boost =
    set_iowait_boost ? IOWAIT_BOOST_MIN : 0;
```

若已经超过一个 tick，旧的 I/O boost 就不可信了，要重新开始：

- 这次仍是 I/O 唤醒：从最小 boost `IOWAIT_BOOST_MIN` 重新开始；
- 这次不是 I/O 唤醒：直接清零。

这里表达的是：**间隔较久后的 I/O 唤醒，被当成一次新的 I/O burst，而非上一次 burst 的延续。**

```c
sg_cpu->iowait_boost_pending = set_iowait_boost;
```

如果这次是新的 I/O 唤醒，标记为 `pending = true`，表示“这个最小 boost 已经为本轮 I/O 请求准备好了”；若不是 I/O 唤醒，则设为 `false`。

```c
return true;
```

表示 reset 已经完成。调用者看到 `true` 后就直接返回，避免同一次调用中又把刚重置成最小值的 boost 翻倍。

这个函数的核心只有一句：

> 超过一个 tick 没有新的调频请求，就切断旧 I/O burst；之后的新 I/O 唤醒从最小 boost 重新起步。

### 3. 不是 I/O 唤醒，就不用继续

```c
if (!set_iowait_boost)
    return;
```

到这里说明旧 boost 没有触发 reset。

若本次不是 I/O wakeup，这个函数不新建、不增加 boost；之后的 `sugov_iowait_apply()` 会负责让旧 boost 逐步衰减。

也就是说：

```text
I/O 唤醒：增加/维持 boost
普通调度更新：不增加，让它以后自然退场
```

### 4. 同一轮只允许翻倍一次

```c
if (sg_cpu->iowait_boost_pending)
    return;
sg_cpu->iowait_boost_pending = true;
```

这是为了避免同一段尚未完成频率处理的 I/O 活动，反复触发 update-util 回调时，把 boost 从 25% 迅速错误翻到 100%。

`pending` 的语义可以理解为：

> “已经有一个 I/O boost 请求等待被这轮调频逻辑消费。”

所以，同一轮里再来 I/O wakeup，也不能再次 `<< 1`。

之后 `sugov_iowait_apply()` 会看到这个标记，并在合适的时机清掉它；下一轮新的 I/O 请求才允许继续翻倍。

------

### 5. 已经有 boost：连续 I/O 唤醒就翻倍

```c
if (sg_cpu->iowait_boost) {
    sg_cpu->iowait_boost =
        min_t(unsigned int, sg_cpu->iowait_boost << 1,
              SCHED_CAPACITY_SCALE);
    return;
}
```

这是它最核心的策略。

假设初始值 `IOWAIT_BOOST_MIN` 是容量的 1/8，那么连续的 I/O 唤醒大致会是：

```text
第一次 I/O 唤醒：1/8
下一轮仍有 I/O 唤醒：1/4
再下一轮：1/2
再下一轮：1/1（封顶）
```

`min_t(..., SCHED_CAPACITY_SCALE)` 保证不会超过 CPU 的满容量，即不会无限左移。

这个渐进式加速是在猜测：

> 连续发生的 I/O 唤醒，往往意味着任务正处于持续的 I/O→CPU→I/O 链路中，CPU 应更积极地提频。

------

### 6. 没有旧 boost：这是新 burst 的第一笔 I/O 唤醒

```c
sg_cpu->iowait_boost = IOWAIT_BOOST_MIN;
```

第一次不会直接满 boost，而是从一个较小值开始。

原因是单次 I/O 唤醒未必真的紧急：可能只是后台任务偶尔读一次数据。只有后续继续出现 I/O 唤醒，才逐步证明这是持续的延迟敏感链路，于是 boost 才不断翻倍。

------

把整个函数压成一句话就是：

> I/O 唤醒先获得一个小的 CPU 容量补偿；若唤醒连续出现，补偿逐步翻倍；若 I/O 活动中断足够久，则旧补偿失效，而一次久别后的 I/O 唤醒会直接得到强响应。

你接下来应该立即读 `sugov_iowait_apply()`：它会回答最关键的后半句——这里记录的 `iowait_boost` 最后怎样压过普通 `util`，真正影响目标频率。



## 使用boost：sugov_iowait_apply

前面依然是一堆的状态机判断，说真的非常绕，AI解读看懂了，回过头来又忘了。可能比较关键的是后面的一段吧，boost起作用的地方。

```
boost = (sg_cpu->iowait_boost * max_cap) >> SCHED_CAPACITY_SHIFT;
```

`iowait_boost` 原先基于统一的容量刻度 `SCHED_CAPACITY_SCALE`；这里把它换算到当前 CPU 的 `max_cap` 刻度，得到能和 `sg_cpu->util` 直接比较的值。

```
boost = uclamp_rq_util_with(cpu_rq(sg_cpu->cpu), boost, NULL);
```

再让这个人为提高的 util 也遵守该 CPU runqueue 的 uclamp 限制。

```
if (sg_cpu->util < boost)
    sg_cpu->util = boost;
```

关键落点在这里：

> I/O boost 不会和正常 util 相加，而是取两者中较大的那个。

所以它不会在 CPU 已经很忙时继续虚增 util；它只在正常 util 因 I/O 睡眠而偏低时，把 util 托高，从而让后续频率计算更积极。