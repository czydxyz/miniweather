/* Qt 上位机主窗口
 * [约束] OTA 块超时 3000ms / 重试 4 次 / 重试间隔 150ms（串口直连的 1.5s/3 次在 WiFi 下会误判）。
 * [约束] 重传必须复用同一个 off，下位机按 off 判幂等。
 * [约束] OTA 时序：Start -> 等擦除 -> 逐块 -> Finish(CRC32) -> 切 Bank，每步等应答。
 */

/* MSVC 编译器默认把源码里的中文字符串按本地编码(GBK)处理，会显示乱码。
   这一句强制它按 UTF-8 存中文。MinGW 不需要，但写了也无害。 */
#if defined(_MSC_VER) && (_MSC_VER >= 1600)
#pragma execution_character_set("utf-8")
#endif

#include "mainwindow.h"

#include <QApplication>
#include <QSerialPort>
#include <QSerialPortInfo>
#include <QTcpServer>
#include <QTcpSocket>
#include <QFile>
#include <QFileDialog>
#include <QMessageBox>
#include <QEventLoop>
#include <QTimer>
#include <QDateTime>
#include <QStatusBar>
#include <QFont>
#include <QScrollBar>

#include <QGroupBox>
#include <QFormLayout>
#include <QGridLayout>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QComboBox>
#include <QSpinBox>
#include <QLineEdit>
#include <QProgressBar>
#include <QPlainTextEdit>
#include <QRadioButton>
#include <QCheckBox>

static void sleepMsEventLoop(int ms)
{
    QEventLoop loop;
    QTimer::singleShot(ms, &loop, &QEventLoop::quit);
    loop.exec();
}

#define OTA_BLOCK_TIMEOUT_MS   3000
#define OTA_BLOCK_RETRY_MAX    4
#define OTA_BLOCK_RETRY_GAP_MS 150

/* ---------------- 错误码 / OTA 状态 的中文翻译 ----------------
   放在文件靠前的位置，因为 onFrame() 和 onBtnOta() 都会用到它们。
   C++ 的规则：函数必须先声明（或定义）才能被调用，顺序不能颠倒。 */
static const char *otaStateText(quint8 s)
{
    switch (s) {
    case OTA_IDLE:      return "空闲";
    case OTA_ERASING:   return "擦除中";
    case OTA_READY:     return "就绪(可发块)";
    case OTA_RECEIVING: return "接收中";
    case OTA_DONE:      return "完成";
    case OTA_FAIL:      return "失败";
    default:            return "未知";
    }
}
static QString otaStateName(quint8 s) { return QString::fromUtf8(otaStateText(s)); }

static const char *errCodeText(quint8 e)
{
    switch (e) {
    case ERR_OK:      return "成功";
    case ERR_CMD:     return "未知命令";
    case ERR_PARAM:   return "参数非法";
    case ERR_LEN:     return "长度超限";
    case ERR_BUSY:    return "设备忙（顺序错了？）";
    case ERR_FLASH:   return "Flash 擦写失败";
    case ERR_RANGE:   return "越界/大小超限";
    case ERR_TIMEOUT: return "超时";
    case ERR_CRC32:   return "CRC32 不匹配";
    default:          return "未知";
    }
}
static QString errText(quint8 e) { return QString::fromUtf8(errCodeText(e)); }

/* 构造 / 析构 */
MainWindow::MainWindow(QWidget *parent)
    : QMainWindow(parent)
{
    /* 串口对象在这里 new，父对象设为 this，
       这样窗口销毁时 Qt 会自动帮你 delete（Qt 的对象树机制）。 */
    m_serial = new QSerialPort(this);
    m_server = new QTcpServer(this);

    buildUi();

    /* ★ 把协议解析器的"收到一帧"信号，接到本窗口的 onFrame 槽。
       这一行就是整个程序的"数据总线"：
       不管是串口来的还是 TCP 来的字节，最终都汇到 onFrame 处理。 */
    connect(&m_parser, &FrameParser::frameReady,
            this, &MainWindow::onFrame);
    connect(&m_parser, &FrameParser::crcError,
            this, [this]() { log(QString("! CRC 错误（累计 %1 次）").arg(m_parser.crcErrors())); });

    onRefreshPorts();
    onModeChanged();

    log("就绪。请选择通道并点击「打开」。");
}

MainWindow::~MainWindow()
{
    /* 析构里做清理：串口和 socket 都是系统资源，不关会导致下次打不开。
       严格来说 Qt 对象树会delete对象，但"显式关闭"是好习惯。 */
    if (m_serial && m_serial->isOpen())
        m_serial->close();
    if (m_sock) {
        m_sock->close();
    }
    if (m_server && m_server->isListening())
        m_server->close();
}

void MainWindow::buildUi()
{
    /* 中心部件：QMainWindow 必须用 setCentralWidget 指定一个，
       不能直接往它上面加控件。这是 QMainWindow 和 QWidget 最大的区别。 */
    QWidget *center = new QWidget(this);
    QVBoxLayout *mainLayout = new QVBoxLayout(center);

    /* 上半部分：三个功能区横向排开 */
    QHBoxLayout *topLayout = new QHBoxLayout();
    topLayout->addWidget(buildConnBox(), 1);
    topLayout->addWidget(buildCmdBox(),  1);
    topLayout->addWidget(buildOtaBox(),  1);
    mainLayout->addLayout(topLayout);

    /* 下半部分：日志区（stretch=1 表示它吃掉所有剩余空间） */
    QGroupBox *logBox = new QGroupBox("通信日志", center);
    QVBoxLayout *logLayout = new QVBoxLayout(logBox);

    m_txtLog = new QPlainTextEdit(logBox);
    m_txtLog->setReadOnly(true);
    /* 等宽字体，方便对齐 HEX */
    QFont f("Consolas", 9);
    m_txtLog->setFont(f);
    m_txtLog->setMaximumBlockCount(2000);   /* 最多留 2000 行，防止长时间跑爆内存 */
    logLayout->addWidget(m_txtLog);

    QHBoxLayout *logBtnLayout = new QHBoxLayout();
    m_chkHex = new QCheckBox("显示原始 HEX", logBox);
    m_chkHex->setChecked(true);
    QPushButton *btnClear = new QPushButton("清空", logBox);
    connect(btnClear, &QPushButton::clicked, this, &MainWindow::onClearLog);
    QPushButton *btnSave = new QPushButton("保存日志", logBox);
    connect(btnSave, &QPushButton::clicked, this, &MainWindow::onSaveLog);
    logBtnLayout->addWidget(m_chkHex);
    logBtnLayout->addStretch();
    logBtnLayout->addWidget(btnSave);
    logBtnLayout->addWidget(btnClear);
    logLayout->addLayout(logBtnLayout);

    mainLayout->addWidget(logBox, 1);

    setCentralWidget(center);
    setWindowTitle("mini-weather 上位机  (OTA / 监控)");
    resize(1000, 680);
    statusBar()->showMessage("未连接");
}

/* --- 连接区 --- */
QGroupBox *MainWindow::buildConnBox()
{
    QGroupBox *box = new QGroupBox("1. 连接", this);
    QVBoxLayout *v = new QVBoxLayout(box);

    /* 模式选择：两个单选按钮。
       QRadioButton 同一个父容器里互斥，点一个另一个自动取消。 */
    m_rbSerial = new QRadioButton("串口（USB-TTL 接 PB10/PB11）", box);
    m_rbTcp    = new QRadioButton("TCP 服务器（ESP8266 透传）", box);
    m_rbSerial->setChecked(true);
    v->addWidget(m_rbSerial);
    v->addWidget(m_rbTcp);
    connect(m_rbSerial, &QRadioButton::toggled, this, &MainWindow::onModeChanged);

    /* 串口参数 */
    QFormLayout *form = new QFormLayout();
    QHBoxLayout *portRow = new QHBoxLayout();
    m_cmbPort = new QComboBox(box);
    m_cmbPort->setMinimumWidth(140);
    QPushButton *btnRefresh = new QPushButton("刷新", box);
    connect(btnRefresh, &QPushButton::clicked, this, &MainWindow::onRefreshPorts);
    portRow->addWidget(m_cmbPort, 1);
    portRow->addWidget(btnRefresh);
    form->addRow("串口号", portRow);

    m_cmbBaud = new QComboBox(box);
    /* ★ 波特率必须和下位机一致：本项目 USART3 = 115200 */
    m_cmbBaud->addItems({"9600", "19200", "38400", "57600", "115200", "230400", "460800"});
    m_cmbBaud->setCurrentText("115200");
    form->addRow("波特率", m_cmbBaud);

    m_spnTcpPort = new QSpinBox(box);
    m_spnTcpPort->setRange(1, 65535);
    m_spnTcpPort->setValue(8888);      /* 和 esp8266.h 里 TCP_SERVER_PORT 一致 */
    form->addRow("TCP 端口", m_spnTcpPort);
    v->addLayout(form);

    m_btnOpen = new QPushButton("打开", box);
    connect(m_btnOpen, &QPushButton::clicked, this, &MainWindow::onOpenClicked);
    v->addWidget(m_btnOpen);

    m_lblStatus = new QLabel("未连接", box);
    m_lblStatus->setWordWrap(true);
    v->addWidget(m_lblStatus);

    v->addStretch();
    return box;
}

/* --- 命令区 --- */
QGroupBox *MainWindow::buildCmdBox()
{
    QGroupBox *box = new QGroupBox("2. 基本命令 / 传感器", this);
    QVBoxLayout *v = new QVBoxLayout(box);

    /* 设备信息 */
    QFormLayout *info = new QFormLayout();
    m_lblVersion  = new QLabel("-", box);
    m_lblUptime   = new QLabel("-", box);
    m_lblOtaState = new QLabel("-", box);
    info->addRow("固件版本", m_lblVersion);
    info->addRow("运行时长", m_lblUptime);
    info->addRow("下位机OTA", m_lblOtaState);
    v->addLayout(info);

    /* 按钮行 */
    QHBoxLayout *row1 = new QHBoxLayout();
    QPushButton *b1 = new QPushButton("查版本", box);
    QPushButton *b2 = new QPushButton("心跳", box);
    QPushButton *b3 = new QPushButton("读传感器", box);
    connect(b1, &QPushButton::clicked, this, &MainWindow::onBtnVersion);
    connect(b2, &QPushButton::clicked, this, &MainWindow::onBtnHeartbeat);
    connect(b3, &QPushButton::clicked, this, &MainWindow::onBtnReadSensor);
    row1->addWidget(b1); row1->addWidget(b2); row1->addWidget(b3);
    v->addLayout(row1);

    /* 执行器控制 */
    QGridLayout *grid = new QGridLayout();
    m_cmbDev = new QComboBox(box);
    m_cmbDev->addItem("舵机 / 开窗", int(DEV_SERVO));
    m_cmbDev->addItem("继电器 / 风扇", int(DEV_RELAY));
    m_cmbDev->addItem("蜂鸣器", int(DEV_BEEP));
    m_cmbAct = new QComboBox(box);
    m_cmbAct->addItem("关 (0)", 0);
    m_cmbAct->addItem("开 (1)", 1);
    QPushButton *b4 = new QPushButton("下发控制", box);
    connect(b4, &QPushButton::clicked, this, &MainWindow::onBtnCtrl);
    grid->addWidget(new QLabel("设备", box), 0, 0);
    grid->addWidget(m_cmbDev, 0, 1);
    grid->addWidget(new QLabel("动作", box), 1, 0);
    grid->addWidget(m_cmbAct, 1, 1);
    grid->addWidget(b4, 2, 0, 1, 2);
    v->addLayout(grid);

    /* 传感器数值 */
    QGroupBox *sbox = new QGroupBox("实时数据", box);
    QFormLayout *sf = new QFormLayout(sbox);
    m_lblTemp  = new QLabel("--", sbox);
    m_lblHumi  = new QLabel("--", sbox);
    m_lblPress = new QLabel("--", sbox);
    m_lblCo2   = new QLabel("--", sbox);
    m_lblTvoc  = new QLabel("--", sbox);
    sf->addRow("温度", m_lblTemp);
    sf->addRow("湿度", m_lblHumi);
    sf->addRow("气压", m_lblPress);
    sf->addRow("CO2eq", m_lblCo2);
    sf->addRow("TVOC", m_lblTvoc);
    v->addWidget(sbox);

    v->addStretch();
    return box;
}

/* --- OTA 区 --- */
QGroupBox *MainWindow::buildOtaBox()
{
    QGroupBox *box = new QGroupBox("3. 固件升级 OTA", this);
    QVBoxLayout *v = new QVBoxLayout(box);

    QHBoxLayout *fileRow = new QHBoxLayout();
    m_edtFw = new QLineEdit(box);
    m_edtFw->setPlaceholderText("选择 .bin 固件文件");
    QPushButton *btnBrowse = new QPushButton("...", box);
    btnBrowse->setFixedWidth(36);
    connect(btnBrowse, &QPushButton::clicked, this, &MainWindow::onBtnBrowse);
    fileRow->addWidget(m_edtFw, 1);
    fileRow->addWidget(btnBrowse);
    v->addLayout(fileRow);

    m_lblFwInfo = new QLabel("未选择文件", box);
    m_lblFwInfo->setWordWrap(true);
    v->addWidget(m_lblFwInfo);

    QFormLayout *f = new QFormLayout();
    m_spnBank = new QSpinBox(box);
    m_spnBank->setRange(0, 1);      /* 0 = A 区(0x08060000)，1 = B 区(0x080A0000) */
    m_spnBank->setValue(0);
    f->addRow("目标 Bank", m_spnBank);
    v->addLayout(f);

    m_barOta = new QProgressBar(box);
    m_barOta->setRange(0, 100);
    m_barOta->setValue(0);
    v->addWidget(m_barOta);

    m_btnOta = new QPushButton("开始升级", box);
    connect(m_btnOta, &QPushButton::clicked, this, &MainWindow::onBtnOta);
    v->addWidget(m_btnOta);

    QLabel *tip = new QLabel(
        "流程：Start → 等擦除 → 分块发送 → Finish(CRC32对账) → 切Bank\n"
        "★ 切 Bank 只写标志区，需复位后由 Bootloader 搬运", box);
    tip->setWordWrap(true);
    v->addWidget(tip);

    v->addStretch();
    return box;
}

/* 二、连接管理 */
void MainWindow::onModeChanged()
{
    bool serial = m_rbSerial->isChecked();
    m_cmbPort->setEnabled(serial);
    m_cmbBaud->setEnabled(serial);
    m_spnTcpPort->setEnabled(!serial);
}

void MainWindow::onRefreshPorts()
{
    m_cmbPort->clear();
    /* QSerialPortInfo 是 Qt 提供的串口枚举工具，
       相当于你在设备管理器里看到的 "COM3" 那些。 */
    const auto ports = QSerialPortInfo::availablePorts();
    if (ports.isEmpty()) {
        m_cmbPort->addItem("(无可用串口)");
        return;
    }
    for (const QSerialPortInfo &p : ports) {
        /* 把描述信息也带上，方便区分"USB-SERIAL CH340" 是哪一个 */
        QString name = p.portName();
        if (!p.description().isEmpty())
            name += " - " + p.description();
        m_cmbPort->addItem(name, p.portName());   /* 显示名 / 实际名分开存 */
    }
}

void MainWindow::onOpenClicked()
{
    if (isOpen()) {
        /* --- 已连接 → 关闭 --- */
        if (m_mode == ModeSerial && m_serial) {
            m_serial->close();
        } else if (m_mode == ModeTcp) {
            if (m_sock) { m_sock->close(); }
            if (m_server) { m_server->close(); }
        }
        m_mode = ModeNone;
        m_btnOpen->setText("打开");
        m_lblStatus->setText("已断开");
        statusBar()->showMessage("已断开");
        log("连接已关闭。");
        return;
    }

    /* --- 未连接 → 打开 --- */
    m_parser.reset();     /* ★ 换通道/重连必须复位状态机，否则会接着上次半帧继续解 */

    if (m_rbSerial->isChecked()) {
        QString portName = m_cmbPort->currentData().toString();
        if (portName.isEmpty()) portName = m_cmbPort->currentText();
        if (portName.isEmpty() || portName.startsWith("(")) {
            QMessageBox::warning(this, "提示", "没有可用串口，请检查 USB-TTL 是否插好。");
            return;
        }
        m_serial->setPortName(portName);
        m_serial->setBaudRate(m_cmbBaud->currentText().toInt());
        m_serial->setDataBits(QSerialPort::Data8);          /* 8 位数据 */
        m_serial->setParity(QSerialPort::NoParity);         /* 无校验 */
        m_serial->setStopBits(QSerialPort::OneStop);        /* 1 位停止位 */
        m_serial->setFlowControl(QSerialPort::NoFlowControl);/* 无流控 */

        if (!m_serial->open(QIODevice::ReadWrite)) {
            QMessageBox::critical(this, "错误",
                                  "打开串口失败：" + m_serial->errorString());
            return;
        }
        /* ★ readyRead 信号 = "有数据来了"。
           类比 STM32：这就是"接收中断"，Qt 帮你把事件排进主循环，
           你只管写处理函数，不用管中断向量表。 */
        connect(m_serial, &QSerialPort::readyRead,
                this, &MainWindow::onSerialReadyRead);
        m_mode = ModeSerial;
        m_btnOpen->setText("关闭");
        m_lblStatus->setText("串口 " + portName + " 已打开 @" + m_cmbBaud->currentText());
        log("串口已打开：" + portName);
    } else {
        quint16 port = quint16(m_spnTcpPort->value());
        /* 下位机是 TCP 客户端（ESP8266 主动连过来），所以上位机必须是服务器 */
        if (!m_server->listen(QHostAddress::Any, port)) {
            QMessageBox::critical(this, "错误",
                                  "监听失败：" + m_server->errorString());
            return;
        }
        connect(m_server, &QTcpServer::newConnection,
                this, &MainWindow::onTcpNewConnection);
        m_mode = ModeTcp;
        m_btnOpen->setText("关闭");
        m_lblStatus->setText(QString("监听端口 %1，等待下位机连接...").arg(port));
        log(QString("TCP 服务器已启动，监听 %1").arg(port));
    }
    statusBar()->showMessage("已连接");
}

void MainWindow::onTcpNewConnection()
{
    /* 可能有多个连接进来，只保留最后一个（本项目只有一个下位机） */
    if (m_sock) {
        m_sock->close();
        m_sock->deleteLater();
    }
    m_sock = m_server->nextPendingConnection();
    connect(m_sock, &QTcpSocket::readyRead, this, &MainWindow::onTcpReadyRead);
    connect(m_sock, &QTcpSocket::disconnected, this, &MainWindow::onTcpDisconnected);
    m_parser.reset();
    log(QString("下位机已连接：%1:%2")
            .arg(m_sock->peerAddress().toString())
            .arg(m_sock->peerPort()));
    m_lblStatus->setText("已连接：" + m_sock->peerAddress().toString());
}

void MainWindow::onTcpDisconnected()
{
    log("下位机断开连接。");
    m_sock = nullptr;
    m_lblStatus->setText("监听中，等待连接...");
}

bool MainWindow::isOpen() const
{
    if (m_mode == ModeSerial)
        return m_serial && m_serial->isOpen();
    if (m_mode == ModeTcp)
        return m_sock && m_sock->state() == QAbstractSocket::ConnectedState;
    return false;
}

/* 三、数据收发 */
void MainWindow::onSerialReadyRead()
{
    QByteArray d = m_serial->readAll();     /* 一次读光缓冲区 */
    logRx(d);
    m_parser.feed(d);                       /* 喂给状态机，来多少喂多少 */
}

void MainWindow::onTcpReadyRead()
{
    if (!m_sock) return;
    QByteArray d = m_sock->readAll();
    logRx(d);
    m_parser.feed(d);
}

bool MainWindow::sendRaw(const QByteArray &d)
{
    if (!isOpen()) return false;
    if (m_mode == ModeSerial)
        return m_serial->write(d) == d.size();
    if (m_mode == ModeTcp && m_sock)
        return m_sock->write(d) == d.size();
    return false;
}

MainWindow::Ack MainWindow::transact(quint16 cmd, const QByteArray &payload, int timeoutMs)
{
    Ack a;

    if (!isOpen()) {
        log("! 未连接，无法发送");
        return a;
    }

    /* ① 清掉上一次的应答残留 */
    m_ackGot     = false;
    m_ackCmd     = 0;
    m_ackPayload.clear();

    /* ② 打包并发送 */
    QByteArray frame = Proto::buildFrame(cmd, payload);
    logTx(frame);
    if (sendRaw(frame) == false) {
        log("! 发送失败");
        return a;
    }

    /* ③ 开一个局部事件循环等应答，最多等 timeoutMs 毫秒 */
    QEventLoop loop;
    QTimer     timer;
    timer.setSingleShot(true);                      /* 只响一次 */
    connect(this,  &MainWindow::ackArrived, &loop, &QEventLoop::quit);
    connect(&timer, &QTimer::timeout,       &loop, &QEventLoop::quit);
    timer.start(timeoutMs);
    loop.exec();                                    /* ★ 阻塞点 */

    /* ④ 判断结果 */
    if (m_ackGot == false) {
        a.ok = false;
        return a;                                   /* 超时 */
    }

    /* 命令字对不对？应答帧 = 请求命令 | 0x8000 */
    if (m_ackCmd != quint16(cmd | PROTO_ACK_FLAG)) {
        a.ok = false;
        log(QString("! 应答命令字不匹配：期望 0x%1，实际 0x%2")
                .arg(quint32(cmd | PROTO_ACK_FLAG), 4, 16, QLatin1Char('0'))
                .arg(quint32(m_ackCmd), 4, 16, QLatin1Char('0')));
        return a;
    }

    a.ok  = true;
    /* ★ 应答数据区第 1 字节永远是错误码，后面才是真正的数据 */
    a.err = m_ackPayload.isEmpty() ? 0xFF : quint8(m_ackPayload.at(0));
    a.data = m_ackPayload.mid(1);
    return a;
}

/* 五、收到一帧后的处理 */
void MainWindow::onFrame(quint16 cmd, const QByteArray &payload)
{
    /* ① 先无条件登记"收到应答了"，去唤醒可能在等待的 transact() */
    m_ackGot     = true;
    m_ackCmd     = cmd;
    m_ackPayload = payload;
    emit ackArrived();

    /* ② 再把数据刷新到界面（这部分是"主动显示"，与等待应答无关） */
    quint16 req = quint16(cmd & ~PROTO_ACK_FLAG);   /* 去掉应答位，还原请求命令 */
    if (payload.isEmpty()) return;
    quint8 err = quint8(payload.at(0));
    QByteArray d = payload.mid(1);                 /* 错误码之后的纯数据 */

    switch (req) {
    case CMD_GET_VERSION:
        m_lblVersion->setText(err == ERR_OK ? QString::fromLatin1(d) : "错误 " + QString::number(err));
        break;

    case CMD_HEARTBEAT:
        /* 应答数据 = 运行秒数(4) + OTA 状态(1) */
        if (err == ERR_OK && d.size() >= 5) {
            quint32 up = Proto::getU32BE(d, 0);
            quint8  st = quint8(d.at(4));
            m_lblUptime->setText(QString("%1 秒 (%2:%3)")
                                     .arg(up)
                                     .arg(up / 60)
                                     .arg(up % 60, 2, 10, QLatin1Char('0')));
            m_lblOtaState->setText(QString("%1 - %2").arg(st).arg(otaStateName(st)));
        }
        break;

    case CMD_READ_SENSOR:
        /* 应答数据 13 字节：
           temp(2,有符号,×10) humi(2,×10) press(4,Pa) co2(2) tvoc(2) valid(1) */
        if (err == ERR_OK && d.size() >= 13) {
            qint16  t  = qint16(Proto::getU16BE(d, 0));   /* ★ 温度有符号 */
            quint16 h  = Proto::getU16BE(d, 2);
            quint32 pa = Proto::getU32BE(d, 4);
            quint16 co2 = Proto::getU16BE(d, 8);
            quint16 tvoc = Proto::getU16BE(d, 10);
            quint8  valid = quint8(d.at(12));

            if (valid == 0) {
                m_lblTemp->setText("未采样");
            } else {
                m_lblTemp->setText(QString::number(t / 10.0, 'f', 1) + " ℃");
                m_lblHumi->setText(QString::number(h / 10.0, 'f', 1) + " %RH");
                m_lblPress->setText(QString::number(pa / 100.0, 'f', 1) + " hPa");
                m_lblCo2->setText(QString::number(co2) + " ppm");
                m_lblTvoc->setText(QString::number(tvoc) + " ppb");
            }
        } else {
            log(QString("! 读传感器失败，错误码 0x%1").arg(quint32(err), 2, 16, QLatin1Char('0')));
        }
        break;

    default:
        break;
    }
}

/* 六、各按钮 */
void MainWindow::onBtnVersion()
{
    Ack a = transact(CMD_GET_VERSION, QByteArray(), 1500);
    if (a.ok)
        log(QString("版本：%1（错误码 0x%2）")
                .arg(a.err == ERR_OK ? QString::fromLatin1(a.data) : "-")
                .arg(quint32(a.err), 2, 16, QLatin1Char('0')));
    else
        log("! 查版本超时 —— 检查：线通不通、波特率对不对、下位机 Comm_Init 跑了没");
}

void MainWindow::onBtnHeartbeat()
{
    Ack a = transact(CMD_HEARTBEAT, QByteArray(), 1500);
    if (!a.ok) log("! 心跳超时");
}

void MainWindow::onBtnReadSensor()
{
    Ack a = transact(CMD_READ_SENSOR, QByteArray(), 1500);
    if (!a.ok) log("! 读传感器超时");
}

void MainWindow::onBtnCtrl()
{
    /* 数据 = 设备ID(1) + 动作(1)。
       currentData() 拿到的是 addItem 时存的 int，用 toInt() 取出。 */
    QByteArray p;
    p.append(char(m_cmbDev->currentData().toInt()));
    p.append(char(m_cmbAct->currentData().toInt()));
    Ack a = transact(CMD_CTRL_ACTUATOR, p, 1500);
    log(a.ok ? QString("控制已下发，错误码 0x%1").arg(quint32(a.err), 2, 16, QLatin1Char('0'))
             : QString("! 控制超时"));
}

/* 固件文件加载（.bin / .hex 自动识别）实现已移到 protocol.cpp，
   由 Proto::loadFirmware() / Proto::parseIntelHex() 提供 —— 放协议层
   是为了让控制台自检程序也能调用同一份真代码做回归测试。 */

void MainWindow::onBtnBrowse()
{
    QString path = QFileDialog::getOpenFileName(
        this, "选择固件文件", QString(), "固件文件 (*.bin *.hex);;所有文件 (*.*)");
    if (path.isEmpty()) return;
    m_edtFw->setText(path);
    refreshFwInfo();        /* 选完立刻算大小 / 包数 / CRC32 显示出来 */
}

/* 读文件 → 解析 → 把 大小/包数/CRC32/起始地址 显示在标签上。
   注意：这里必须走 loadFirmware()，不能直接 readAll()，
   否则选到 .hex 时显示的大小和 CRC32 全是文本的，与实际要发的固件不符。 */
void MainWindow::refreshFwInfo()
{
    const QString path = m_edtFw->text();
    if (path.isEmpty()) { m_lblFwInfo->setText("未选择固件"); return; }

    QByteArray fw;
    quint32    base = 0;
    QString    err;
    if (!Proto::loadFirmware(path, &fw, &base, &err)) {
        m_lblFwInfo->setText("读文件失败：" + err);
        return;
    }

    const quint32 crc    = Proto::crc32(reinterpret_cast<const quint8 *>(fw.constData()),
                                        quint32(fw.size()));
    const int     blocks = int((fw.size() + OTA_BLOCK_DATA - 1) / OTA_BLOCK_DATA);

    QString txt = QString("大小 %1 字节 (%2 KB)\n包数 %3（每包 %4 字节）\nCRC32 0x%5")
                      .arg(fw.size())
                      .arg(fw.size() / 1024.0, 0, 'f', 1)
                      .arg(blocks)
                      .arg(OTA_BLOCK_DATA)
                      .arg(quint32(crc), 8, 16, QLatin1Char('0')).toUpper();

    /* hex 里带地址，顺便校验一下链接基址对不对；bin 看不到地址，只能提示 */
    if (base != 0) {
        txt += QString("\n起始地址 0x%1").arg(base, 8, 16, QLatin1Char('0')).toUpper();
        if (base != OTA_APP_BASE) {
            txt += QString("\n⚠ 基址应为 0x%1").arg(OTA_APP_BASE, 8, 16, QLatin1Char('0')).toUpper();
        }
    }
    if (fw.size() > int(OTA_BANK_SIZE)) {
        txt += QString("\n⚠ 超过 Bank 容量 %1 字节").arg(OTA_BANK_SIZE);
    }
    m_lblFwInfo->setText(txt);
}

bool MainWindow::waitOtaReady(int timeoutMs)
{
    QElapsedTimer t;
    t.start();
    int lastState = -1;
    while (t.elapsed() < timeoutMs) {
        Ack a = transact(CMD_HEARTBEAT, QByteArray(), 800);
        if (a.ok && a.err == ERR_OK && a.data.size() >= 5) {
            quint8 st = quint8(a.data.at(4));
            if (st != lastState) {          /* 状态变了才打印，避免刷屏 */
                log(QString("  下位机 OTA 状态 → %1 (%2)")
                        .arg(st).arg(otaStateName(st)));
                lastState = st;
            }
            if (st == OTA_READY)     return true;
            if (st == OTA_FAIL)      { log("! 下位机 OTA 状态 = 失败，请查 Flash"); return false; }
        }
        sleepMsEventLoop(250);              /* 每 250ms 问一次，别把串口堵死 */
    }
    log("! 等待擦除完成超时");
    return false;
}

void MainWindow::onBtnOta()
{
    if (m_otaRunning) return;               /* 防重入 */

    /* ---------- ① 读固件文件 ----------
       ★ 必须走 loadFirmware()：它会把 .hex 解析成裸镜像，
         直接 readAll() 拿到的是 ASCII 文本，发下去必然失败。 */
    QString path = m_edtFw->text();
    if (path.isEmpty()) { QMessageBox::warning(this, "提示", "请先选择固件文件"); return; }

    QByteArray fw;
    quint32    base = 0;
    QString    err;
    if (!Proto::loadFirmware(path, &fw, &base, &err)) {
        QMessageBox::warning(this, "错误", "读固件失败：" + err);
        return;
    }
    if (fw.isEmpty()) { QMessageBox::warning(this, "错误", "文件是空的"); return; }

    /* hex 自带地址 → 顺便验一下链接基址，能提前拦住"刷了跑不起来"的固件。
       bin 看不到地址，只能靠生成时保证（fromelf 出来的必然是 APP_ADDR 基址）。 */
    if (base != 0 && base != OTA_APP_BASE) {
        if (QMessageBox::question(this, "基址警告",
                QString("这个 hex 的起始地址是 0x%1，不是 App 运行区 0x%2。\n"
                        "刷进去后搬运到 App 区会跑不起来，确定继续？")
                    .arg(base, 8, 16, QLatin1Char('0'))
                    .arg(quint32(OTA_APP_BASE), 8, 16, QLatin1Char('0')).toUpper(),
                QMessageBox::Yes | QMessageBox::No, QMessageBox::No) != QMessageBox::Yes) {
            return;
        }
    }

    /* ★ 大小检查：下位机 OTA_BANK_SIZE = 256KB，超了它会回 ERR_RANGE。
       在上位机提前挡住，比等下位机报错体验好得多。 */
    if (fw.size() > int(OTA_BANK_SIZE)) {
        QMessageBox::warning(this, "错误",
            QString("固件 %1 字节超过 Bank 容量 %2 字节").arg(fw.size()).arg(OTA_BANK_SIZE));
        return;
    }
    if (!isOpen()) { QMessageBox::warning(this, "提示", "请先打开连接"); return; }

    quint32 size   = quint32(fw.size());
    quint32 crc    = Proto::crc32(reinterpret_cast<const quint8 *>(fw.constData()), size);
    int     blocks = int((size + OTA_BLOCK_DATA - 1) / OTA_BLOCK_DATA);
    quint8  bank   = quint8(m_spnBank->value());

    m_otaRunning = true;
    m_btnOta->setEnabled(false);
    m_barOta->setRange(0, blocks);
    m_barOta->setValue(0);

    log("========== 开始 OTA ==========");
    log(QString("固件 %1 字节 / %2 包 / CRC32=0x%3 / Bank=%4")
            .arg(size).arg(blocks)
            .arg(quint32(crc), 8, 16, QLatin1Char('0')).toUpper()
            .arg(bank));

    bool ok = false;

    /* ---------- ② START ---------- */
    {
        QByteArray p;
        Proto::putU32BE(p, size);          /* 总大小 */
        Proto::putU16BE(p, quint16(blocks));/* 总包数 */
        Proto::putU32BE(p, crc);           /* 整包 CRC32 */
        p.append(char(bank));              /* 目标 Bank（第 11 字节，下位机兼容 10/11） */

        Ack a = transact(CMD_UPDATE_START, p, 3000);
        if (!a.ok)      { log("! START 超时"); goto done; }
        if (a.err != ERR_OK) {
            log(QString("! START 被拒，错误码 0x%1 (%2)")
                    .arg(quint32(a.err), 2, 16, QLatin1Char('0'))
                    .arg(errText(a.err)));
            goto done;
        }
        log("START 已接受，等待下位机擦除 Flash...");
    }

    /* ---------- ③ 等擦除完成（这一步必须有！） ---------- */
    if (!waitOtaReady(30000)) goto done;
    log("擦除完成，开始发送固件块。");

    {
        int i          = 0;      /* 当前要发的块号 */
        int resyncLeft = 32;     /* 最多重新对齐 32 次，防止异常时死循环 */

        while (i < blocks) {
            quint32 off   = quint32(i) * OTA_BLOCK_DATA;
            /* mid(off, 128)：最后一块会自动变短，不用特殊处理 */
            QByteArray chunk = fw.mid(int(off), OTA_BLOCK_DATA);

            QByteArray p;
            Proto::putU32BE(p, off);       /* 偏移 */
            p.append(chunk);               /* 固件数据 */

            bool sent = false;
            int  jumpTo = -1;              /* >=0 表示要跳到这块重来 */

            for (int retry = 0; retry < OTA_BLOCK_RETRY_MAX; retry++) {
                /* ★ 超时放宽见文件顶部 OTA_BLOCK_TIMEOUT_MS 的注释（WiFi 下会秒级抖动） */
                Ack a = transact(CMD_UPDATE_BLOCK, p, OTA_BLOCK_TIMEOUT_MS);
                if (a.ok && a.err == ERR_OK) {
                    sent = true;
                    break;
                }
                log(QString("  第 %1 块失败(第 %2 次重试)，err=0x%3")
                        .arg(i).arg(retry + 1)
                        .arg(quint32(a.err), 2, 16, QLatin1Char('0')));

                /* ★ 下位机在 ERR_RANGE 的应答里塞了 4 字节"它实际收到哪了"，
                   拿它当真相重新对齐 —— 这比自己猜靠谱一万倍。 */
                if (a.ok && a.err == ERR_RANGE && a.data.size() >= 4) {
                    quint32 real = Proto::getU32BE(a.data, 0);
                    int     j    = int(real / OTA_BLOCK_DATA);
                    if (j != i) {
                        log(QString("  → 下位机实际已收 %1 字节，改从第 %2 块续发")
                                .arg(real).arg(j));
                        jumpTo = j;
                        break;             /* 跳出重试，用新块号重发 */
                    }
                }
                /* ★ 重试之间多喘一会儿：WiFi 下重发太急会把 ESP8266 缓冲灌满，
                     反而让应答更晚出来（2026-09-28 第 223 块事故）。 */
                sleepMsEventLoop(OTA_BLOCK_RETRY_GAP_MS);
            }

            if (jumpTo >= 0) {             /* 重新对齐 */
                if (--resyncLeft <= 0) {
                    log("! 多次重新对齐仍失败，中止升级");
                    goto done;
                }
                i = jumpTo;
                continue;
            }
            if (!sent) {
                log(QString("! 第 %1 块最终失败，中止升级").arg(i));
                goto done;
            }

            i++;
            m_barOta->setValue(i);
            /* 每 16 块打一次日志，否则 2048 块会把日志刷爆 */
            if ((i % 16) == 0 || i == blocks)
                log(QString("  进度 %1/%2 (%3%)")
                        .arg(i).arg(blocks).arg(i * 100 / blocks));
            /* 让界面有机会刷新（进度条、日志滚动） */
            qApp->processEvents();
        }
    }

    /* ---------- ⑤ FINISH：双方对账 CRC32 ---------- */
    {
        QByteArray p;
        Proto::putU32BE(p, crc);
        Ack a = transact(CMD_UPDATE_FINISH, p, 5000);
        if (!a.ok)      { log("! FINISH 超时"); goto done; }
        if (a.err != ERR_OK) {
            log(QString("! FINISH 失败，错误码 0x%1 (%2)")
                    .arg(quint32(a.err), 2, 16, QLatin1Char('0')).arg(errText(a.err)));
            goto done;
        }
        log("FINISH 通过，CRC32 对账一致。");
    }

    /* ---------- ⑥ 切 Bank：只写标志区 ---------- */
    {
        QByteArray p;
        p.append(char(bank));
        Ack a = transact(CMD_SWITCH_BANK, p, 5000);
        if (!a.ok)      { log("! 切 Bank 超时"); goto done; }
        if (a.err != ERR_OK) {
            log(QString("! 切 Bank 失败，错误码 0x%1").arg(quint32(a.err), 2, 16, QLatin1Char('0')));
            goto done;
        }
        log(QString("已切换启动 Bank = %1，标志区写入完成。").arg(bank));
    }

    ok = true;
    log("========== OTA 完成 ==========");
    /* ★ 关键提示：切 Bank 只是写了个标志，真正搬运动作在 Bootloader 里。
       所以必须复位，Boot 上电看到 upgrade_flag=PENDING 才会把 Bank 搬到 App 区。
       ★ 不要在 ACK 刚发出去就立刻复位 —— 多等 100ms 让串口把 ACK 发完。 */
    sleepMsEventLoop(150);
    QMessageBox::information(this, "升级完成",
        QString("固件已写入 Bank %1，CRC32 校验通过。\n\n"
                "现在请【复位开发板】：\n"
                "Bootloader 上电后会看到升级标志，自动把固件搬运到 App 区并跳转。\n\n"
                "⚠ 搬运期间（约 1~3 秒）请勿断电！").arg(bank));

done:
    if (!ok) log("========== OTA 失败 ==========");
    m_otaRunning = false;
    m_btnOta->setEnabled(true);
}

/* 八、日志 */
void MainWindow::log(const QString &s)
{
    QString t = QDateTime::currentDateTime().toString("hh:mm:ss.zzz");
    m_txtLog->appendPlainText(QString("[%1] %2").arg(t, s));
    /* 自动滚到最后一行 */
    m_txtLog->verticalScrollBar()->setValue(m_txtLog->verticalScrollBar()->maximum());
}

void MainWindow::logTx(const QByteArray &d)
{
    if (m_chkHex && m_chkHex->isChecked())
        log("TX → " + Proto::toHexString(d));
    else
        log(QString("TX → %1 字节").arg(d.size()));
}

void MainWindow::logRx(const QByteArray &d)
{
    if (m_chkHex && m_chkHex->isChecked())
        log("RX ← " + Proto::toHexString(d));
    else
        log(QString("RX ← %1 字节").arg(d.size()));
}

void MainWindow::onClearLog()
{
    m_txtLog->clear();
}

void MainWindow::onSaveLog()
{
    /* 默认文件名带时间戳：ota_log_20260923_233015.txt
       联调时一次一存，出问题好对比"这次和上次差在哪一帧" */
    QString def = QString("ota_log_%1.txt")
                      .arg(QDateTime::currentDateTime().toString("yyyyMMdd_hhmmss"));
    QString path = QFileDialog::getSaveFileName(this, "保存日志", def,
                                                "文本文件 (*.txt);;所有文件 (*.*)");
    if (path.isEmpty()) return;

    QFile f(path);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Text)) {
        QMessageBox::warning(this, "错误", "保存失败：" + f.errorString());
        return;
    }
    f.write(m_txtLog->toPlainText().toUtf8());
    f.close();
    log("日志已保存：" + path);
}
