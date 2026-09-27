# lab-so

Linux kernel modules and user-space programs written for an operating systems
lab course (PUCRS). Everything runs on a small i386 Linux distribution built
with Buildroot 2025.02.11 (kernel 6.12.27) and booted in QEMU.

The Buildroot tree itself is not included. Only the files added or modified
on top of it are versioned, under `buildroot-2025.02.11/`, so that the relative
paths used by the build scripts and module Makefiles stay valid.

The report for assignment 3 (SSTF disk scheduler) is in
[`relatorio-t3.md`](relatorio-t3.md), in Portuguese.

## Contents

### Kernel modules (`buildroot-2025.02.11/modules/`)

| Module | Description |
|---|---|
| `hello` | Prints one message at each `printk` log level on load and a message on unload. |
| `khello-trace` | Same as `hello`, using `trace_printk()`. Output goes to the ftrace ring buffer (`/sys/kernel/tracing/trace`). |
| `mymodule_param` | Takes a string (`string_par`) and an integer (`int_par`) module parameter and prints them. Refuses to load without them. |
| `procdriver` | Creates `/proc/helloworld`. Each read returns `HelloWorld!, N reads`. |
| `chardriver` | Character device `/dev/chardrv`. A write stores the text prefixed with its length; the next read returns it. |
| `timerdriver` | High-resolution timer that logs `Hello from timer!` periodically (default 1 s). The period is set by the `timeout_nsec` parameter and a `timeout_sec` sysfs attribute. |
| `waitdriver` | Character device `/dev/waitdrv` with a kernel thread blocked on a wait queue. `open`, `read`, `write`, `close` and module exit wake the thread, which logs which operation woke it. |
| `pubsub` | Publish/subscribe message broker exposed as `/dev/pubsub` (see below). |
| `sstf` | Shortest Seek Time First I/O scheduler for blk-mq (see below). |

#### pubsub

Commands are written to `/dev/pubsub` as text:

- `/subscribe <topic>` subscribes the calling process. The topic is created on first subscription.
- `/unsubscribe <topic>` removes the subscription. A topic with no subscribers is removed.
- `/publish <topic> <message>` queues the message for every subscriber of the topic. Surrounding double quotes are stripped. Messages are truncated to 4096 bytes.
- `/fetch <topic>` selects the topic; each following `read()` on the same file descriptor returns the oldest queued message for the calling process.

Subscribers are identified by PID and are unsubscribed when they close the device.
The module also provides:

- `/proc/pubsub`: each topic and its total message count.
- `/sys/pubsub/<topic>/max_subscribers`: subscriber limit per topic (default 10), writable.
- Module parameter `max_topics` (default 10).

#### sstf

Registers an elevator named `sstf`, selectable through
`/sys/block/<dev>/queue/scheduler`. Requests are held in a queue and
dispatched when the queue reaches `queue_size` requests or when `max_wait_ms`
expires. Each dispatch picks the pending request whose sector is closest to
the current head position. With `debug=1` the module logs its decisions and,
on exit, the total sectors traveled in arrival order (FCFS) and in SSTF order
for the same request stream.

Parameters: `queue_size` (default 50, intended range 20-100), `max_wait_ms`
(default 50, intended range 20-100), `debug` (0/1).

The module includes the kernel-internal header `block/elevator.h` from the
Buildroot kernel build directory, so it only builds against that tree.

### Applications (`buildroot-2025.02.11/apps/`)

| Program | Installed as | Description |
|---|---|---|
| `hello.c` | `/usr/bin/hello` | Prints an ASCII-art greeting. Run at boot by `S50hello`. |
| `monitor.c` | `/var/www/cgi-bin/monitor` | CGI program that renders an HTML page with system information read from `/proc`: kernel version, uptime, RTC date, CPU, load, CPU usage, memory, disk I/O, filesystems, devices, network interfaces and running processes. |
| `pubsub-teste.c` | `/usr/bin/pubsub-teste` | Interactive client for `/dev/pubsub`. Each input line is written to the device; `/read` reads one message; `/quit` exits. |
| `schedrt/query.c` | `/usr/bin/schedrt-query` | Prints the scheduling policy and attributes of the calling process (`sched_getattr`). |
| `schedrt/dl.c` | `/usr/bin/schedrt-dl` | Runs a job loop under `SCHED_DEADLINE`. Arguments: runtime, deadline, period and work time, in ms. |
| `schedrt/multi.c` | `/usr/bin/schedrt-multi` | Three `SCHED_DEADLINE` threads with different runtime/period pairs (total utilization 0.265). |
| `disk-test/raw.c` | `/usr/bin/disk-test-raw` | Writes a string at the start of `/dev/sdb`. |
| `disk-test/sector-read.c` | `/usr/bin/disk-test-sector-read` | Configures the `sdb` queue (no merges, 4 KiB max request, no read-ahead) and reads random sectors. |
| `disk-test/bench.c` | `/usr/bin/disk-test-bench` | Runs read and write workloads on `sdb` under each available I/O scheduler and compares `/sys/block/sdb/stat`. |
| `sstf-teste.c` | `/usr/bin/sstf-teste` | Forks processes that issue random or sequential `O_DIRECT` reads and writes over the test disk. Device defaults to `/dev/sdb`, overridable with `SSTF_DEV`. |

### Build and init scripts (`buildroot-2025.02.11/custom-scripts/`)

- `pre-build.sh`: Buildroot pre-build script. Generates and installs the network init script, compiles the modules and applications with the Buildroot cross-compiler, installs them into the target root filesystem, sets up the web server files and adds a `tracefs` entry to `/etc/fstab`.
- `network-config`: template for `S41network-config`. The guest gets `192.168.1.10/24` on `eth0`. `pre-build.sh` replaces the placeholder with the host address taken from the host's default route at build time.
- `S50hello`, `S60httpd`: init scripts. `S60httpd` starts the BusyBox `httpd` on port 80 serving `/var/www` as user `httpd`.
- `index.html`: redirects to `/cgi-bin/monitor`.
- `qemu-ifup`: host-side QEMU tap script. Creates the tap interface, enables IPv4 forwarding and adds a route to `192.168.1.10`.
- `sstf-bench.sh`: installed as `/usr/bin/sstf-bench`. Runs the five SSTF test cases from the report, loading the module with each case's parameters.
- `users.txt.example`: template for the Buildroot users table.

### Buildroot configuration

- `buildroot-2025.02.11/.config`: full Buildroot configuration (i386, glibc, kernel 6.12.27, BusyBox, trace-cmd). It refers to `custom-scripts/pre-build.sh`, `custom-scripts/users.txt`, `board/qemu/x86/linux.config` and `package/busybox/busybox.config`.
- `board/qemu/x86/linux.config`: upstream QEMU x86 kernel config plus `CONFIG_FTRACE`, `CONFIG_FUNCTION_TRACER` and `CONFIG_FUNCTION_GRAPH_TRACER`.
- `package/busybox/busybox.config`: BusyBox config with `httpd` and CGI support.

## Building

Requirements: the host packages listed in the
[Buildroot manual](https://buildroot.org/downloads/manual/manual.html#requirement)
and QEMU (`qemu-system-i386`).

1. Clone this repository and download Buildroot 2025.02.11 from
   <https://buildroot.org/downloads/> into the repository root.

2. Extract it without overwriting the versioned files:

   ```sh
   tar --skip-old-files -xzf buildroot-2025.02.11.tar.gz
   ```

   The repository's `.gitignore` ignores the extracted Buildroot files.

3. Create the users table and set a password for the `httpd` user:

   ```sh
   cd buildroot-2025.02.11
   cp custom-scripts/users.txt.example custom-scripts/users.txt
   ```

4. Optionally set a root password with `make menuconfig`
   (System configuration, Root password). It is empty in the versioned
   `.config`. The serial console logs in to a shell without a password.

5. Build:

   ```sh
   make
   ```

   The kernel image and root filesystem are written to `output/images/`
   (`bzImage`, `rootfs.ext2`).

Modules can be rebuilt after the first full build with
`make -C modules/<name>`, which compiles against
`output/build/linux-6.12.27` and installs into `output/target`. Run `make`
again in the Buildroot directory to regenerate the root filesystem image.

## Running in QEMU

The upstream QEMU x86 board (`board/qemu/x86/readme.txt`) boots the image with:

```sh
qemu-system-i386 -M pc -kernel output/images/bzImage \
  -drive file=output/images/rootfs.ext2,if=virtio,format=raw \
  -append "rootwait root=/dev/vda console=tty1 console=ttyS0" \
  -serial stdio -net nic,model=virtio -net user
```

The repository does not contain the exact command used in the course. The
scripts expect the following:

- Networking with the web server: a tap network backend whose `script=`
  is `custom-scripts/qemu-ifup`, instead of `-net user`. The monitor page is
  then at `http://192.168.1.10/` from the host.
- Disk scheduler tests: a second disk that appears as `/dev/sdb` in the guest.
  The report used an image named `sdb.bin` attached with `-hdb sdb.bin`, and
  the benchmarks assume a size of 1 GiB (262144 blocks of 4096 bytes).

## SSTF benchmark

From `relatorio-t3.md`. Each run used 1000 operations on a 1 GiB disk
(`/dev/sdb`, `nr_requests=128`). The module counts sectors traveled for the
same request stream in arrival order (FCFS) and in SSTF order.

| Case | queue_size | max_wait_ms | writes | pattern | procs | FCFS sectors | SSTF sectors | Reduction |
|---|---|---|---|---|---|---|---|---|
| 1 random | 50 | 50 | 30% | rand | 128 | 652,622,928 | 42,948,032 | 93.4% |
| 2 sequential | 50 | 50 | 30% | seq | 1 | 7,992 | 7,992 | 0.0% |
| 3a small queue | 20 | 50 | 30% | rand | 128 | 611,423,712 | 116,125,856 | 81.0% |
| 3b large queue | 100 | 50 | 30% | rand | 128 | 620,409,616 | 24,825,712 | 96.0% |
| 4a short timeout | 50 | 20 | 30% | rand | 128 | 624,265,496 | 56,014,952 | 91.0% |
| 4b long timeout | 50 | 100 | 30% | rand | 128 | 655,987,976 | 39,155,000 | 94.0% |
| 5 write-heavy | 50 | 50 | 80% | rand | 128 | 620,530,312 | 51,281,832 | 91.7% |

The reduction grows with queue size and timeout, is zero for a sequential
stream and does not depend on the read/write ratio.

## Authors

Guilherme Hoffmann, Endrew Soares, João Sbardelotto and George Rother, as
listed in the report and in `MODULE_AUTHOR` of the `pubsub` module.
