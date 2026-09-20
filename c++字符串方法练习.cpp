#include <algorithm>
#include <iostream>
#include <queue>
#include <stack>
#include <string>
#include <vector>

using namespace std;

int main() {
    string s = "hello world";
    string t = "cpp";

    // 获取长度、判断为空、访问字符
    cout << "长度: " << s.size() << '\n';
    cout << "是否为空: " << s.empty() << '\n';
    cout << "第一个字符: " << s[0] << '\n';
    cout << "最后一个字符: " << s[s.size() - 1] << '\n';

    // 拼接字符串、追加字符
    string joined = s + " " + t;
    joined += '!';
    cout << "拼接后: " << joined << '\n';

    // 查找、从指定位置查找、判断未找到
    int pos = joined.find("world");
    cout << "world 的位置: " << pos << '\n';
    int dotPos = joined.find('.', 0);
    if (dotPos == string::npos) {
        cout << "没有找到点号" << '\n';
    }

    // 截取、删除、插入
    string part = joined.substr(0, 5);
    cout << "截取前 5 个字符: " << part << '\n';

    string edited = joined;
    edited.erase(5, 1);       // 删除空格
    edited.insert(5, "-");   // 插入连字符
    cout << "删除并插入后: " << edited << '\n';

    // 清空字符串、判断相等、字典序比较
    string emptyString = edited;
    emptyString = "";
    cout << "清空后是否为空: " << emptyString.empty() << '\n';
    cout << "s 和 t 是否相等: " << (s == t) << '\n';
    cout << "s 是否排在 t 前面: " << (s < t) << '\n';

    // 字符串和数字互相转换
    string numberText = "12345";
    int number = stoi(numberText);
    long long bigNumber = stoll("1234567890123");
    string numberAgain = to_string(number + 1);
    cout << "stoi: " << number << '\n';
    cout << "stoll: " << bigNumber << '\n';
    cout << "to_string: " << numberAgain << '\n';

    // 反转、排序
    string letters = "dcab";
    reverse(letters.begin(), letters.end());
    cout << "反转后: " << letters << '\n';
    sort(letters.begin(), letters.end());
    cout << "排序后: " << letters << '\n';

    // compare：比较指定长度、判断前缀
    string filename = "cpp_notes.txt";
    string prefix = "cpp";
    cout << "前缀是否是 cpp: "
         << (filename.compare(0, prefix.size(), prefix) == 0) << '\n';
    cout << "前 3 个字符是否等于 cpp: "
         << (filename.compare(0, 3, "cpp") == 0) << '\n';

    // vector 中使用 replace：把所有 2 替换成 9
    vector<int> numbers = {1, 2, 3, 2, 4};
    replace(numbers.begin(), numbers.end(), 2, 9);
    cout << "vector 替换后: ";
    for (int number : numbers) {
        cout << number << ' ';
    }
    cout << '\n';

    // stack：后进先出
    stack<int> st;
    st.push(10);
    st.push(20);
    cout << "stack 栈顶: " << st.top() << '\n';
    st.pop();
    cout << "stack 出栈后栈顶: " << st.top() << '\n';

    // queue：先进先出
    queue<int> q;
    q.push(10);
    q.push(20);
    cout << "queue 队首: " << q.front() << '\n';
    cout << "queue 队尾: " << q.back() << '\n';
    q.pop();
    cout << "queue 出队后队首: " << q.front() << '\n';

    return 0;
}
