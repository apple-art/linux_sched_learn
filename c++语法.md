### string方法

| 需求           | 只记这个         | 示例                           |
| -------------- | ---------------- | ------------------------------ |
| 获取长度       | `s.size()`       | `int n = s.size();`            |
| 判断为空       | `s.empty()`      | `if (s.empty())`               |
| 访问字符       | `s[i]`           | `char c = s[i];`               |
| 第一个字符     | `s[0]`           | `char c = s[0];`               |
| 最后一个字符   | `s[s.size()-1]`  | `char c = s[s.size()-1];`      |
| 拼接字符串     | `+`、`+=`        | `s += t;`                      |
| 追加字符       | `+=`             | `s += 'a';`                    |
| 查找           | `find()`         | `int pos = s.find("abc");`     |
| 从指定位置查找 | `find(x, start)` | `s.find('.', start);`          |
| 判断未找到     | `string::npos`   | `pos == string::npos`          |
| 截取字符串     | `substr()`       | `s.substr(start, len)`         |
| 删除内容       | `erase()`        | `s.erase(pos, len);`           |
| 插入内容       | `insert()`       | `s.insert(pos, "abc");`        |
| 清空字符串     | 直接赋值         | `s = "";`                      |
| 判断相等       | `==`             | `if (s == t)`                  |
| 字典序比较     | `<`、`>`         | `if (s < t)`                   |
| 字符串转整数   | `stoi()`         | `int x = stoi(s);`             |
| 字符串转长整数 | `stoll()`        | `long long x = stoll(s);`      |
| 数字转字符串   | `to_string()`    | `string s = to_string(x);`     |
| 反转           | `reverse()`      | `reverse(s.begin(), s.end());` |
| 排序           | `sort()`         | `sort(s.begin(), s.end());`    |
| 比较指定长度的字符串 | `compare()`        | `s.compare(0, prefix.size(), prefix) == 0`      |
| 判断字符串前缀       | `compare()`        | `if (s.compare(0, prefix.size(), prefix) == 0)` |

### vector 常用方法

| 需求       | 方法          | 示例                                  |
| ---------- | ------------- | ------------------------------------- |
| 替换元素   | `replace()`   | `replace(v.begin(), v.end(), 1, 2);`  |

### stack 常用方法

| 需求       | 方法       | 示例                 |
| ---------- | ---------- | -------------------- |
| 入栈       | `push()`   | `st.push(1);`        |
| 查看栈顶   | `top()`    | `st.top()`           |
| 出栈       | `pop()`    | `st.pop();`          |
| 判断为空   | `empty()`  | `st.empty()`         |

### queue 常用方法

| 需求       | 方法       | 示例                 |
| ---------- | ---------- | -------------------- |
| 入队       | `push()`   | `q.push(1);`         |
| 查看队首   | `front()`  | `q.front()`          |
| 查看队尾   | `back()`   | `q.back()`           |
| 出队       | `pop()`    | `q.pop();`           |
| 判断为空   | `empty()`  | `q.empty()`          |





### unordered_set方法

| 操作           | 写法           | 说明                         |
| -------------- | -------------- | ---------------------------- |
| 插入           | `st.insert(x)` | 插入元素                     |
| 查找           | `st.find(x)`   | 返回迭代器                   |
| 判断存在       | `st.count(x)`  | 存在返回 `1`，不存在返回 `0` |
| 删除指定值     | `st.erase(x)`  | 删除元素                     |
| 删除迭代器位置 | `st.erase(it)` | 删除对应元素                 |
| 元素个数       | `st.size()`    | 返回大小                     |
| 判空           | `st.empty()`   | 是否为空                     |
| 清空           | `st.clear()`   | 删除所有元素                 |



### unordered_map方法

| 操作       | 写法                      | 说明               |
| ---------- | ------------------------- | ------------------ |
| 插入键值对 | `mp[key] = value`         | 最常用             |
| 插入键值对 | `mp.insert({key, value})` | 不覆盖已有 key     |
| 查找       | `mp.find(key)`            | 返回迭代器         |
| 判断存在   | `mp.count(key)`           | 存在返回 `1`       |
| 访问值     | `mp[key]`                 | 根据 key 访问      |
| 安全访问   | `mp.at(key)`              | key 不存在会抛异常 |
| 删除       | `mp.erase(key)`           | 根据 key 删除      |
| 元素个数   | `mp.size()`               | 返回大小           |
| 判空       | `mp.empty()`              | 是否为空           |
| 清空       | `mp.clear()`              | 删除所有元素       |

### vector

| 操作         | 写法                   | 说明           |
| ------------ | ---------------------- | -------------- |
| 尾部添加     | `v.push_back(x)`       | 最常用         |
| 尾部删除     | `v.pop_back()`         | 删除最后一个   |
| 获取长度     | `v.size()`             | 元素个数       |
| 判断为空     | `v.empty()`            | 是否为空       |
| 访问元素     | `v[i]`                 | 下标访问       |
| 第一个元素   | `v[0]`                 | 注意非空       |
| 最后一个元素 | `v[v.size()-1]`        | 注意非空       |
| 指定位置插入 | `v.insert(pos, x)`     | `pos` 是迭代器 |
| 指定位置删除 | `v.erase(pos)`         | `pos` 是迭代器 |
| 删除区间     | `v.erase(first, last)` | 左闭右开       |
| 清空         | `v.clear()`            | 删除所有元素   |
