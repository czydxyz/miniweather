# ============================================================================
# miniweather_host.pro —— qmake 工程文件
#
# 【qmake 是什么】
#   你写的是 .h/.cpp，但编译器需要 Makefile。
#   qmake 就是"读这个 .pro → 生成 Makefile"的工具，相当于 CMake 的简化版。
#   Qt Creator 打开 .pro 时会自动帮你跑一遍。
#
# 【serialport 模块不是默认带的】
#   Qt 把功能切成很多模块：core/gui/widgets 是基础，
#   串口在 serialport 模块、网络在 network 模块，必须显式加进来，
#   否则 #include <QSerialPort> 会报 "No such file or directory"。
# ============================================================================

QT += core gui
QT += widgets
QT += serialport
QT += network

# Qt5 起 widgets 从 gui 里独立出来了，这行是 Qt4/Qt5 兼容写法
greaterThan(QT_MAJOR_VERSION, 4): QT += widgets

CONFIG += c++11

TARGET   = miniweather_host
TEMPLATE = app

# 源码
SOURCES += \
    main.cpp \
    mainwindow.cpp \
    protocol.cpp

HEADERS += \
    mainwindow.h \
    protocol.h

# MSVC 编译器下让源码中的中文按 UTF-8 处理，避免界面中文乱码
win32-msvc* {
    QMAKE_CXXFLAGS += /utf-8
}

# 发布/调试版本的额外设置（可不看）
CONFIG(debug, debug|release) {
    # 调试版：打开运行时断言
    DEFINES += QT_DEPRECATED_WARNINGS
}
