#include <algorithm>
#include <iostream>
#include <string>

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

    return 0;
}
