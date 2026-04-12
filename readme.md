# KBPaste

通过模拟键盘输入解决某些环境下无法使用 Ctrl+V 粘贴的问题。

警告：请勿在对输入行为有明确约束或可能产生不良后果的场合使用。使用者须自行承担全部责任。

编译：g++ -o kbpaste.exe kbpaste.cc -mwindows

用法：运行前将文本复制到剪贴板，然后执行 kbpaste.exe。