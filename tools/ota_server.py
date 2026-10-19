#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
OTA 固件 HTTP 服务（常驻版）
=====================================================================
替代临时用的 `python -m http.server 80`。为什么要单独写一个：

  1. 目录/端口/日志路径全部固定，不依赖"在哪个目录下敲的命令"。
     `python -m http.server` 把当前工作目录当根目录，换个地方启动就 404。

  2. 访问日志同时写文件。临时服务的日志只存在于那个黑窗口里，
     窗口一关、会话一结束，OTA 失败时想回查"服务器到底收到请求没有"
     就完全没证据了 —— 上一轮排查 ota3/ota4 全靠这个日志。

  3. 提供 /health 端点。一条命令确认"服务活着 + 固件版本 + 文件 md5"，
     不用再靠 tasklist / netstat 猜。

  4. 客户端中途断开（ESP 重连、CRC 失败重试）时不打印 traceback。
     默认的 http.server 每次连接被 reset 都会甩一屏堆栈，把真正的请求行冲掉。

用法：
    python ota_server.py --port 80 --root D:\\A_Project\\ota_release

常驻方式见同目录 install_ota_service.cmd（注册成开机自启的计划任务）。
=====================================================================
"""

import argparse
import hashlib
import json
import os
import socket
import sys
import time
import urllib.request
from datetime import datetime
from http.server import SimpleHTTPRequestHandler, ThreadingHTTPServer

# 全局配置，main() 里根据命令行参数填充
ROOT = "."
PORT = 80
BIND = "0.0.0.0"
LOG_FILE = None
START_TS = time.time()

# 日志超过 5MB 就轮转一份 .1，避免长期运行把盘写满
MAX_LOG_BYTES = 5 * 1024 * 1024


def _now():
    return datetime.now().strftime("%Y-%m-%d %H:%M:%S")


def log_line(text):
    """一条日志同时进控制台和文件。文件刷盘，被杀进程时也不丢。"""
    line = "%s  %s" % (_now(), text)

    if LOG_FILE:
        try:
            # 简单轮转：主日志超限就改名成 .log.1，旧的直接覆盖
            if os.path.exists(LOG_FILE) and os.path.getsize(LOG_FILE) > MAX_LOG_BYTES:
                bak = LOG_FILE + ".1"
                if os.path.exists(bak):
                    os.remove(bak)
                os.replace(LOG_FILE, bak)
            with open(LOG_FILE, "a", encoding="utf-8") as f:
                f.write(line + "\n")
        except OSError:
            pass    # 日志写不了不能拖垮服务本身

    # 写控制台。必须容错：作为计划任务用 pythonw.exe 启动时，
    # sys.stdout 是 None，print() 会抛 AttributeError 把服务直接打崩。
    # （踩过一次：任务状态显示"正在运行"，但 80 端口没有任何监听）
    if sys.stdout is not None:
        try:
            print(line, flush=True)
        except (OSError, AttributeError, ValueError):
            pass


def md5_of(path):
    """算固件 md5，用于 /health 核对"服务器上这份是不是我要刷的那份"。"""
    h = hashlib.md5()
    try:
        with open(path, "rb") as f:
            for chunk in iter(lambda: f.read(256 * 1024), b""):
                h.update(chunk)
    except OSError:
        return "-"
    return h.hexdigest()


class OTAHandler(SimpleHTTPRequestHandler):
    """在标准静态文件服务上加：/health 端点 + 结构化日志。"""

    server_version = "OTA-Firmware-Server/1.0"

    def __init__(self, *args, **kwargs):
        # directory 固定成配置的固件目录，不受启动时工作目录影响
        super().__init__(*args, directory=ROOT, **kwargs)

    # ---------- 请求处理 ----------

    def do_GET(self):
        # _t0 = 起始时刻，用于算耗时；_status/_size 在发送过程中填
        self._t0 = time.time()
        self._status = 0
        self._size = 0
        self._aborted = False

        try:
            if self.path.split("?")[0] == "/health":
                self._send_health()
            else:
                # 其余全部交给标准实现：查文件、发 200 + Content-Length、发正文
                super().do_GET()
        except (BrokenPipeError, ConnectionResetError, ConnectionAbortedError):
            # 客户端中途断开：照样记一行，但要标出来，
            # 否则日志里看着像"200 正常完成"，实际固件只传了一半
            self._aborted = True
            raise
        finally:
            # 统一在这里记一行，此时状态码和字节数都已经确定
            self.log_request(self._status or "-",
                             self._size if self._size else "-")

    def send_response(self, code, message=None):
        """覆盖标准实现。

        标准实现会在**发响应头之前**就调用 log_request(code)，
        那时候 Content-Length 还没添加，日志里的字节数永远是 "-"，
        用它判断"固件到底传了多少"就没意义了。
        这里只把状态码存下来，不打印；真正的日志在 do_GET 的 finally 里打。
        """
        self._status = code
        self.send_response_only(code, message)
        self.send_header("Server", self.version_string())
        self.send_header("Date", self.date_time_string())

    def send_header(self, keyword, value):
        # 顺手把 Content-Length 记下来，作为本次响应的字节数
        if keyword.lower() == "content-length":
            try:
                self._size = int(value)
            except (TypeError, ValueError):
                pass
        super().send_header(keyword, value)

    def _send_health(self):
        items = []
        try:
            for name in sorted(os.listdir(ROOT)):
                if not name.lower().endswith(".bin"):
                    continue
                full = os.path.join(ROOT, name)
                items.append({
                    "name": name,
                    "size": os.path.getsize(full),
                    "md5": md5_of(full),
                    "mtime": datetime.fromtimestamp(
                        os.path.getmtime(full)).strftime("%Y-%m-%d %H:%M:%S"),
                })
        except OSError as e:
            log_line("读取固件目录失败：%s" % e)

        body = json.dumps({
            "status": "ok",
            "server": "ota-firmware-server",
            "root": ROOT,
            "listen": "%s:%d" % (BIND, PORT),
            "uptime_s": int(time.time() - START_TS),
            "firmwares": items,
        }, ensure_ascii=False, indent=2).encode("utf-8")

        self.send_response(200)
        self.send_header("Content-Type", "application/json; charset=utf-8")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)
        # 不再单独调用 log_request：do_GET 的 finally 会统一记录一行

    # ---------- 日志 ----------

    def log_request(self, code="-", size="-"):
        """每个请求一行：客户端 / 方法 / 路径 / 状态码 / 字节数 / 耗时。

        ESP 端发的是 GET /A_Project_v3.0.bin HTTP/1.1，
        所以"有没有这行"就是"设备的 GET 到底过没过网"的铁证；
        行末的字节数则直接对应"固件传了多少"，206884 才算完整。
        """
        dt = (time.time() - getattr(self, "_t0", time.time())) * 1000.0
        ip = self.client_address[0] if self.client_address else "-"

        # 客户端提前断开时补一个后缀，避免看起来像正常完成
        tail = "   <-- 客户端提前断开" if getattr(self, "_aborted", False) else ""

        log_line("%-15s %-4s %-32s -> %-4s %8sB %6.0fms%s"
                 % (ip, self.command or "-", self.path[:32], code, size, dt, tail))

    def log_message(self, fmt, *args):
        """标准实现会往 stderr 写。这里改成进日志文件，并标 ERR 方便过滤。"""
        log_line("ERR  " + (fmt % args))

    def log_error(self, fmt, *args):
        # send_error 的错误详情，同样落文件
        log_line("ERR  " + (fmt % args))


class OTAServer(ThreadingHTTPServer):
    # 每个连接一个线程，客户端断开后线程不阻塞主循环
    daemon_threads = True
    allow_reuse_address = True

    def handle_error(self, request, client_address):
        """吞掉"客户端提前断开"这一类噪音。

        ESP 在 CRC 失败重试、掉电、烧录重启时都会直接 reset 连接，
        标准实现会把 BrokenPipe 的完整 traceback 甩到控制台，
        把真正有价值的请求日志冲得找不到。
        """
        etype = sys.exc_info()[0]
        if etype in (ConnectionResetError, BrokenPipeError,
                     ConnectionAbortedError, TimeoutError):
            ip = client_address[0] if client_address else "-"
            log_line("%-15s 连接被客户端提前关闭（不计为错误）" % ip)
            return
        super().handle_error(request, client_address)


def port_in_use(port, host="127.0.0.1"):
    """端口上是否已经有东西在监听。

    为什么不能靠 bind 失败来发现冲突：
    Windows 的 SO_REUSEADDR 语义和 Linux 完全不同 —— 它允许
    **重复绑定一个已经在被监听的端口**。实测两个实例都能 bind 成功，
    端口看起来只有一个，实际是两套 socket 在互相抢连接，
    排障时会看到"请求时好时坏"这种最难受的现象。
    所以必须在 bind 之前主动探一次。
    """
    s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    s.settimeout(0.5)
    try:
        # connect_ex 返回 0 表示连得上，即有人监听
        return s.connect_ex((host, port)) == 0
    except OSError:
        return False
    finally:
        s.close()


def probe_existing(port):
    """端口上是否已经跑着一个"本服务"的实例。

    用途：计划任务配了开机 + 登录两个触发器（快速启动下 AtStartup 会漏跑），
    两个触发器可能撞在一起。任务本身设置了 MultipleInstances=IgnoreNew，
    正常情况下不会重复启动；但如果是手工再跑一次 ota_server.py，
    第二个进程会因端口占用而启动失败。

    这里主动探一下：能拿到 /health 且 server 字段对得上，
    就说明是"自己人"，静默退出即可，不该报错。
    """
    try:
        with urllib.request.urlopen(
                "http://127.0.0.1:%d/health" % port, timeout=2) as resp:
            data = json.loads(resp.read().decode("utf-8"))
        return data.get("server") == "ota-firmware-server"
    except Exception:
        # 连不上 / 不是 JSON / 字段不对 —— 一律认为不是本服务
        return False


def local_ipv4_list():
    """列出本机 IPv4，方便确认"设备端该填哪个地址"。"""
    ips = []

    # 方法一：连一下外网拿默认出口地址（UDP connect 不发包，断网也不报错）
    try:
        s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        s.connect(("8.8.8.8", 80))
        ips.append(s.getsockname()[0])
        s.close()
    except OSError:
        pass

    # 方法二：主机名解析，能拿到热点网段等其它网卡地址
    try:
        for info in socket.getaddrinfo(socket.gethostname(), None, socket.AF_INET):
            ip = info[4][0]
            if ip not in ips and not ip.startswith("127."):
                ips.append(ip)
    except OSError:
        pass

    return ips


def main():
    global ROOT, PORT, BIND, LOG_FILE

    here = os.path.dirname(os.path.abspath(__file__))

    ap = argparse.ArgumentParser(description="OTA 固件 HTTP 服务（常驻版）")
    ap.add_argument("--port", type=int, default=80, help="监听端口，默认 80")
    ap.add_argument("--root", default=os.path.join(here, "..", "ota_release"),
                    help="固件目录，默认脚本上一级的 ota_release")
    ap.add_argument("--bind", default="0.0.0.0", help="监听地址，默认 0.0.0.0")
    ap.add_argument("--log", default=os.path.join(here, "ota_server.log"),
                    help="访问日志路径，默认脚本同目录 ota_server.log")
    args = ap.parse_args()

    ROOT = os.path.abspath(args.root)
    PORT = args.port
    BIND = args.bind
    LOG_FILE = os.path.abspath(args.log)

    try:
        sys.stdout.reconfigure(encoding="utf-8", errors="replace")
    except (AttributeError, OSError):
        pass

    if not os.path.isdir(ROOT):
        print("[OTA] 固件目录不存在：%s" % ROOT)
        print("[OTA] 用 --root 指定正确路径，或先把 bin 放进这个目录")
        return 2

    log_line("=" * 62)
    log_line("OTA 固件服务启动")

    # bind 之前先探端口（原因见 port_in_use 的注释）：
    # 判断顺序是"先看是不是自己人，再看是不是别人"
    probe_host = BIND if BIND not in ("0.0.0.0", "::") else "127.0.0.1"
    if port_in_use(PORT, probe_host):
        if probe_existing(PORT):
            # 开机触发器和登录触发器撞在一起时会走到这里，属于正常情况
            log_line("端口 %d 上已有本服务实例在运行，本次启动跳过" % PORT)
            return 0

        log_line("端口 %d 已被其它程序占用，用下面两条命令查：" % PORT)
        log_line("    netstat -ano | findstr :%d" % PORT)
        log_line('    tasklist /FI "PID eq <上一步查到的PID>"')
        return 1

    try:
        httpd = OTAServer((BIND, PORT), OTAHandler)
    except OSError as e:
        log_line("绑定 %s:%d 失败：%s" % (BIND, PORT, e))

        # 先看是不是"自己已经在了"。是的话属于正常情况，静默成功退出，
        # 免得开机+登录两个触发器撞在一起时日志里刷一条吓人的报错
        if probe_existing(PORT):
            log_line("端口上已有本服务实例在运行，本次启动跳过")
            return 0

        log_line("端口被其它程序占用，用下面两条命令查：")
        log_line("    netstat -ano | findstr :%d" % PORT)
        log_line('    tasklist /FI "PID eq <上一步查到的PID>"')
        return 1

    log_line("固件目录  %s" % ROOT)
    log_line("访问日志  %s" % LOG_FILE)

    ips = local_ipv4_list()
    if ips:
        log_line("本机地址  %s" % "   ".join(ips))

    bins = [n for n in sorted(os.listdir(ROOT)) if n.lower().endswith(".bin")]
    if bins:
        for n in bins:
            full = os.path.join(ROOT, n)
            log_line("  可用固件  %-24s %8d 字节  md5=%s"
                     % (n, os.path.getsize(full), md5_of(full)))
    else:
        log_line("  警告：固件目录里没有任何 .bin 文件")

    log_line("自检地址  http://127.0.0.1:%d/health" % PORT)
    log_line("=" * 62)

    try:
        httpd.serve_forever()
    except KeyboardInterrupt:
        log_line("收到 Ctrl+C，服务停止")
    finally:
        httpd.server_close()

    return 0


if __name__ == "__main__":
    sys.exit(main())
