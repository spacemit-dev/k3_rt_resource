# RPMsg 大小核通信性能测试

本目录用于测试 Spacemit K3 大核 Linux 与小核 ESOS/RT-Thread 之间的 RPMsg 通信性能。

## 文件说明

| 文件 | 运行侧 | 说明 |
| --- | --- | --- |
| `rpmsg_perf_test.c` | 小核 RCPU | 创建 `rpmsg:perf_test` 服务，收到大核数据后立即 echo 回传，每 `1000` 包记录一次小核侧收发统计，测试结束后统一打印汇总 |
| `k3_rpmsg_perf.c` | 大核 Linux | 创建 Linux RPMsg endpoint，发送固定长度数据包并等待 echo，统计 RTT 和吞吐 |

## RPMsg 参数

| 参数 | 值 |
| --- | --- |
| 服务名 | `rpmsg:perf_test` |
| 小核地址 | `1010` |
| 大核地址 | `1011` |
| 默认控制设备 | `/dev/rpmsg_ctrl0` |
| 默认数据设备 | `/dev/rpmsg0` |
| 最大 frame 长度 | `2048 bytes` |
| frame 头长度 | `20 bytes` |
| 最大 payload 长度 | `2028 bytes` |

## 小核侧使用

`rpmsg_perf_test.c` 已加入 `bsp/spacemit/applications/SConscript` 的 `os1_rcpu` 构建源文件列表。

启动小核后，在 MSH 中执行：

```text
rpmsg_perf
```

正常会看到类似输出：

```text
[RPMSG_PERF] Service starting...
[RPMSG_PERF] rpdev ready, creating endpoint...
[RPMSG_PERF] Endpoint created: rpmsg:perf_test (src=1010, dst=1011), max_frame=2048, tx_payload_limit=xxx, rx_payload_limit=xxx
```

测试过程中小核侧不实时打印统计，避免 `rt_kprintf` 影响测试结果。小核每收到 `1000` 个包记录一次统计快照；停止收包并空闲约 `3000 ms` 后，统一打印记录和汇总，例如：

```text
[RPMSG_PERF] Test idle 3000 ms, print recorded summary
[RPMSG_PERF] [1] total=1000, delta=1000, rx=xxx KB/s, tx=xxx KB/s, bad=0, err=0
[RPMSG_PERF] [2] total=2000, delta=1000, rx=xxx KB/s, tx=xxx KB/s, bad=0, err=0
...
[RPMSG_PERF] Summary: rx=10000 packets xxxx KB, tx=10000 packets xxxx KB, bad=0, err=0
```

如果测试包数不是 `1000` 的整数倍，最后不足 `1000` 包的一段也会在汇总时记录并打印。默认最多保存 `1024` 条记录；超出后最终汇总会带有 `record_overflow` 提示。

## 大核侧编译

在 K3 Linux 目标板上编译：

```bash
cd /media/chenzhaoqi/data/tmp/whls/esos/bsp/spacemit/applications/rt-perf-test/07_rpmsg
gcc -Wall -Wextra -O2 -o k3_rpmsg_perf k3_rpmsg_perf.c
```

交叉编译时将 `gcc` 替换成目标工具链，例如：

```bash
cd /media/chenzhaoqi/data/tmp/whls/esos/bsp/spacemit/applications/rt-perf-test/07_rpmsg
riscv64-unknown-linux-gnu-gcc -Wall -Wextra -O2 -o k3_rpmsg_perf k3_rpmsg_perf.c
```

## 大核侧运行

先在小核 MSH 启动 `rpmsg_perf`，再在大核 Linux 执行：

```bash
sudo ./k3_rpmsg_perf
```

默认测试参数：

- payload：`128 bytes`
- packet count：`10000`
- 大核侧每 `1000` 个包打印一次统计；小核侧每 `1000` 个包记录一次统计，测试结束后统一打印

示例输出：

```text
RPMsg ready: service=rpmsg:perf_test src=1011 dst=1010
[1000/10000] avg_rtt=xxx.xx us min=xxx.xx us max=xxx.xx us throughput=xxx.xx KB/s
...
Summary: packets=10000 payload=128 frame=148 elapsed=x.xxx s
RTT: avg=xxx.xx us min=xxx.xx us max=xxx.xx us
Full-duplex echo throughput: xxx.xx KB/s
```

## 常用参数

```bash
sudo ./k3_rpmsg_perf -n 50000 -s 256 -r 5000
```

| 参数 | 说明 |
| --- | --- |
| `-n <count>` | 发送包数量 |
| `-s <bytes>` | payload 大小，范围 `0..2028` |
| `-r <count>` | 每多少个包打印一次统计 |
| `-c <device>` | RPMsg control 设备，默认 `/dev/rpmsg_ctrl0` |
| `-d <device>` | RPMsg data 设备，默认 `/dev/rpmsg0` |

## 测试建议

1. 先使用默认 `128 bytes` payload 测 RTT 和基础吞吐。
2. 再分别测试典型 payload：`0`、`32`、`64`、`128`、`256`、`476`、`1024`、`2028`。
3. 大核程序采用一发一收模式，因此统计的是 request/echo 往返性能，不是纯单向极限吞吐。
4. 小核侧统计会在停止收包并空闲约 `3000 ms` 后统一打印；测试刚结束时等待片刻即可看到汇总。
5. 如果提示 `timeout waiting echo`，检查小核是否已执行 `rpmsg_perf`，以及 `/dev/rpmsg_ctrl0`、`/dev/rpmsg0` 是否存在。
6. 代码层最大 frame 已放大到 `2048 bytes`，但最终是否能发送超过 `496 bytes` 仍取决于底层 RPMsg/virtio buffer 配置。小核启动时打印的 `tx_payload_limit` / `rx_payload_limit` 是当前固件实际可用的单包 payload 上限；如果仍为约 `496`，需要同步放大 OpenAMP 的 `RPMSG_BUFFER_SIZE` 以及 Linux 侧 RPMsg vring buffer 配置。
