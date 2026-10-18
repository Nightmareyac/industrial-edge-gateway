# -*- coding: utf-8 -*-
"""
ESP8266 启动诊断抓包器
用途：在模块上电的瞬间抓取原始字节，判断模块是"没启动"、"进了下载模式"还是"正常跑了固件"。

判据：
  74880 出现 "boot mode:(1,x)"  -> UART 下载模式（可以烧录）
  74880 出现 "boot mode:(3,x)"  -> Flash 启动模式（GPIO0 悬空，正常跑固件）
  74880 出现 "boot mode:(2,x)"  -> 跳线启动（少见）
  两个波特率都 0 字节          -> 模块没上电 / CH_PD 没拉高 / TX 线没通 / 供电不足

用法：
  python esp_probe.py COM7
"""

import sys
import time

try:
    import serial
except ImportError:
    print("缺少 pyserial")
    sys.exit(1)

PORT = sys.argv[1] if len(sys.argv) > 1 else "COM7"


def capture(baud, seconds, label):
    print("\n--- %s @ %d bps ---" % (label, baud))
    try:
        sp = serial.Serial(PORT, baud, timeout=0.2)
    except Exception as exc:
        print("  打开失败: %s" % exc)
        return b""

    time.sleep(0.2)
    sp.reset_input_buffer()

    buf = bytearray()
    t0 = time.time()
    while time.time() - t0 < seconds:
        chunk = sp.read(512)
        if chunk:
            buf += chunk

    sp.close()

    if not buf:
        print("  (收到 0 字节)")
        return b""

    print("  共 %d 字节：" % len(buf))
    for off in range(0, min(len(buf), 320), 16):
        part = buf[off:off + 16]
        hx = " ".join("%02X" % b for b in part)
        asc = "".join(chr(b) if 32 <= b < 127 else "." for b in part)
        print("  %04X  %-47s  %s" % (off, hx, asc))
    return bytes(buf)


def main():
    print("=" * 60)
    print("ESP8266 启动抓包  port = %s" % PORT)
    print("=" * 60)

    print("\n[第 1 步] 先看模块当前在说什么（115200，正常固件会打印 ready）")
    print("  请保持当前状态，不要动线")
    input("  按回车开始 >> ")
    a = capture(115200, 3, "当前状态")

    print("\n[第 2 步] 抓上电瞬间的 boot log（74880，这是 ESP8266 固定波特率）")
    print("  现在请做：模块断电 -> 确认 GPIO0 接地（烧录模式）-> 重新上电")
    input("  按回车后立刻给模块上电 >> ")
    b = capture(74880, 8, "上电瞬间")

    print("\n" + "=" * 60)
    print("判读结果")
    print("=" * 60)

    if not a and not b:
        print("  两个波特率都是 0 字节 -> 模块完全没输出")
        print("  ├─ 供电不足（最常见）：量 VCC 上电瞬间是否 >=3.0V")
        print("  ├─ CH_PD 没接 3.3V")
        print("  └─ 模块 TXD 没接到 USB-TTL 的 RXD")
        return

    if b:
        text = b.decode("latin-1", "ignore")
        if "boot mode:(1" in text:
            print("  >> 检测到 boot mode:(1,..) = UART 下载模式")
            print("     模块已就绪，可以直接烧录：")
            print('     esptool --port %s --baud 115200 --before no-reset --after no-reset write-flash 0x0 "<bin路径>"' % PORT)
        elif "boot mode:(3" in text:
            print("  >> 检测到 boot mode:(3,..) = Flash 启动模式")
            print("     GPIO0 没接地，或接地在上电之后才做。请看得到 boot log 说明线路正常，")
            print("     只需按【断电 -> GPIO0 接 GND -> 上电】重来一次。")
        else:
            print("  >> 收到数据但没匹配到 boot mode 关键字，原始内容见上表")

    if a:
        print("  >> 115200 下收到了数据：模块固件在运行（可能已打印 ready）")

    print("\n判读完成。")


if __name__ == "__main__":
    main()
