# pidinfo

A Linux kernel character device driver that exposes live process information via `/dev/pidinfo`. Write a PID to the device and read it back to get a formatted report pulled directly from the kernel's internal `task_struct`.

```
name       : firefox
pid        : 1176
parent pid : 638
state      : SLEEPING
threads    : 98
memory     : 582408 kB
user cpu   : 15119 ms
kernel cpu : 14340 ms
```

Unlike `ps` or `top`, which read from `/proc`, this driver accesses kernel data structures directly through internal kernel APIs.

## Requirements

- Linux kernel headers matching your running kernel
- Clang + lld (required on CachyOS/kernels built with Clang)

```bash
sudo pacman -S linux-cachyos-headers   # CachyOS
# or
sudo apt install linux-headers-$(uname -r)   # Debian/Ubuntu
```

## Build

```bash
make CC=clang LD=ld.lld
```

## Usage

**Load the module:**
```bash
sudo insmod pidinfo.ko
```

**Query a process:**
```bash
sudo sh -c 'echo <PID> > /dev/pidinfo && cat /dev/pidinfo'
```

**Live monitoring:**
```bash
watch -n1 "sudo sh -c 'echo $(pgrep firefox) > /dev/pidinfo && cat /dev/pidinfo'"
```

**Unload:**
```bash
sudo rmmod pidinfo
```

## Notes

The `sh -c` wrapper is necessary because `echo` and `cat` need to share the same open file descriptor. The written PID is stored in per-session state and a separately opened `cat` would not see it.

Kernel messages can be inspected with `sudo dmesg | tail -5`.
