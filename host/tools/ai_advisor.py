#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
微气候 AI 顾问 —— 串口直连版（PC 侧服务 + 大模型）

★ 本版是"串口模式"：PC 通过板载 CH340（USART1，PA9/PA10）直连开发板，
  不需要 ESP8266、不需要 WiFi、不需要 TCP 服务器。

数据流向：
    板子 printf/日志  ──USART1 TX──▶  PC（本脚本）── 只当日志打印
    板子传感器 JSON   ──USART1 TX──▶  PC（本脚本）──▶ 大模型 ──┐
    LCD 显示          ◀──USART1 RX──  PC（本脚本）◀────────────┘

为什么改成串口（而不是原来的 TCP）：
    ESP8266 那一路现在还在联调，而"AI 顾问"这个功能是题目里明确要求的，
    不能因为网络没通就整个缺失。CH340 那一路是板子出厂就通的调试口，
    先拿它把 "采样 → 大模型 → 建议回传显示" 这条链路跑通，
    等 ESP8266 通了，把下面 AI_Report() 的发送函数换回 ESP8266_SendData 即可，
    协议一行都不用改（上行 JSON / 下行 ASCII 文本）。

为什么大模型放 PC 而不放板子：
    STM32 只有 1MB Flash / 192KB SRAM，跑不动任何大模型；
    而"把读数翻译成人话"这件事对实时性没要求，放 PC 最合理。
    PC 侧拿不到大模型（没 Key / 断网）时自动降级到内置规则引擎，
    板子断网时还有本地阈值兜底 —— 三层降级，任何一层挂了功能都不消失。

用法：
    python ai_advisor.py                    # 自动扫描并列出串口后退出
    python ai_advisor.py COM6               # 115200（默认）
    python ai_advisor.py COM6 115200

★ API 在哪配（三选一，优先级从高到低）：
    ① 配置文件（推荐，改一次永久生效）：
       编辑本脚本同目录下的 ai_advisor_config.json
    ② 环境变量（临时生效，关掉终端就没了）：
       set OPENAI_API_KEY=sk-xxxxxx
       set OPENAI_BASE_URL=https://api.xxx.com/v1
       set OPENAI_MODEL=qwen-plus
    ③ 什么都不配：自动降级到规则引擎，功能照样跑

★ 没有 API Key 也能跑：自动降级到规则引擎，答辩现场断网照样能演示整条链路。
"""

import json
import os
import sys
import time
import urllib.error
import urllib.request

DEFAULT_BAUD = 115200

# LCD 只有 ASCII 字模（asc2_1608[95][16]），回传建议必须纯 ASCII，且长度受限
MAX_ADVICE_LEN = 120

# 配置文件：与本脚本同目录。不存在就忽略（全部走环境变量/默认值）
CONFIG_FILE = os.path.join(os.path.dirname(os.path.abspath(__file__)),
                           "ai_advisor_config.json")


def load_config():
    """读配置文件。读不到就返回空字典 —— 配置缺失不是错误，降级即可。"""
    try:
        with open(CONFIG_FILE, "r", encoding="utf-8") as f:
            cfg = json.load(f)
        return cfg if isinstance(cfg, dict) else {}
    except FileNotFoundError:
        return {}
    except Exception as e:
        print("[WARN ] 配置文件读不了（%s），忽略" % e)
        return {}


# 优先级：环境变量 > 配置文件 > 默认值
CFG = load_config()


def cfg_get(env_name, cfg_key, default=""):
    v = os.environ.get(env_name, "").strip()
    if v:
        return v
    v = str(CFG.get(cfg_key, "")).strip()
    return v if v else default


# ---------------------------------------------------------------------------
# 0) 串口工具
# ---------------------------------------------------------------------------
def list_ports():
    """列出本机所有串口，方便用户确认 COM 号。"""
    try:
        import serial.tools.list_ports as lp
        return [(p.device, p.description or "") for p in lp.comports()]
    except Exception as e:
        print("[WARN ] 扫描串口失败: %s" % e)
        return []


def open_serial(port, baud):
    import serial
    s = serial.Serial(port, baud, timeout=0.2, write_timeout=1)
    # ★ DTR/RTS 拉低会让某些 CH340 板子复位，这里显式放开
    try:
        s.dtr = False
        s.rts = False
    except Exception:
        pass
    s.reset_input_buffer()
    return s


# ---------------------------------------------------------------------------
# 1) 大模型调用
# ---------------------------------------------------------------------------
def build_prompt(d):
    """把采样数据组织成给大模型的提示词。

    提示词三件事必须说清楚：角色 / 数据 / 输出约束。
    尤其是【输出约束】—— 必须限定纯 ASCII + 长度，否则模型回一段中文，
    板子的 LCD 没有中文字模，显示出来就是一屏乱码块。
    """
    return (
        "You are a microclimate advisor for a dorm room / small office.\n"
        "Given the current sensor readings, give ONE short, actionable suggestion.\n"
        "Rules: ASCII only. Max %d characters. No emoji. Start with an action verb.\n"
        "Normal ranges: 18-28 C, 30-70%% RH, CO2eq < 1000 ppm, TVOC < 300 ppb.\n"
        "If every reading is inside the normal ranges, just say it is comfortable.\n"
        "\n"
        "Readings:\n"
        "  temperature = %.1f C\n"
        "  humidity    = %.1f %%\n"
        "  pressure    = %d Pa\n"
        "  CO2eq       = %d ppm\n"
        "  TVOC        = %d ppb\n"
        "\n"
        "Answer:"
    ) % (
        MAX_ADVICE_LEN,
        d.get("temp", 0.0),
        d.get("humi", 0.0),
        int(d.get("press", 0)),
        int(d.get("co2eq", 0)),
        int(d.get("tvoc", 0)),
    )


def call_llm(prompt):
    """调用 OpenAI 兼容的 chat/completions 接口。失败时抛异常，由上层降级。"""
    api_key = cfg_get("OPENAI_API_KEY", "api_key", "")
    if not api_key:
        raise RuntimeError("no OPENAI_API_KEY (填 ai_advisor_config.json 或设环境变量)")

    base_url = cfg_get("OPENAI_BASE_URL", "base_url",
                       "https://api.openai.com/v1").rstrip("/")
    model = cfg_get("OPENAI_MODEL", "model", "gpt-4o-mini")

    body = {
        "model": model,
        "messages": [{"role": "user", "content": prompt}],
        "temperature": 0.3,          # 建议类输出，温度调低，避免每次说法差太多
        "max_tokens": 96,
    }
    req = urllib.request.Request(
        base_url + "/chat/completions",
        data=json.dumps(body).encode("utf-8"),
        headers={
            "Content-Type": "application/json",
            "Authorization": "Bearer " + api_key,
        },
        method="POST",
    )
    with urllib.request.urlopen(req, timeout=15) as resp:
        data = json.loads(resp.read().decode("utf-8"))
    return data["choices"][0]["message"]["content"].strip()


# ---------------------------------------------------------------------------
# 2) 降级方案：本地规则引擎
# ---------------------------------------------------------------------------
def rule_engine(d):
    """没有大模型时的兜底逻辑。

    这一段存在的意义：让"云端不可达"≠"系统没有智能"。
    它本身就是一套可解释的专家规则，答辩时能说清楚依据。
    阈值与板侧 AI_LocalAdvice() 完全一致，保证两端结论方向不打架。
    """
    t = d.get("temp", 0.0)
    h = d.get("humi", 0.0)
    co2 = int(d.get("co2eq", 0))
    tvoc = int(d.get("tvoc", 0))

    if co2 > 1000:
        return "Open windows now: CO2eq %d ppm is high." % co2
    if tvoc > 300:
        return "Ventilate room: TVOC %d ppb exceeds limit." % tvoc
    if t > 28.0 and h > 70.0:
        return "Hot and humid: start fan or dehumidifier."
    if t < 18.0:
        return "Room is cold (%.1f C): consider heating." % t
    if h < 30.0:
        return "Air is dry (%.0f%%): use a humidifier." % h
    return "Comfortable: temp %.1f C, humidity %.0f%%." % (t, h)


def sanitize(text):
    """强制收敛成板子能显示的形式：纯 ASCII、单行、限长。"""
    text = " ".join(text.split())
    text = text.encode("ascii", "ignore").decode("ascii")
    return text[:MAX_ADVICE_LEN]


def get_advice(d):
    """先试大模型，失败降级到规则引擎。调用方永远拿得到一个字符串。"""
    try:
        return sanitize(call_llm(build_prompt(d))), "llm"
    except (urllib.error.URLError, urllib.error.HTTPError, KeyError, RuntimeError, OSError) as e:
        print("[LLM  ] unavailable (%s) -> fallback to rules" % e)
        msg = str(e)
        # 常见踩坑：填了 key 但 base_url 是官方地址，国内直连不通
        if "Tunnel" in msg or "502" in msg or "timed out" in msg or "Name or service" in msg:
            print("[HINT ] 地址不可达。国内请换 OpenAI 兼容端点，例如 "
                  "base_url=https://api.deepseek.com/v1 / model=deepseek-chat")
        return sanitize(rule_engine(d)), "rule"


# ---------------------------------------------------------------------------
# 3) 主循环：从串口读，往串口写
# ---------------------------------------------------------------------------
def selftest():
    """不接板子的自检：把三组样本直接喂进"提示词→大模型→sanitize"这条链路。

    用途：先确认 API 配对了、模型能回话、输出是纯 ASCII 且没超长，
    再去接板子。这样出问题能立刻分清是"PC 侧"还是"物理链路"。
    """
    samples = [
        {"temp": 27.9, "humi": 71.4, "press": 100832, "co2eq": 1180, "tvoc": 320},
        {"temp": 24.3, "humi": 52.0, "press": 101120, "co2eq": 640,  "tvoc": 90},
        {"temp": 22.1, "humi": 26.5, "press": 100980, "co2eq": 520,  "tvoc": 60},
    ]
    print("[TEST ] 自检模式（不接板子）")
    for i, d in enumerate(samples, 1):
        advice, src = get_advice(d)
        ok_ascii = all(ord(c) < 128 for c in advice)
        ok_len = len(advice) <= MAX_ADVICE_LEN
        print("[TEST ] #%d src=%-4s ascii=%s len=%d/%d %s"
              % (i, src, "OK" if ok_ascii else "BAD",
                 len(advice), MAX_ADVICE_LEN, "OK" if ok_len else "TOO LONG"))
        print("        -> %s" % advice)
    print("[TEST ] done\n")


def loopback(port, baud):
    """串口回环自检：把 CH340 的 TX 与 RX 用杜邦线短接，脚本自己发、自己收。

    能验证什么：COM 号对不对、串口能不能收发、整条"收 JSON → 问模型 → 回文本"
    流水线通不通 —— 全程不需要板子跑 ai_client 固件。
    判据：打印出 [LOOP ] recv 且内容是建议文本 = 物理层 + 逻辑层都 OK。
    """
    try:
        ser = open_serial(port, baud)
    except Exception as e:
        print("[FAIL ] 打开 %s 失败: %s" % (port, e))
        return

    print("[LOOP ] %s @ %d —— 请把 TX/RX 短接。Ctrl+C 退出" % (port, baud))
    sample = {"temp": 27.9, "humi": 71.4, "press": 100832, "co2eq": 1180, "tvoc": 320}
    line = json.dumps(sample) + "\r\n"
    try:
        ser.write(line.encode("ascii"))
        print("[LOOP ] sent: %s" % line.strip())
        waited = 0.0
        while waited < 25.0:
            raw = ser.readline()
            if raw:
                print("[LOOP ] recv: %s" % raw.decode("utf-8", "ignore").strip())
                break
            waited += 0.2
        else:
            print("[LOOP ] 25s 没收到回环 —— TX/RX 短接了吗？COM 号对吗？")
    except KeyboardInterrupt:
        print("\n[LOOP ] 退出")
    finally:
        ser.close()


def main():
    # python ai_advisor.py --selftest ：不接板子的自检
    if len(sys.argv) > 1 and sys.argv[1] in ("--selftest", "-t", "test"):
        selftest()
        return

    # python ai_advisor.py --loopback COM6 ：串口回环自检（TX/RX 短接）
    if len(sys.argv) > 1 and sys.argv[1] == "--loopback":
        if len(sys.argv) < 3:
            print("用法: python ai_advisor.py --loopback COM6")
            for dev, desc in list_ports():
                print("   %-8s %s" % (dev, desc))
            return
        lb = int(sys.argv[3]) if len(sys.argv) > 3 else int(CFG.get("baud", DEFAULT_BAUD))
        loopback(sys.argv[2], lb)
        return

    # 串口号/波特率也能写进配置文件，省得每次敲
    port = sys.argv[1] if len(sys.argv) > 1 else str(CFG.get("port", "")).strip()
    if not port:
        print("用法: python ai_advisor.py <COM口> [波特率]")
        print("       python ai_advisor.py COM6")
        print("       python ai_advisor.py COM6 115200\n")
        print("本机可用串口：")
        for dev, desc in list_ports():
            print("   %-8s %s" % (dev, desc))
        if not list_ports():
            print("   （没扫到，确认 CH340 已插上 / 装了驱动）")
        return

    baud = int(sys.argv[2]) if len(sys.argv) > 2 else int(CFG.get("baud", DEFAULT_BAUD))

    try:
        ser = open_serial(port, baud)
    except Exception as e:
        print("[FAIL ] 打开 %s 失败: %s" % (port, e))
        print("        被占用？串口助手还开着？COM 号对不对？")
        return

    ak = cfg_get("OPENAI_API_KEY", "api_key", "")
    print("[INIT ] AI advisor (serial) on %s @ %d" % (port, baud))
    print("[INIT ] LLM: %s (model=%s, base=%s)" % (
        "enabled" if ak else "disabled(rules only)",
        cfg_get("OPENAI_MODEL", "model", "gpt-4o-mini"),
        cfg_get("OPENAI_BASE_URL", "base_url", "https://api.openai.com/v1")))
    print("[INIT ] 等待板子上报 JSON ... 板子日志以 [LOG ] 打印\n")

    try:
        while True:
            raw = ser.readline()                       # 板子每条都以 \r\n 结尾
            if not raw:
                continue

            text = raw.decode("utf-8", "ignore").strip()
            if not text:
                continue

            # ★ 只认以 '{' 开头的行为采样数据；其余全是板子 printf 日志。
            #   这样"日志"和"数据"共用一根串口也不会互相干扰。
            if not text.startswith("{"):
                print("[LOG  ] %s" % text[:200])
                continue

            print("[RECV ] %s" % text[:200])
            try:
                d = json.loads(text)
            except ValueError:
                print("[WARN ] 不是合法 JSON，忽略")
                continue

            t0 = time.time()
            advice, src = get_advice(d)
            print("[ADVICE][%s][%.1fs] %s" % (src, time.time() - t0, advice))

            # 回传纯文本建议，板子直接送 LCD 显示
            try:
                ser.write((advice + "\r\n").encode("ascii", "ignore"))
            except Exception as e:
                print("[WARN ] 写串口失败: %s" % e)

    except KeyboardInterrupt:
        print("\n[EXIT ] bye")
    finally:
        ser.close()


if __name__ == "__main__":
    main()
