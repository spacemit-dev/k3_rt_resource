# K3 RT 资源包说明

本仓库用于保存 Spacemit K3 小核 ESOS/RT-Thread 相关固件、系统更新脚本以及实时性能测试用例。主要用途包括：

- 将 `firmware/` 中的 ESOS 固件更新到目标系统；
- 按 Bianbu 系统版本自动选择匹配的 ESOS 固件；
- 通过指定目录更新外部固件包；
- 提供小核实时性能测试套件 `rt-perf-test/`；
- 便于在目标板上快速部署、验证和对比实时性能。

## 目录结构

| 路径 | 说明 |
| --- | --- |
| `.gitmodules` | 记录 `rt-perf-test/` 子模块来源 |
| `update_esos.sh` | 默认 ESOS 固件更新脚本，会从 `firmware/` 或匹配的版本子目录读取固件并替换系统文件，随后按启动介质刷写 `esos.itb` |
| `update_esos_from_dir.sh` | 从用户指定的固件目录更新 ESOS 固件 |
| `firmware/` | 存放默认待更新 ESOS 固件文件 |
| `firmware/esos.itb` | 默认 ESOS 启动镜像，脚本会将其复制到系统目录并刷写到启动分区 |
| `firmware/rt24_os0_rcpu.elf` | 小核 OS0 RCPU 固件 |
| `firmware/rt24_os1_rcpu.elf` | 小核 OS1 RCPU 固件 |
| `firmware/4.0.0/` | Bianbu `VERSION_ID=4.0` 对应固件目录 |
| `firmware/4.0.1/` | Bianbu `VERSION_ID=4.0.1` 对应固件目录 |
| `rt-perf-test/` | 小核实时性能测试套件，包含 EKF、FOC、信号处理、AHRS、内存、实时延迟、RPMsg、MPC、模型推理和调度压力测试 |

## 获取子模块

`rt-perf-test/` 以 Git 子模块形式维护。首次克隆仓库后可执行：

```bash
git submodule update --init --recursive
```

如果需要单独添加该子模块，可使用：

```bash
git submodule add https://github.com/spacemit-dev/rt-perf-test rt-perf-test
```

## 更新 ESOS 固件

### 使用仓库内固件

在目标系统中进入本目录后，直接执行：

```bash
bash update_esos.sh
```

`update_esos.sh` 会优先根据目标系统 `/etc/os-release` 自动选择固件目录：

| 系统 | 固件目录 |
| --- | --- |
| `ID=bianbu` 且 `VERSION_ID=4.0` | `firmware/4.0.0/` |
| `ID=bianbu` 且 `VERSION_ID=4.0.1` | `firmware/4.0.1/` |
| 其他系统或未匹配版本 | `firmware/` |

### 使用指定固件目录

如果固件不在本仓库 `firmware/` 中，可使用：

```bash
bash update_esos_from_dir.sh /path/to/firmware_dir
```

指定目录中必须包含 `esos.itb`、`rt24_os0_rcpu.elf` 和 `rt24_os1_rcpu.elf`。

脚本会执行以下操作：

1. 选择仓库内固件目录，或检查用户指定固件目录；
2. 将固件复制到临时目录 `~/tmp_esos`；
3. 替换系统目录 `/usr/lib/riscv64-linux-gnu/esos/` 下的固件文件；
4. 判断当前是否处于 chroot 环境，若是则跳过启动分区刷写；
5. 判断当前平台是否为 Spacemit X100，非目标平台则跳过启动分区刷写；
6. 根据 `boot_mode`、`root=` 和设备类型选择目标启动介质；
7. 使用 `dd` 将 `esos.itb` 写入启动分区；
8. 执行 `update-initramfs -u` 更新 initramfs。

## 固件文件要求

执行更新脚本前，请确认实际使用的固件目录中存在以下文件：

```text
esos.itb
rt24_os0_rcpu.elf
rt24_os1_rcpu.elf
```

对于仓库内固件，常见目录为：

```text
firmware/
firmware/4.0.0/
firmware/4.0.1/
```

如果缺少任一必需文件，脚本会直接退出并打印缺失文件路径。

## 实时性能测试

`rt-perf-test/` 目录提供小核实时性能测试用例，常用 MSH 命令包括：

```text
ekf_perf
foc_perf
signal_perf
ahrs_perf
mem_perf
rtlat_perf
rpmsg_perf
mpc_perf
model_perf
sched_perf
```

详细说明见：

- `rt-perf-test/README.md`
- `rt-perf-test/07_rpmsg/README.md`

## 注意事项

- `update_esos.sh` 会写系统目录并可能刷写启动分区，通常需要 root 权限。
- 刷写启动分区前请确认 `firmware/esos.itb` 与当前板卡、启动介质和系统版本匹配。
- 脚本会自动识别 `emmc`、`sdcard`、`ufs`、`nor`、`nand` 等启动模式，但异常分区布局仍需人工确认。
- 在 chroot 环境中执行时，脚本只替换系统固件文件，不会刷写启动分区。
- 非 Spacemit X100 平台执行时，脚本会跳过启动分区刷写。
- 更新完成后建议重启系统，并通过小核日志或 MSH 命令确认 ESOS/RT-Thread 固件启动正常。
