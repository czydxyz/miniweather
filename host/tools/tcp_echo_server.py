# -*- coding: utf-8 -*-
"""
MiniWeather —— ESP8266 TCP 联调用的最简服务端

干什么用的
----------
板子上的 ESP8266 会主动发起 TCP 连接（AT+CIPSTART="TCP",<IP>,<PORT>），
所以 PC 这一侧必须是【服务端】。
（注意：Qt 上位机那个 TCP 模式是"客户端"，连不进来，只能拿它去连别人。）

怎么跑
------
1. 电脑开移动热点（频段改 2.4GHz），板子连这个热点
2. ipconfig 看热点网卡的 IP（Windows 移动热点默认 192.168.137.1）
3. 确认 BSP/Inc/esp8266.h 里的 TCP_SERVER_IP / TCP_SERVER_PORT 和这里一致
4. 运行：
       python tcp_echo_server.py                # 监听 0.0.0.0:8888
       python tcp_echo_server.py 192.168.137.1 9000
5. 防火墙会弹窗 -> 允许访问（专用网络）

看到什么算通
------------
    [CONN] 192.168.137.xxx:54321 已连接
    [RECV] 12 字节
      48 65 6C 6C 6F ...   |Hello...|

按键：Ctrl+C 退出
"""

import sys
import socket
import threading
import time

DEFAULT_HOST = "0.0.0.0"   # 0.0.0.0 = 监听本机所有网卡，省得查 IP
DEFAULT_PORT = 8888


def hexdump(data: bytes, width: int = 16) -> str:
    """把一段字节变成 "HEX  |ASCII|" 的可读形式"""
    out = []
    for i in range(0, len(data), width):
        chunk = data[i:i + width]
        hex_part = " ".join("%02X" % b for b in chunk)
        # 不可打印字符统一显示成 '.'，否则中文/二进制会把终端刷乱
        ascii_part = "".join(chr(b) if 32 <= b < 127 else "." for b in chunk)
        out.append("      %-47s  |%s|" % (hex_part, ascii_part))
    return "\n".join(out)


def handle(conn: socket.socket, addr):
    print("[CONN] %s:%d 已连接" % (addr[0], addr[1]))
    conn.settimeout(None)
    try:
        while True:
            data = conn.recv(1024)
            if not data:
                print("[DISC] %s:%d 断开（对端主动关闭）" % (addr[0], addr[1]))
                break
            print("[RECV] %d 字节 @ %s" % (len(data), time.strftime("%H:%M:%S")))
            print(hexdump(data))
            # 原样回一个 ACK，方便验证"下行"方向也是通的
            try:
                conn.sendall(b"ACK:%d\r\n" % len(data))
            except OSError:
                break
    except (ConnectionResetError, OSError) as e:
        print("[ERR ] %s:%d -> %s" % (addr[0], addr[1], e))
    finally:
        try:
            conn.close()
        except OSError:
            pass


def main():
    host = sys.argv[1] if len(sys.argv) > 1 else DEFAULT_HOST
    port = int(sys.argv[2]) if len(sys.argv) > 2 else DEFAULT_PORT

    srv = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    # SO_REUSEADDR：上次的连接还在 TIME_WAIT 时也能立刻重新绑定端口
    srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    srv.bind((host, port))
    srv.listen(5)

    print("=" * 56)
    print("MiniWeather TCP 测试服务端")
    print("=" * 56)
    print("监听 %s:%d" % (host, port))
    print("等待 ESP8266 连接中……（Ctrl+C 退出）")
    print("提示：连不上先查 PC 防火墙是否放行该端口")

    try:
        while True:
            conn, addr = srv.accept()
            # 每条连接开一个线程，方便同时看多个客户端
            threading.Thread(target=handle, args=(conn, addr), daemon=True).start()
    except KeyboardInterrupt:
        print("\n[EXIT] 已退出")
    finally:
        srv.close()


if __name__ == "__main__":
    main()
