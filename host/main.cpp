#include "mainwindow.h"

#include <QApplication>
#include <QtGlobal>

/* MSVC 下让中文字符串按 UTF-8 编码 */
#if defined(_MSC_VER) && (_MSC_VER >= 1600)
#pragma execution_character_set("utf-8")
#endif

int main(int argc, char *argv[])
{
    QApplication a(argc, argv);

    /* 可选：让 Qt 在控制台打印调试信息（用 Qt Creator 运行时默认就有） */
    qSetMessagePattern("%{time hh:mm:ss.zzz} %{message}");

    MainWindow w;
    w.show();                 /* 显示窗口（不 show 的话窗口是不会出现的） */

    return a.exec();          /* ★ 进入事件循环，阻塞到这里直到程序退出 */
}
