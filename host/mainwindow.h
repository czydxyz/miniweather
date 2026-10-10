#ifndef MAINWINDOW_H
#define MAINWINDOW_H

#include <QMainWindow>
#include <QByteArray>
#include <QTimer>
#include <QElapsedTimer>

#include "protocol.h"

/* 前向声明（forward declaration）：
   头文件里只说"有这么个类"，不 include 它的头文件。
   好处：改了 Qt 头文件不会导致所有文件重新编译，编译快很多。
   这是 Qt 项目的标准写法，你会在几乎所有 .h 里见到。 */
class QSerialPort;
class QTcpServer;
class QTcpSocket;
class QPushButton;
class QComboBox;
class QSpinBox;
class QLineEdit;
class QLabel;
class QProgressBar;
class QPlainTextEdit;
class QRadioButton;
class QCheckBox;
class QGroupBox;

class MainWindow : public QMainWindow
{
    Q_OBJECT

public:
    explicit MainWindow(QWidget *parent = nullptr);
    ~MainWindow();      /* 析构函数：关串口、关 socket，避免程序退出时资源泄漏 */

private slots:
    /* --- 连接相关 --- */
    void onOpenClicked();      /* 打开/关闭 连接 */
    void onRefreshPorts();     /* 刷新可用串口列表 */
    void onModeChanged();      /* 切换 串口/TCP 模式 */

    /* --- 数据到达 --- */
    void onSerialReadyRead();  /* 串口来数据了 */
    void onTcpNewConnection(); /* 有下位机 TCP 连进来了 */
    void onTcpReadyRead();     /* TCP 来数据了 */
    void onTcpDisconnected();  /* 下位机断开 */

    /* --- 协议帧到达（由 FrameParser 的 frameReady 触发）--- */
    void onFrame(quint16 cmd, const QByteArray &payload);

    /* --- 按钮 --- */
    void onBtnVersion();       /* 查版本 */
    void onBtnHeartbeat();     /* 心跳 */
    void onBtnReadSensor();    /* 读传感器 */
    void onBtnCtrl();          /* 控制执行器 */
    void onBtnBrowse();        /* 选择固件文件 */
    void onBtnOta();           /* ★ 开始 OTA 升级 */

    void onClearLog();         /* 清空日志 */
    void onSaveLog();          /* 保存日志到 txt（联调留证据用） */

signals:
    /* 当收到"任意应答帧"时发射，用来唤醒正在等待的 transact() */
    void ackArrived();

private:
    /* --- 界面搭建（构造函数里调用）--- */
    void buildUi();            /* 创建所有控件并布局 */
    QGroupBox *buildConnBox(); /* 连接区 */
    QGroupBox *buildCmdBox();  /* 命令区 */
    QGroupBox *buildOtaBox();  /* OTA 区 */
    void refreshFwInfo();      /* 重新读取固件并刷新"大小/包数/CRC32/基址"显示 */

    /* --- 通信 --- */
    bool     isOpen() const;                       /* 当前是否已连上 */
    bool     sendRaw(const QByteArray &d);         /* 原始发送（按当前通道分发） */
    void     log(const QString &s);                /* 往日志框加一行 */
    void     logTx(const QByteArray &d);           /* 记录发出的帧 */
    void     logRx(const QByteArray &d);           /* 记录收到的原始字节 */

    /* --- 收发一帧并等应答（本程序的核心机制，教学重点）--- */
    struct Ack {
        bool        ok = false;   /* 是否收到应答（超时 = false） */
        quint8      err = 0xFF;   /* 应答里的错误码 */
        QByteArray  data;         /* 错误码之后的数据 */
    };
    Ack transact(quint16 cmd, const QByteArray &payload, int timeoutMs = 1500);

    /* --- OTA --- */
    bool waitOtaReady(int timeoutMs);   /* START 之后轮询心跳，等擦除完成 */

    /* --- 成员变量 --- */
    enum LinkMode { ModeNone, ModeSerial, ModeTcp };
    LinkMode    m_mode = ModeNone;

    QSerialPort *m_serial = nullptr;    /* 串口通道 */
    QTcpServer  *m_server = nullptr;    /* TCP 服务器（下位机是客户端，主动连进来） */
    QTcpSocket  *m_sock   = nullptr;    /* 已连接的下位机 socket */

    FrameParser  m_parser;              /* 协议解析状态机（两种通道共用） */

    /* 等待应答用的临时区 */
    bool        m_ackGot = false;
    quint16     m_ackCmd = 0;
    QByteArray  m_ackPayload;

    bool        m_otaRunning = false;   /* OTA 进行中，禁止重复点击 */

    /* --- 控件指针（需要跨函数访问的才存成成员）--- */
    QRadioButton   *m_rbSerial = nullptr;
    QRadioButton   *m_rbTcp    = nullptr;
    QComboBox      *m_cmbPort  = nullptr;
    QComboBox      *m_cmbBaud  = nullptr;
    QSpinBox       *m_spnTcpPort = nullptr;
    QPushButton    *m_btnOpen  = nullptr;
    QLabel         *m_lblStatus = nullptr;

    QLabel         *m_lblVersion  = nullptr;
    QLabel         *m_lblUptime   = nullptr;
    QLabel         *m_lblOtaState = nullptr;
    QComboBox      *m_cmbDev      = nullptr;
    QComboBox      *m_cmbAct      = nullptr;

    QLabel         *m_lblTemp  = nullptr;
    QLabel         *m_lblHumi  = nullptr;
    QLabel         *m_lblPress = nullptr;
    QLabel         *m_lblCo2   = nullptr;
    QLabel         *m_lblTvoc  = nullptr;

    QLineEdit      *m_edtFw    = nullptr;
    QLabel         *m_lblFwInfo = nullptr;
    QSpinBox       *m_spnBank  = nullptr;
    QPushButton    *m_btnOta   = nullptr;
    QProgressBar   *m_barOta   = nullptr;

    QPlainTextEdit *m_txtLog   = nullptr;
    QCheckBox      *m_chkHex   = nullptr;
};

#endif // MAINWINDOW_H
