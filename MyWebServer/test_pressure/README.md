# MyWebServer 压力测试（Webbench）

> 本目录用于对 **MyWebServer**（Step11 `step11.cpp` / `server`）做 HTTP 压测。  
> 工具来源：从 [TinyWebServer](https://github.com/qinguoyi/TinyWebServer) 的 `test_pressure` 复制而来，内含 **webbench-1.5**（Lionbridge 开发的经典压测工具）。  
> **详细教程：** [docs/11-压力测试指南.md](../docs/11-压力测试指南.md)

---

## 目录结构

```text
MyWebServer/
├── server                          # g++ 编译出的 Web 服务器
├── www/index.html                  # 压测常用 URL 对应的静态页
└── test_pressure/
    ├── README.md                   # 本文件
    └── webbench-1.5/
        ├── Makefile
        ├── webbench.c
        └── webbench                # make 之后生成（可执行文件）
```

从 Tiny 复制 **`test_pressure` 整个文件夹**到 MyWebServer 根目录即可使用，**无需**改 Tiny 的 server 代码。

---

## 1. 编译 webbench（只需做一次）

在 **WSL2 / Linux** 下：

```bash
cd /path/to/MyWebServer/test_pressure/webbench-1.5
make clean && make
```

成功标志：当前目录出现可执行文件 **`webbench`**。

```bash
./webbench --help
```

| 情况 | 处理 |
|------|------|
| `make` 提示 `ctags` 警告 | 一般可忽略，只要有 `webbench` 文件 |
| `gcc: command not found` | `sudo apt install build-essential` |
| 之前编译过、移动过目录 | `make clean && make` 重新编译 |

---

## 2. 启动 MyWebServer（压测前）

在 **另一个终端**，于 **项目根目录**：

```bash
cd /path/to/MyWebServer

# 先编译 step11（若尚未编译）
g++ -std=c++17 -Wall -pthread -o server Step1to6/step11.cpp -lmysqlclient
# 或：g++ ... -o server step11.cpp -lmysqlclient

ulimit -n 65535
./server -p 8080 -t 4 -c 1
```

| 参数 | 含义 |
|------|------|
| `-p 8080` | 监听端口（与 webbench URL 一致） |
| `-t 4` | 线程池 4 个工作线程 |
| `-c 1` | **关闭日志**（压测时建议关闭，QPS 更高） |

冒烟测试：

```bash
curl -I http://127.0.0.1:8080/index.html
# 期望：HTTP/1.1 200 OK
```

---

## 3. 运行压测

在 **项目根目录**执行（路径相对于 MyWebServer 根）：

```bash
# 从小并发开始，逐步加大
./test_pressure/webbench-1.5/webbench -c 100  -t 5 http://127.0.0.1:8080/index.html
./test_pressure/webbench-1.5/webbench -c 500  -t 5 http://127.0.0.1:8080/index.html
./test_pressure/webbench-1.5/webbench -c 1000 -t 5 http://127.0.0.1:8080/index.html
```

### 参数说明

| 参数 | 含义 |
|------|------|
| `-c` | 并发客户端数（webbench 用多进程模拟） |
| `-t` | 持续秒数 |
| URL | 建议写 **`/index.html`**，测静态 GET（与 Step11 教程一致） |

### 建议压什么、不压什么

| URL | 是否推荐 |
|-----|----------|
| `http://127.0.0.1:8080/index.html` | ✅ 主压测目标 |
| `http://127.0.0.1:8080/about.html` | ✅ 可以 |
| `POST /login`、`POST /register` | ❌ webbench 默认 GET，且走 MySQL，不适合做主 QPS 指标 |

注册 / 登录请用浏览器或 curl 抽检（Step10 已验收）。

---

## 4. 如何看结果

输出中重点关注：

```text
Requests per second:    xxxxx [#/sec] (mean)   ← QPS
Failed requests:        0                      ← 应为 0 或极少
```

| 字段 | 说明 |
|------|------|
| **Requests per second** | 每秒请求数（QPS） |
| **Failed requests** | 失败数；非 0 时先查 server 是否在跑、`ulimit -n`、`-c` 是否过大 |
| **Complete requests** | 总完成数 ≈ QPS × 时间 |

更详细的字段说明见 [11-压力测试指南 · 第七部分](../docs/11-压力测试指南.md#第七部分如何读懂-webbench-输出)。

---

## 5. 对比实验示例（Step11）

固定：URL = `index.html`，webbench `-t 5`，server 使用 `-c 1` 关日志。

| 编号 | 启动 server | webbench | 记录 |
|------|-------------|----------|------|
| 1 | `./server -t 2 -c 1` | `-c 500` | QPS |
| 2 | `./server -t 4 -c 1` | `-c 500` | QPS |
| 3 | `./server -t 8 -c 1` | `-c 500` | QPS |
| 4 | `./server -t 4 -c 0` | `-c 500` | 开日志对比 |
| 5 | `./server -t 4 -c 1 --no-timer` | `-c 500` | 关定时器对比 |

**一次只改一个变量**，记下 QPS 与 `Failed requests`。

---

## 6. 常见问题

### `webbench: command not found`

未编译或不在 PATH 里。请用**完整路径**：

```bash
./test_pressure/webbench-1.5/webbench -c 100 -t 5 http://127.0.0.1:8080/index.html
```

### `Failed requests` 很多

1. `ulimit -n 65535` 后重启 server  
2. 降低 `-c`（如 100 → 500 再试）  
3. `curl -I http://127.0.0.1:8080/index.html` 确认服务正常  

### QPS 很低

- 是否用了 `-c 0` 开着日志？压测请用 **`-c 1` 关日志**  
- 是否在 WSL / 虚拟机中（性能低于裸机 Linux）  
- 线程数可试 `-t 4` 或 `-t 8`  

### 在 Windows 本机能否压测？

请在 **WSL2** 中编译 `server` 与 `webbench`，与 Step4～11 一致。

---

## 7. 关于 webbench 工具本身

- **Webbench** 通过 fork 多个子进程向目标 URL 反复发 HTTP 请求，统计 QPS 与失败率。  
- 本目录仅存放压测工具源码，**不参与** MyWebServer 的编译链接。  
- Tiny README 中的超高 QPS 多在关日志 + LT/ET 调优环境下测得；学习项目以**相对对比**（关日志 vs 开日志、不同线程数）为主，不必追求复现具体数字。

---

## 相关文档

- [docs/11-压力测试指南.md](../docs/11-压力测试指南.md) — Step11 完整教程  
- [docs/Step5之后完善路线图.md](../docs/Step5之后完善路线图.md) — Step11 / Step12 路线  
