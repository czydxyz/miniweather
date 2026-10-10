# -*- coding: utf-8 -*-
"""
MiniWeather —— AI 顾问 TCP 服务端（板子经 WiFi 上报 → 大模型 → 建议回传 LCD）

和 tcp_echo_server.py 的区别
----------------------------
那个只是原样回 ACK，用来证明链路通；
这个是【真正干活的】：

    板子 TaskSensor 每 60 秒  --TCP-->  本服务端
        {"temp":26.5,"humi":57.9,"press":97953,"co2eq":400,"tvoc":0}\r\n
                                            |
                                            v
                                     DeepSeek / 规则引擎
                                            |
    板子 LCD 滚动显示建议      <--TCP--   "Comfortable: temp 26.5 C, humidity 58%."\r\n

链路位置：STM32 --USART3--> ESP8266 --WiFi(2.4G)--> 本服务端

怎么跑
------
1. 电脑开移动热点，【频段必须 2.4GHz】（ESP8266 连不了 5G，会报 +CWJAP:3 扫不到）
2. 管理员 PowerShell 放行端口（热点算"公用"网络，默认拦入站）：
       New-NetFirewallRule -DisplayName "MiniWeather TCP 8888" -Direction Inbound `
                           -Protocol TCP -LocalPort 8888 -Action Allow -Profile Any
3. python ai_tcp_server.py                # 监听 0.0.0.0:8888
   python ai_tcp_server.py 192.168.137.1 9000

★ LLM 配置在同目录 ai_advisor_config.json（api_key / base_url / model）。
  没 key 或断网会自动降级到本地规则引擎，功能不消失（src=rule）。
  先用 `python ai_advisor.py --selftest` 单独验证 LLM 是否可用，再接板子。

看到什么算通
------------
    [CONN] 192.168.137.220:53657 已连接
    [JSON ] {"temp":26.5,...}
    [AI   ][llm][1.8s] Comfortable: temp 26.5 C, humidity 58%.
    [SEND ] Comfortable: temp 26.5 C, humidity 58%.
（板子串口助手这边会打印 [AI    ] 同样的内容，LCD 上开始滚动显示）
"""

import json
import os
import sys
import socket
import threading
import time

# ai_advisor.py 与本脚本同目录：把它加进模块搜索路径，
# 这样 get_advice()（含 LLM 调用 + 规则引擎降级 + ASCII 收敛）可以整个复用，
# 不用在两份脚本里维护两套提示词和两套降级逻辑。
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from ai_advisor import get_advice, cfg_get, MAX_ADVICE_LEN   # noqa: E402

DEFAULT_HOST = "0.0.0.0"
DEFAULT_PORT = 8888

# 协议帧头（与下位机 protocol.c 一致）：AA 55 | cmd(2B) | len(2B) | data | CRC16(2B)
FRAME_HEAD = b"\xAA\x55"


def hexdump(data: bytes, width: int = 16) -> str:
    """把一段字节变成 "HEX  |ASCII|" 的可读形式"""
    out = []
    for i in range(0, len(data), width):
        chunk = data[i:i + width]
        hex_part = " ".join("%02X" % b for b in chunk)
        ascii_part = "".join(chr(b) if 32 <= b < 127 else "." for b in chunk)
        out.append("      %-47s  |%s|" % (hex_part, ascii_part))
    return "\n".join(out)


def parse_frame(buf: bytearray):
    """尝试从缓冲头部切出一帧完整的自定义协议帧。

    返回 (frame_bytes, consumed)；不成帧返回 (None, 0)。

    ★ 为什么要自己切成帧而不是"来多少收多少"：
      TCP 是字节流，没有消息边界 —— 一帧 22 字节可能分两次到达，
      也可能两条消息粘在一个包里。不按长度字段切，就会把半帧当整帧处理。
    """
    if len(buf) < 2 or bytes(buf[:2]) != FRAME_HEAD:
        return None, 0
    if len(buf) < 6:
        return None, 0                       # 长度字段还没到齐
    n = (buf[4] << 8) | buf[5]               # 长度字段（大端）
    if n > 256:                              # 协议规定 N<=256，超了说明错位了
        return None, -1                      # -1 = 让调用方丢掉头部一个字节重新找帧头
    total = 8 + n
    if len(buf) < total:
        return None, 0                       # 还没收完，继续等
    return bytes(buf[:total]), total


def handle(conn: socket.socket, addr):
    print("[CONN] %s:%d 已连接" % (addr[0], addr[1]))
    conn.settimeout(None)
    buf = bytearray()

    try:
        while True:
            data = conn.recv(1024)
            if not data:
                print("[DISC] %s:%d 断开（对端主动关闭）" % (addr[0], addr[1]))
                break
            buf += data

            # ---------- ① 先抽协议帧（AA 55 开头） ----------
            while True:
                frame, used = parse_frame(buf)
                if used == -1:               # 帧头错位：丢一个字节重找
                    del buf[:1]
                    continue
                if frame is None:
                    break
                del buf[:used]
                cmd = (frame[2] << 8) | frame[3]
                print("[FRAME] cmd=0x%04X len=%d" % (cmd, len(frame) - 8))
                print(hexdump(frame))
                # 协议帧由下位机自己组应答。
                # ★ 2026-09-28：这行纯 ASCII 的 ACK 回执当初是为了验证"下行方向通不通"，
                #   但板侧会把任何非协议帧的 ASCII 行当成【AI 建议文本】收走，
                #   于是 LCD 上显示 "ACK:15" 而不是大模型的建议，
                #   真正的建议要等到下一次 60 秒上报才覆盖掉它。
                #   下行链路已经验证过了，这里关掉。要再验链路时把下面两行放开即可。
                # try:
                #     conn.sendall(b"ACK:%d\r\n" % len(frame))
                # except OSError:
                #     break

            # ---------- ② 再按行抽文本（JSON 上报 / 板子日志） ----------
            while b"\n" in buf:
                raw, _, rest = bytes(buf).partition(b"\n")
                buf = bytearray(rest)
                text = raw.decode("utf-8", "ignore").strip()
                if not text:
                    continue

                if not text.startswith("{"):
                    # 板子 printf 出来的日志（透传态下也会顺着 TCP 飘过来）
                    print("[LOG  ] %s" % text[:200])
                    continue

                print("[JSON ] %s" % text[:200])
                try:
                    d = json.loads(text)
                except ValueError:
                    print("[WARN ] 不是合法 JSON，忽略")
                    continue

                t0 = time.time()
                advice, src = get_advice(d)
                print("[AI   ][%s][%.1fs] %s" % (src, time.time() - t0, advice))

                # ★ 回传纯 ASCII 一行，以 \r\n 结尾。
                #   下位机 AI_CollectText() 以 '\n' 作为行结束标志，
                #   没有行尾它会一直等 —— 这个 '\n' 不能省。
                try:
                    conn.sendall((advice + "\r\n").encode("ascii", "ignore"))
                    print("[SEND ] %s" % advice)
                except OSError:
                    break

            # ---------- ③ 兜底：缓冲里堆了既不成帧也不成行的垃圾 ----------
            if len(buf) > 4096 and (b"\n" not in buf) and (bytes(buf[:2]) != FRAME_HEAD):
                print("[WARN ] 缓冲堆积 %d 字节且无帧头/换行，清空防失控" % len(buf))
                buf.clear()

    except (ConnectionResetError, OSError) as e:
        print("[ERR  ] %s:%d -> %s" % (addr[0], addr[1], e))
    finally:
        try:
            conn.close()
        except OSError:
            pass


def main():
    host = sys.argv[1] if len(sys.argv) > 1 else DEFAULT_HOST
    port = int(sys.argv[2]) if len(sys.argv) > 2 else DEFAULT_PORT

    srv = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    srv.bind((host, port))
    srv.listen(5)

    ak = cfg_get("OPENAI_API_KEY", "api_key", "")
    print("=" * 60)
    print("MiniWeather AI 顾问 TCP 服务端")
    print("=" * 60)
    print("监听      : %s:%d" % (host, port))
    print("大模型    : %s (model=%s, base=%s)" % (
        "enabled" if ak else "disabled -> 规则引擎兜底",
        cfg_get("OPENAI_MODEL", "model", "deepseek-chat"),
        cfg_get("OPENAI_BASE_URL", "base_url", "https://api.deepseek.com/v1")))
    print("回传限长  : %d 字符（LCD 一行只能放 30 个字符，板侧会横向滚动）"
          % MAX_ADVICE_LEN)
    print("等待 ESP8266 连接中……（Ctrl+C 退出）")
    print("提示：连不上先查 ①热点是 2.4GHz ②防火墙放行该端口")

    try:
        while True:
            conn, addr = srv.accept()
            threading.Thread(target=handle, args=(conn, addr), daemon=True).start()
    except KeyboardInterrupt:
        print("\n[EXIT ] 已退出")
    finally:
        srv.close()


if __name__ == "__main__":
    main()
