update_load_avg在fair.c中，第一次被调用

```
static inline void update_load_avg(struct cfs_rq *cfs_rq, struct sched_entity *se, int not_used1)
{
	cfs_rq_util_change(cfs_rq, 0);
}
```

在cfs_rq_util_change内部，又会进入这个函数

```
cpufreq_update_util(rq, flags);
```

进入函数发现：

```c
static inline void cpufreq_update_util(struct rq *rq, unsigned int flags)
{
	struct update_util_data *data;

	data = rcu_dereference_sched(*per_cpu_ptr(&cpufreq_update_util_data,
						  cpu_of(rq)));
	if (data)
		data->func(data, rq_clock(rq), flags);
}
```

追踪下来是这个函数

```
struct update_util_data {
       void (*func)(struct update_util_data *data, u64 time, unsigned int flags);
};
```

线索到这里就断了，需要去代码里面看，它把函数指针指向何处。

在ai的帮助下。摸索到，最后是去绑定的schedutil里面的计算nextfreq的程序。

```
cpufreq core 启动 schedutil
  ↓
sugov_start(policy)
  ↓
for_each_cpu(cpu, policy->cpus)
  ↓
cpufreq_add_update_util_hook(cpu, &sg_cpu->update_util, func)
```

所以这条路线不太对，这是在知道util变化的情况下，去更新下次频率的函数路线。





### 什么情况下会触发util的计算

调度器主导。如果出现任务切换，调度的路径上，会主动调用__update_load_avg来计算util。这里需要注意的是，并不是我们通常会以为的周期性触发。

切换任务的时候，很多时候时间间隔是不固定的，所以更新util还会去考虑时间间隔。

这个是ai梳理的函数调用关系。

```
__update_load_avg_se()
  └─ ___update_load_sum()
       └─ accumulate_sum()
            ├─ 旧 util_sum 衰减
            └─ 根据 running 状态加入新 util_sum

  └─ ___update_load_avg()
       └─ util_avg = util_sum / divider
```

对于CFS的函数调用则是下面：

```
__update_load_avg_cfs_rq()
  └─ ___update_load_sum(..., cfs_rq->curr != NULL)
  └─ ___update_load_avg(&cfs_rq->avg, 1)
       ↓
cfs_rq->avg.util_avg 更新
```



这个里就可以看到三个主要入口函数：

```
int __update_load_avg_blocked_se(u64 now, struct sched_entity *se)
int __update_load_avg_se(u64 now, struct cfs_rq *cfs_rq, struct sched_entity *se)
int __update_load_avg_cfs_rq(u64 now, struct cfs_rq *cfs_rq)
```

