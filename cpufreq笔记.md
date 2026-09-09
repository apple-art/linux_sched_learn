`cpufreq_register_driver()` 的核心作用是：**把具体平台的调频能力交给 cpufreq core，并立刻为当前在线 CPU 初始化 `cpufreq_policy`，最终启动默认 governor。**

你可以把它理解为 driver 接入 cpufreq 框架的总入口。
这里注册的不是 `schedutil`，而是硬件侧的 driver，例如平台提供的 `->init()`、`->target_index()`、`->fast_switch()` 这些回调。

# cpufreq_register_driver

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

所以我们前面看到的：

```c
sugov_init()
sugov_start()
```

并不是 `schedutil` 自己凭空启动的。它的上游链路正是：

```text
cpufreq_register_driver()
  → cpufreq_add_dev()
  → cpufreq_online()
  → cpufreq_init_policy()
  → cpufreq_set_policy()
  → cpufreq_init_governor()
  → schedutil->init()，即 sugov_init()
  → cpufreq_start_governor()
  → schedutil->start()，即 sugov_start()
```

------

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