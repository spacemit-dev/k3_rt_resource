#!/bin/bash
set -e

running_in_chroot() {
  if [ "${SYSTEMD_IGNORE_CHROOT:-0}" = "1" ]; then
    return 1
  fi

  if [ -e "/proc/1/root" ]; then
    root_dev_ino=$(stat -c '%d:%i' / 2>/dev/null) || return 0
    proc1_root_dev_ino=$(stat -L -c '%d:%i' /proc/1/root 2>/dev/null) || return 0

    [ "$root_dev_ino" = "$proc1_root_dev_ino" ] && return 1 || return 0
  fi

  if [ ! -d "/proc" ] || [ ! -r "/proc/version" ]; then
    [ "$$" = "1" ] && return 1 || return 0
  fi

  return 0
}

get_boot_device_from_mode() {
  case "$1" in
    emmc)
      echo "/dev/mmcblk2"
      ;;
    sdcard)
      echo "/dev/mmcblk0"
      ;;
    nor|nand)
      if [ -e "/dev/mtdblock0" ]; then
        echo "/dev/mtdblock0"
      fi
      ;;
    ufs)
      echo "/dev/sda"
      ;;
    *)
      echo ""
      ;;
  esac
}

get_base_device() {
  case "$1" in
    /dev/mmcblk0*)
      echo "/dev/mmcblk0"
      ;;
    /dev/mmcblk2*)
      echo "/dev/mmcblk2"
      ;;
    /dev/sda*)
      echo "/dev/sda"
      ;;
    /dev/nvme0n1*)
      echo "/dev/nvme0n1"
      ;;
    /dev/mtdblock*)
      echo "/dev/mtdblock0"
      ;;
    *)
      echo ""
      ;;
  esac
}

get_mtdblock_by_name() {
  local name="$1"
  local mtd

  if [ ! -f "/proc/mtd" ]; then
    return 1
  fi

  mtd=$(awk -F'[:"]' -v name="$name" '$3 == name {print $1; exit}' /proc/mtd)
  if [ -n "$mtd" ]; then
    echo "/dev/mtdblock${mtd#mtd}"
    return 0
  fi

  return 1
}

get_esos_kernel_url_base() {
  local base_url="./firmware"
  local os_id=""
  local os_version_id=""
  local path_version=""

  if [ -r "/etc/os-release" ]; then
    os_id=$(awk -F= '$1 == "ID" {gsub(/"/, "", $2); print $2; exit}' /etc/os-release)
    os_version_id=$(awk -F= '$1 == "VERSION_ID" {gsub(/"/, "", $2); print $2; exit}' /etc/os-release)
  fi

  if [ "$os_id" = "bianbu" ]; then
    case "$os_version_id" in
      4.0)
        path_version="4.0.0"
        ;;
      4.0.1)
        path_version="4.0.1"
        ;;
    esac

    if [ -n "$path_version" ]; then
        echo "识别到 Bianbu 版本: $os_version_id" >&2
        echo "$base_url/$path_version"
        return 0
    fi
  fi

  echo "$base_url"
}

rm -rf ~/tmp_esos
mkdir -p ~/tmp_esos
cd ~/tmp_esos

echo "📥 下载 esos 文件..."
ESOS_KERNEL_URL_BASE=$(get_esos_kernel_url_base)
cp "$ESOS_KERNEL_URL_BASE/esos.itb" .
cp "$ESOS_KERNEL_URL_BASE/rt24_os0_rcpu.elf" .
cp "$ESOS_KERNEL_URL_BASE/rt24_os1_rcpu.elf" .

echo "🔁 替换系统文件..."
mkdir -p /usr/lib/riscv64-linux-gnu/esos
cp esos.itb /usr/lib/riscv64-linux-gnu/esos/esos.itb
cp rt24_os0_rcpu.elf /usr/lib/riscv64-linux-gnu/esos/rt24_os0_rcpu.elf
cp rt24_os1_rcpu.elf /usr/lib/riscv64-linux-gnu/esos/rt24_os1_rcpu.elf

if running_in_chroot; then
  echo "⚠️ 当前运行在 chroot 中，跳过刷写启动分区。"
  exit 0
fi

if grep -q 'Spacemit(R) X100' /proc/cpuinfo; then
  :
else
  echo "⚠️ 不是 Spacemit X100，跳过刷写启动分区。"
  exit 0
fi

ROOT=""
BOOT_MODE=""
for x in $(cat /proc/cmdline); do
  case $x in
    root=UUID=*)
      ROOT_UUID=${x#root=UUID=}
      DEVICES=$(blkid -s UUID | grep "$ROOT_UUID" | awk '{print $1}')
      DEVICE_COUNT=$(echo "$DEVICES" | wc -l)
      if [ "$DEVICE_COUNT" -gt 1 ]; then
        echo "Warning: Multiple devices found with the same UUID $ROOT_UUID:"
        echo "$DEVICES"
        echo "This may cause installation issues. Please ensure unique UUIDs."
        exit 0
      fi
      ROOT=$(echo "$DEVICES" | head -n1)
      ;;
    root=*)
      ROOT=${x#root=}
      ;;
    boot_mode=*)
      BOOT_MODE=${x#boot_mode=}
      ;;
  esac
done

TARGET_DEVICE=""
if [ -n "$BOOT_MODE" ]; then
  BOOT_DEVICE=$(get_boot_device_from_mode "$BOOT_MODE")
  if [ -n "$BOOT_DEVICE" ] && [ -n "$ROOT" ]; then
    ROOT_BASE_DEVICE=$(get_base_device "$ROOT")
    if [ "$BOOT_DEVICE" != "$ROOT_BASE_DEVICE" ]; then
      TARGET_DEVICE="$BOOT_DEVICE"
      echo "Boot device ($BOOT_MODE -> $BOOT_DEVICE) differs from rootfs device ($ROOT_BASE_DEVICE), using boot device."
    else
      TARGET_DEVICE="$ROOT_BASE_DEVICE"
      echo "Boot device matches rootfs device, using $TARGET_DEVICE."
    fi
  elif [ -n "$BOOT_DEVICE" ]; then
    TARGET_DEVICE="$BOOT_DEVICE"
    echo "Using boot device from boot_mode=$BOOT_MODE: $TARGET_DEVICE"
  else
    echo "Unsupported boot_mode=$BOOT_MODE, unable to determine boot device."
    exit 1
  fi
fi

if [ -z "$TARGET_DEVICE" ] && [ -n "$ROOT" ]; then
  TARGET_DEVICE=$(get_base_device "$ROOT")
fi

ESOS=""
ESOS_SEEK=""
case "$TARGET_DEVICE" in
  /dev/mmcblk0)
    ESOS=/dev/mmcblk0
    ESOS_SEEK=4096
    ;;
  /dev/mmcblk2)
    ESOS=/dev/mmcblk2
    ESOS_SEEK=4096
    ;;
  /dev/sda)
    ESOS=/dev/sda
    ESOS_SEEK=4096
    ;;
  /dev/mtdblock0)
    if [ ! -f "/proc/mtd" ]; then
      echo "Error: /proc/mtd not found, cannot determine MTD partition layout"
      exit 1
    fi

    ESOS_MTD=$(get_mtdblock_by_name "esos" || true)
    if [ -n "$ESOS_MTD" ]; then
      echo "Detected MTD esos partition: $ESOS_MTD"
      ESOS="$ESOS_MTD"
      ESOS_SEEK=0
    else
      MTD0_NAME=$(grep '^mtd0:' /proc/mtd | awk -F'"' '{print $2}')
      if [ "$MTD0_NAME" = "bootinfo" ]; then
        echo "Detected MTD partition layout: independent partitions"
        ESOS=/dev/mtdblock3
        ESOS_SEEK=0
      else
        echo "Detected MTD partition layout: single device with offsets (mtd0=$MTD0_NAME)"
        ESOS=/dev/mtdblock0
        ESOS_SEEK=704
      fi
    fi
    ;;
  /dev/nvme0n1)
    if [ -f "/proc/mtd" ]; then
      MTD0_NAME=$(grep '^mtd0:' /proc/mtd | awk -F'"' '{print $2}')
      if [ "$MTD0_NAME" = "bootinfo" ]; then
        ESOS=/dev/mtdblock3
        ESOS_SEEK=0
      else
        ESOS=/dev/mtdblock0
        ESOS_SEEK=704
      fi
    else
      ESOS=/dev/mmcblk2
      ESOS_SEEK=704
    fi
    ;;
  *)
    echo "Unsupported target device=$TARGET_DEVICE"
    exit 1
    ;;
esac

if [ -z "$ESOS" ] || [ -z "$ESOS_SEEK" ]; then
  echo "Unable to determine target device (missing root= or boot_mode= in cmdline)"
  exit 1
fi

for file in /usr/lib/riscv64-linux-gnu/esos/esos.itb "$ESOS"; do
  if [ ! -e "$file" ]; then
    echo "Missing $file"
    exit 1
  fi
done

echo "💾 刷写 itb 到启动分区: $ESOS (seek=$ESOS_SEEK)"
set -x
dd if=/usr/lib/riscv64-linux-gnu/esos/esos.itb of="$ESOS" seek="$ESOS_SEEK" bs=1K
sync
set +x

echo "🧱 更新 initramfs..."
update-initramfs -u

echo "✅ 完成"

rm -rf ~/tmp_esos
