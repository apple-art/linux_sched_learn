# cpufreq_register_driver

`cpufreq_register_driver()` 的核心作用是：**把具体平台的调频能力交给 cpufreq core，并立刻为当前在线 CPU 初始化 `cpufreq_policy`，最终启动默认 governor。**

## 先看完整主线

```text
driver 调用 cpufreq_register_driver(driver_data)
  │
  ├─ 1. 检查 driver 回调是否符合 cpufreq 框架要求
  ├─ 2. 把全局 cpufreq_driver 指向这个 driver
  ├─ 3. 注册 cpufreq_interface 到 CPU 子系统
  │      │
  │      └─ 对当前每个已存在的 CPU device 调用 cpufreq_add_dev()
  │             │
  │             └─ cpufreq_online(cpu)
  │                    └─ driver->init(policy)
  │                    └─ 创建 policy、sysfs、QoS 等
  │                    └─ cpufreq_init_policy()
  │                           └─ 选择并启动默认 governor
  │                                  └─ schedutil 的 init/start
  │
  └─ 4. 注册 CPU hotplug 回调
         └─ 后续 CPU online/offline 时，继续创建或销毁 cpufreq policy
```

## 1. 前置检查：这个 driver 能不能接入

```c
if (cpufreq_disabled())
	return -ENODEV;
```

如果内核启动参数或平台已经关闭 cpufreq，直接失败。

```c
if (!get_cpu_device(0))
	return -EPROBE_DEFER;
```

> 这里的device是指的内核设备模型中，CPU本身的对象。cpufreq driver是管理这个CPU对象频率的方法。这一步预防的是初始化顺序不对。

cpufreq core 后面依赖每个 CPU 的 `struct device`。如果连 CPU0 的 device 都尚未创建，说明初始化时机太早，让 driver 稍后重新 probe。

接下来这段最重要：

```c
if (!driver_data || !driver_data->verify || !driver_data->init ||
    !(driver_data->setpolicy || driver_data->target_index ||
		    driver_data->target) ||
     (driver_data->setpolicy && (driver_data->target_index ||
		    driver_data->target)) ||
     (!driver_data->get_intermediate != !driver_data->target_intermediate) ||
     (!driver_data->online != !driver_data->offline) ||
	 (driver_data->adjust_perf && !driver_data->fast_switch))
	return -EINVAL;
```

Driver对象是在具体的驱动里面被初始化的，这段代码是在检查 driver 的“接口契约”。

| 要求                                                     | 含义                                                         |
| -------------------------------------------------------- | ------------------------------------------------------------ |
| 必须有 `->init()`                                        | driver 必须会初始化一个 policy，告诉 core 频点表、受影响 CPU、频率范围等信息。 |
| 必须有 `->verify()`                                      | core 修改 min/max 限制前，需要 driver 校验该范围是否合理。   |
| 必须有一种调频方式                                       | `->setpolicy()` 或 `->target()` / `->target_index()` 至少一个。 |
| `setpolicy` 不能和 `target*` 共存                        | 两条调频模型只能二选一。                                     |
| `get_intermediate` 与 `target_intermediate` 必须成对出现 | 支持“中间频点切换”时，读取与设置接口要配套。                 |
| `online` 与 `offline` 必须成对出现                       | 支持 policy 的轻量下线/恢复时，回调必须完整。                |
| 有 `adjust_perf` 就必须有 `fast_switch`                  | 性能百分比调整依赖快速切频能力。                             |

对于 `schedutil`，最关键的是这一句：

```c
!(driver_data->setpolicy || driver_data->target_index ||
  driver_data->target)
```

因为 `schedutil` 不是直接操作硬件，它最终必须借助 driver 的：

```text
target_index / target
或 fast_switch
```

去完成真正的调频。

------

## 2. 保存全局 driver：全系统只能有一个

```c
cpus_read_lock();

write_lock_irqsave(&cpufreq_driver_lock, flags);
if (cpufreq_driver) {
    ...
    ret = -EEXIST;
    goto out;
}
cpufreq_driver = driver_data;
write_unlock_irqrestore(&cpufreq_driver_lock, flags);
```

`cpufreq_driver` 是全局指针，表示“当前系统实际使用的 cpufreq driver”。

所以同一时刻只允许一个 driver 注册成功。第二个再来注册就返回 `-EEXIST`。

这里的两层保护你先这样理解即可：

- `cpus_read_lock()`：注册期间不允许 CPU hotplug 并发改变 CPU online/offline 状态。
- `cpufreq_driver_lock`：保护全局 `cpufreq_driver` 指针及 policy 列表，防止并发读写混乱。

------

## 3. 让 scheduler 在负载判断中考虑当前频率

```
if (!cpufreq_driver->setpolicy) {
    static_branch_enable_cpuslocked(&cpufreq_freq_invariance);
    pr_debug("supports frequency invariance");
}
```

前面的合法性检查已经保证：如果 driver 没有 `setpolicy()`，那么它必然提供了 `target()` 或 `target_index()`，属于 governor 参与决策的常规调频模式。

此时 cpufreq core 打开 `cpufreq_freq_invariance` 这个静态开关，使 scheduler 在计算 CPU 的当前能力和任务负载时，把“当前频率相对最高频率的比例”考虑进去。

------

## 4. 注册 interface：真正触发当前 CPU 的 policy 初始化

```c
ret = subsys_interface_register(&cpufreq_interface);
```

`cpufreq_interface` 在同文件中定义：

```c
static struct subsys_interface cpufreq_interface = {
    .name       = "cpufreq",
    .subsys     = &cpu_subsys,
    .add_dev    = cpufreq_add_dev,
    .remove_dev = cpufreq_remove_dev,
};
```

这句话的直觉理解：

> 把 cpufreq 挂接到 CPU 子系统上。
> CPU 子系统中每发现一个 CPU device，就让 cpufreq core 对它执行 `cpufreq_add_dev()`。

尤其要注意：**注册 interface 时，系统当前已经存在的 CPU device 也会被依次处理。**

> 此处可以先不深究，记住是把 cpufreq 接到 CPU 设备和 CPU 上下线机制中。

对，这样读更顺。`cpufreq_register_governor()` 很短，就按它的 3 个区域看。

# cpufreq_register_governor

检查一个 governor 是否为空、cpufreq 是否启用、名称是否重复；如果不重复，就把它挂入 cpufreq core 的全局 governor 链表。

## 1. 注册前检查：系统是否允许 governor 存在

```c
if (!governor)
	return -EINVAL;

if (cpufreq_disabled())
	return -ENODEV;
```

这里处理两个异常场景：

- 没传入 governor 对象，参数无效；
- cpufreq 子系统整体被关闭，注册 governor 没有意义。

这一步解决的是：当前这个 governor 有没有资格进入 cpufreq 系统。

------

## 2. 加入全局名单：让 core 能找到它

```c
mutex_lock(&cpufreq_governor_mutex);

err = -EBUSY;
if (!find_governor(governor->name)) {
	err = 0;
	list_add(&governor->governor_list,
		 &cpufreq_governor_list);
}

mutex_unlock(&cpufreq_governor_mutex);
```

cpufreq core 维护了一张全局 governor 名单：

```text
performance
powersave
ondemand
schedutil
...
```

比如注册 schedutil 时，core 做两件事：

1. 按名字检查是否已经有 `"schedutil"`；
2. 如果没有，就把它加入全局链表。

如果已经存在同名 governor，就返回 `-EBUSY`，避免系统里出现两个同名算法。

这里加锁是为了保护这张全局名单，防止注册、注销、查找同时发生。

### 名单机制

cpufreq.c有全局变量cpufreq_governor_list，它可以理解为一个双向的链表。

每当有新的governor注册的时候，就会追加。cpufreq_governor_list是整个链表的头，向后遍历就可以找到所有已有的策略。

```
链表头 → schedutil → performance
   ↑                         ↓
   └─────────────────────────┘
```

具体的调频策略结构体里面，有个字段

```c
struct cpufreq_governor {
	...
	struct list_head	governor_list;
	...
};
```

这个governor_list字段与全局的链表头是同一个类型，可以实现双向指向，最终实现挂载。

------

## 3. 注册完成后的状态：可被选择，但还没有运行

注册成功后，系统只是知道：

```text
“schedutil 是一个可用的 governor”
```

但此时还没有：

```text
policy->governor = &schedutil_gov
sugov_init(policy)
sugov_start(policy)
cpufreq_add_update_util_hook(...)
```

也就是说：

- 没有为某个 policy 创建 schedutil 状态；
- 没有注册 update-util 回调；
- 没有开始根据负载决定频率。

后续当某个 policy 选择 schedutil 时，才会沿着：

```text
选择 schedutil
    ↓
cpufreq_init_governor()
    ↓
sugov_init()
    ↓
cpufreq_start_governor()
    ↓
sugov_start()
```

进入真正的运行阶段。

因此，`cpufreq_register_governor()` 的核心概念是：

> 把一个 governor 算法登记为“系统可选项”，而不是启动这个算法。

schedutil 文件中的：

```c
cpufreq_governor_init(schedutil_gov);
```

就是在模块加载或内核初始化阶段，触发这次登记。

# policy 如何建立并启动 governor

## 1. `cpufreq_online()`：为 CPU 建立或恢复 policy

CPU 上线时进入这个函数。

它主要做三件事：

```c
policy = per_cpu(cpufreq_cpu_data, cpu);
```

先检查这个 CPU 是否已经属于某个 policy：

- 没有 policy：分配新的 `policy`
- 已有 policy：恢复或加入原来的 policy

然后调用 driver：

```c
ret = cpufreq_driver->init(policy);
```

由具体 driver 填充：

```c
policy->cpus
policy->related_cpus
policy->min / max
policy->freq_table
policy->cpuinfo
```

之后创建 sysfs、QoS 限制、频率表等基础设施。

最后进入：

```c
ret = cpufreq_init_policy(policy);
```

也就是：硬件信息准备好后，开始初始化默认的调频策略。

------

## 2. `cpufreq_init_policy()`：选择初始 governor

这个函数决定当前 policy 使用哪个 governor。

优先级大致是：

```text
上次使用的 governor
    ↓
系统默认 governor
    ↓
cpufreq core 的兜底 governor
```

例如：

```c
gov = get_governor(policy->last_governor);
```

如果上次没有记录，就寻找默认 governor。

对于普通 governor 模式，最后调用：

```c
cpufreq_set_policy(policy, gov, pol);
```

所以它本身主要负责：

> 选择 governor，不负责真正启动 governor。

------

## 3. `cpufreq_set_policy()`：应用新的 policy

这个函数是真正“设置策略”的地方。

首先收集当前限制：

```c
new_data.min = freq_qos_read_value(...);
new_data.max = freq_qos_read_value(...);
```

然后让 driver 检查范围是否合法：

```c
ret = cpufreq_driver->verify(&new_data);
```

接着把用户请求的频率范围，转换成硬件实际支持的频率范围：

```c
policy->min = __resolve_freq(...);
policy->max = __resolve_freq(...);
```

之后分两种情况。

### `setpolicy` 模式

```c
if (cpufreq_driver->setpolicy)
    return cpufreq_driver->setpolicy(policy);
```

由 driver 自己决定策略和调频。

### governor 模式

如果 governor 没有变化，只更新限制：

```c
cpufreq_governor_limits(policy);
```

如果 governor 发生变化：

```text
停止旧 governor
    ↓
退出旧 governor
    ↓
设置新 governor
    ↓
初始化新 governor
    ↓
启动新 governor
```

初次初始化时，`old_gov` 通常为空，因此会直接初始化并启动新 governor。

------

## 4. `cpufreq_init_governor()`：初始化 governor

这个函数调用 governor 的初始化回调：

```c
policy->governor->init(policy);
```

对于 `schedutil`，对应：

```c
sugov_init(policy);
```

`sugov_init()` 主要创建和准备 schedutil 的 policy 私有数据，例如：

```text
sugov_policy
sg_cpu
锁和工作项
频率更新状态
```

注意：

> `init` 是建立 governor 运行所需的数据和资源，还没有正式开始响应负载变化。

------

## 5. `cpufreq_start_governor()`：正式启动 governor

这个函数调用：

```c
policy->governor->start(policy);
```

对于 `schedutil`，对应：

```c
sugov_start(policy);
```

`sugov_start()` 会把 schedutil 注册到调度器的 update-util 回调链上。

从这里开始：

```text
调度器更新 CPU 利用率
    ↓
触发 schedutil 回调
    ↓
schedutil 计算目标频率
    ↓
调用 cpufreq driver 调频
```

启动后还会调用：

```c
policy->governor->limits(policy);
```

让 governor 立即根据当前的 `min/max` 限制修正一次频率。

------

整体主线可以记成：

```text
cpufreq_online()
    ↓
driver->init(policy)
    ↓
cpufreq_init_policy()
    ↓
cpufreq_set_policy()
    ↓
cpufreq_init_governor()
    ↓
governor->init()      // sugov_init
    ↓
cpufreq_start_governor()
    ↓
governor->start()     // sugov_start
```

最核心的理解是：

> `cpufreq_online()` 建立 policy，
> `cpufreq_init_policy()` 选择 governor，
> `cpufreq_set_policy()` 应用 policy，
> `cpufreq_init_governor()` 准备 governor，
> `cpufreq_start_governor()` 让 governor 开始工作。



## cpufreq_online初始化的三种情况：

可以，用一个 **4 核 CPU** 的例子。

假设 CPU0、CPU1 共享一个频率域，也就是它们共用一个 `policy`：

```text
policy A
├── CPU0
└── CPU1
```

### 场景一：已有 active policy

开始时：

```text
CPU0：在线
CPU1：在线
policy A：正在工作
```

此时 CPU1 下线：

```text
CPU0：在线
CPU1：离线
policy A：仍然 active
```

之后 CPU1 又上线，执行：

```c
if (policy && !policy_is_inactive(policy))
    return cpufreq_add_policy_cpu(policy, cpu);
```

意思是：

> policy A 还在工作，CPU1 只是重新加入，不需要重新初始化 driver。

结果：

```text
policy A
├── CPU0
└── CPU1
```

------

### 场景二：已有 inactive policy

如果 CPU0、CPU1 都下线：

```text
CPU0：离线
CPU1：离线
policy A：inactive，但对象还保留
```

之后 CPU0 又上线：

```c
policy = per_cpu(cpufreq_cpu_data, cpu);
```

仍然能找到旧的 `policy A`，但它是 inactive。

于是执行：

```c
cpufreq_driver->online(policy);
```

意思是：

> 这个 policy 以前存在，现在把它恢复工作。

这里通常不需要重新执行完整的：

```c
cpufreq_driver->init(policy);
```

------

### 场景三：完全没有 policy

系统刚启动，CPU0 第一次被 cpufreq 接管：

```text
CPU0：在线
没有任何 policy
```

于是：

```c
policy = cpufreq_policy_alloc(cpu);
```

创建新的 policy，然后：

```c
cpufreq_driver->init(policy);
```

由 driver 告诉 core：

```text
哪些 CPU 共享频率
支持哪些频率
最小频率和最大频率是多少
频率表是什么
```

例如 driver 返回：

```text
policy->cpus        = { CPU0 }
policy->related_cpus = { CPU0, CPU1 }
```

core 就知道：

> CPU0 和 CPU1 属于同一个频率控制范围，后续 CPU1 上线时可以加入这个 policy。

所以三种情况可以记成：

```text
第一次见到 CPU      → 创建 policy，调用 driver->init()
policy 还在工作     → CPU 加入已有 policy
policy 暂时 inactive → 恢复旧 policy，调用 driver->online()
```

# schedutil 的频率请求如何经过 core

## `cpufreq_driver_resolve_freq()` 

它负责把 schedutil 算出的“理想频率”，转换成硬件真正支持的频率。

```c
return __resolve_freq(policy, target_freq, CPUFREQ_RELATION_LE);
```

例如 schedutil 算出：

```text
目标频率：1.37GHz
```

但硬件只支持：

```text
800MHz、1.2GHz、1.5GHz、2GHz
```

这个函数要找到合适的实际频率。

### 先把目标频率限制在 policy 范围内

`__resolve_freq()` 首先会做类似：

```c
target_freq = clamp_val(target_freq,
                        policy->min,
                        policy->max);
```

例如：

```text
schedutil 目标：2GHz
policy 最大频率：1.5GHz
```

会先变成：

```text
目标频率：1.5GHz
```

也就是说：

> schedutil 想要的频率，不能突破 policy 设置的上下限。

### 没有频率表的情况

```c
if (!policy->freq_table)
    return target_freq;
```

如果 driver 没有提供频率表，core 就不再查找具体频点，直接返回限制后的目标频率。

先记住这一层：

```text
原始目标频率
    ↓
限制在 policy->min/max 范围内
    ↓
再决定是否映射到具体频点
```

## 调频的最终入口

### __cpufreq_driver_target：异步调频

这个是异步调频，最终的驱动入口。

```
update_util 回调
    ↓
irq_work_queue()
    ↓
sugov_irq_work()
    ↓
kthread 执行 sugov_work()
    ↓
__cpufreq_driver_target()
    ↓
driver->target()
```

### `cpufreq_driver_fast_switch()`：快速切频

它的作用是：

> governor 已经算出目标频率后，直接调用 driver，立即尝试切频。

典型场景：

```text
CPU 负载突然升高
    ↓
schedutil 算出 2GHz
    ↓
调用 cpufreq_driver_fast_switch()
    ↓
driver->fast_switch(2GHz)
```

它只有在：

```c
policy->fast_switch_enabled
```

时才会使用。

如果硬件和 driver 支持快速切频，就在调度器更新利用率的路径中直接完成；如果不支持，就走后面的异步慢速路径。

所以它和前面函数的区别是：

```text
resolve_freq()
    → 选择一个合法频点

fast_switch()
    → 立即把硬件切到这个频点
```

可以先记住：

> `cpufreq_driver_fast_switch()` 是 schedutil 的快速调频出口。

# 频率限制

频率限制函数，可以按照下面的调用链来认知

```text
限制发生变化
    ↓
cpufreq_update_limits()
    ↓
cpufreq_update_policy()
    ↓
cpufreq_governor_limits()
    ↓
通知 schedutil 重新适应限制
```

假设下面的具体场景。

例如 thermal 发现温度过高，把最高频率从：

```text
2.4GHz → 1.5GHz
```

这时需要重新应用 policy 限制。

### `cpufreq_update_limits()`

它是入口：

```text
driver 有 update_limits()
    → 调用 driver 自己的处理

没有
    → 调用 cpufreq_update_policy()
```

### `cpufreq_update_policy()`

它重新读取 policy 的限制，并调用：

```c
refresh_frequency_limits(policy);
```

最终重新执行一次：

```c
cpufreq_set_policy(policy,
                   policy->governor,
                   policy->policy);
```

注意：这里 governor 没有变化，只是限制变了。

### `cpufreq_governor_limits()`

因为 governor 没变，所以不重新执行 `init/start`，只调用：

```c
policy->governor->limits(policy);
```

对于 schedutil，就是：

```c
sugov_limits(policy);
```

让 schedutil 根据新的最高频率重新调整。

核心理解：

> 不是重新切换 governor，而是把新的 min/max 限制通知给当前 governor。