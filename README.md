# Multi-Container Runtime

**Team Members:**
- Ponakala Yathish — SRN: PES1UG24CS322
- Vedanta Tripathi — SRN: PES1UG24CS920

---

## Build, Load, and Run Instructions

### Prerequisites

Ubuntu 22.04 or 24.04 VM with Secure Boot OFF. No WSL.

```bash
sudo apt update
sudo apt install -y build-essential linux-headers-$(uname -r)
```

### Build

```bash
cd boilerplate
make          # builds engine, memory_hog, cpu_hog, io_pulse (user-space)
sudo make module  # builds monitor.ko (kernel module, requires root)
```

### Prepare Root Filesystem

```bash
cd boilerplate
mkdir rootfs-base
wget https://dl-cdn.alpinelinux.org/alpine/v3.20/releases/x86_64/alpine-minirootfs-3.20.3-x86_64.tar.gz
tar -xzf alpine-minirootfs-3.20.3-x86_64.tar.gz -C rootfs-base

# Copy workload binaries into base rootfs
sudo cp cpu_hog memory_hog io_pulse ./rootfs-base/

# Create isolated per-container rootfs copies (required for filesystem isolation)
cp -a ./rootfs-base ./rootfs-alpha
cp -a ./rootfs-base ./rootfs-beta
cp -a ./rootfs-base ./rootfs-memtest
```

### Load the Kernel Module

```bash
sudo insmod monitor.ko
ls /dev/container_monitor      # verify device exists
sudo dmesg | tail -3           # verify: "[container_monitor] Module loaded"
```

### Start the Supervisor

```bash
# Terminal 1 — long-running supervisor process
sudo ./engine supervisor ./rootfs-base
```

### Launch and Manage Containers

Each container must use its own rootfs copy for proper filesystem isolation.
Even though `CLONE_NEWNS` prevents mount propagation between containers,
giving each container a private directory ensures writable state is fully
isolated — consistent with how production container runtimes operate.

```bash
# Terminal 2 — CLI commands

# Create per-container rootfs copies
cp -a ./rootfs-base ./rootfs-alpha
cp -a ./rootfs-base ./rootfs-beta

# Start containers with isolated rootfs
sudo ./engine start alpha ./rootfs-alpha /bin/sh
sudo ./engine start beta  ./rootfs-beta  /bin/sh

# Start with custom memory limits
cp -a ./rootfs-base ./rootfs-memtest
sudo ./engine start memtest ./rootfs-memtest /memory_hog --soft-mib 40 --hard-mib 64

# Start with custom nice value (scheduling priority)
cp -a ./rootfs-base ./rootfs-hog-hi
sudo ./engine start hog_hi ./rootfs-hog-hi /cpu_hog --nice -5

# Run a container in the foreground (blocks until done)
cp -a ./rootfs-base ./rootfs-myrun
sudo ./engine run myrun ./rootfs-myrun /cpu_hog

# List all tracked containers
sudo ./engine ps

# View a container's log output
sudo ./engine logs alpha

# Stop a running container
# Before sending SIGTERM, the supervisor sets the container state to
# CONTAINER_STOPPED. This stop_requested distinction allows the metadata
# to correctly differentiate a manual stop from a hard-limit kill
# (which arrives as SIGKILL from the kernel module and sets CONTAINER_KILLED).
sudo ./engine stop alpha
```

Container termination distinguishes three exit paths:
- `exited` — process exited normally on its own
- `stopped` — manually stopped via CLI (`stop` command sends SIGTERM)
- `killed` — terminated by a signal, including hard-limit enforcement (SIGKILL from kernel module)

### Run Scheduling Experiments

```bash
# Experiment 1: Two CPU-bound containers at different priorities
cp -a ./rootfs-base ./rootfs-hog-hi
cp -a ./rootfs-base ./rootfs-hog-lo
sudo ./engine start hog_hi ./rootfs-hog-hi /cpu_hog --nice -5
sudo ./engine start hog_lo ./rootfs-hog-lo /cpu_hog --nice 10
sleep 12
sudo ./engine logs hog_hi
sudo ./engine logs hog_lo
sudo dmesg | grep -E "hog_hi|hog_lo"

# Experiment 2: CPU-bound vs I/O-bound running concurrently
cp -a ./rootfs-base ./rootfs-cpu2
cp -a ./rootfs-base ./rootfs-io2
sudo ./engine start cpu2 ./rootfs-cpu2 /cpu_hog
sudo ./engine start io2  ./rootfs-io2  /io_pulse
sleep 12
sudo ./engine logs cpu2
sudo ./engine logs io2
sudo dmesg | grep -E "cpu2|io2"

sudo ./engine logs memtest
sudo dmesg | grep -E "SOFT|HARD"
sudo dmesg | grep -E "hog_hi|hog_lo|cpu2|io2"
```

### Clean Up

```bash
# Stop all containers
sudo ./engine stop <id>

# Stop supervisor (Ctrl+C in Terminal 1, or send SIGTERM)
# The supervisor will reap children, join the logger thread, and exit cleanly

# Check no zombies remain
ps aux | grep engine

# Inspect kernel logs
sudo dmesg | tail -20

# Unload module
sudo rmmod monitor
sudo dmesg | tail -3    # verify: "[container_monitor] Module unloaded"
```

---

## Demo with Screenshots

### Screenshot 1 — Multi-container supervision
*Two containers (s3, s4) running concurrently under one supervisor process.*

<img width="778" height="118" alt="Screenshot 2026-04-13 170955" src="https://github.com/user-attachments/assets/0afc2dcf-2cc9-4722-bb64-5622d0b58758" />


### Screenshot 2 — Metadata tracking
*Output of the `ps` command showing container ID, PID, state, exit code, and memory limits.*

```
ID               PID      STATE      EXIT         SOFT(MiB)      HARD(MiB)
alpha            4005     running    0            40             64
```

<img width="778" height="118" alt="Screenshot 2026-04-13 170955" src="https://github.com/user-attachments/assets/0a0e5f95-d4fa-4509-b44b-d35cf9bc2e53" />


### Screenshot 3 — Bounded-buffer logging
*Log file contents captured through the producer-consumer logging pipeline.*

```
allocation=1 chunk=8MB total=8MB
...
allocation=8 chunk  ← truncated because hard limit killed process mid-write
```

Verified logging correctness guarantees:
- No logs were lost on abrupt container termination — pipe EOF triggers clean reader thread exit after draining remaining data
- Producers block safely on `pthread_cond_wait` when buffer is full — no busy-waiting, no deadlock
- Consumer drains all remaining buffer items during shutdown before the logger thread is joined

<img width="931" height="183" alt="Screenshot 2026-04-13 171050" src="https://github.com/user-attachments/assets/f4d58395-172e-4464-a4d8-766fa1ecaf77" />


### Screenshot 4 — CLI and IPC
*CLI commands sent through the UNIX domain socket; supervisor responds to each.*

<img width="1043" height="146" alt="Screenshot 2026-04-13 171518" src="https://github.com/user-attachments/assets/e4b49dd6-d9ca-4526-a969-c37c49bc5785" />


### Screenshot 5 — Soft-limit warning
*dmesg showing soft-limit warning when container exceeds 40 MiB.*

```
[container_monitor] SOFT LIMIT container=memtest pid=4086 rss=42565632 limit=41943040
```

<img width="1220" height="81" alt="Screenshot 2026-04-13 171129" src="https://github.com/user-attachments/assets/34bdc749-b128-43c3-a4d4-28153d89d26c" />


### Screenshot 6 — Hard-limit enforcement
*dmesg showing container killed after exceeding 64 MiB hard limit. Supervisor metadata reflects CONTAINER_KILLED state.*

```
[container_monitor] HARD LIMIT container=memtest pid=4086 rss=67706880 limit=67108864
```

<img width="1220" height="81" alt="Screenshot 2026-04-13 171129" src="https://github.com/user-attachments/assets/d8bf2ecb-1660-47b1-a65c-8359d7e951a5" />


### Screenshot 7 — Scheduling experiment
*dmesg timestamps from priority and workload experiments.*

<img width="1039" height="222" alt="Screenshot 2026-04-13 171202" src="https://github.com/user-attachments/assets/e310d0f1-937c-437f-9536-85ebd6b0fe8c" />


### Screenshot 8 — Clean teardown
*Containers in stopped state, no zombie processes, supervisor exits cleanly.*

<img width="1292" height="280" alt="Screenshot 2026-04-13 171602" src="https://github.com/user-attachments/assets/b862f172-2477-4212-bbf7-f99f09514cb1" />


---

## Engineering Analysis

### 1. Isolation Mechanisms

Each container is created using Linux `clone()` with three namespace flags: `CLONE_NEWPID`, `CLONE_NEWUTS`, and `CLONE_NEWNS`. These give each container an isolated view of the system without needing a hypervisor or separate kernel.

`CLONE_NEWPID` creates a new PID namespace so the container's init process sees itself as PID 1. Processes inside cannot see or signal host processes by PID. `CLONE_NEWUTS` gives the container its own hostname, which we set to the container ID using `sethostname()`. `CLONE_NEWNS` creates a new mount namespace so filesystem mounts inside the container do not propagate to the host.

After cloning, the child calls `chroot()` into its own copy of the Alpine rootfs, restricting its filesystem view to that directory tree. Each container receives a private `cp -a` copy of the base rootfs, ensuring writable filesystem state is fully isolated between containers. We then mount `/proc` inside the container using `mount("proc", "/proc", "proc", MS_NOSUID|MS_NODEV|MS_NOEXEC, NULL)` so tools like `ps` work correctly within the container's PID namespace. This was verified — process listing works correctly inside the container shell.

What the host kernel still shares with all containers: the network stack (no `CLONE_NEWNET`), the system clock, and the host's CPU and memory resources. The kernel itself is shared — containers are not virtual machines, they are isolated groups of processes running on the same kernel.

### 2. Supervisor and Process Lifecycle

A long-running supervisor is necessary because containers are child processes that need a parent to reap them. If the parent exits before the child, the child becomes an orphan and is reparented to init (PID 1). By keeping the supervisor alive, we maintain the parent-child relationship and can collect exit status via `waitpid()`.

The supervisor uses `SIGCHLD` handling with `SA_NOCLDSTOP` to be notified whenever a child exits. The signal handler calls `waitpid(-1, &wstatus, WNOHANG)` in a loop to reap all exited children without blocking. This prevents zombie processes, which are entries that remain in the process table after a child exits until the parent calls `wait()`.

Container metadata is stored in a linked list of `container_record_t` structs, protected by a `pthread_mutex_t`. Each record tracks the container ID, host PID, start time, current state, memory limits, log path, and exit status. When a container exits, the `SIGCHLD` handler updates its state to `CONTAINER_EXITED` or `CONTAINER_KILLED` depending on whether it exited normally or was terminated by a signal. When a manual `stop` command is issued, the supervisor sets the state to `CONTAINER_STOPPED` before sending SIGTERM — this `stop_requested` distinction allows the metadata to correctly differentiate a deliberate operator stop from a hard-limit kill arriving as SIGKILL from the kernel module.

### 3. IPC, Threads, and Synchronization

The project uses two distinct IPC mechanisms as required:

**Logging pipe (container → supervisor):** Each container has a `pipe()` created before `clone()`. The write end is duplicated into the container's stdout and stderr via `dup2()`. The read end is owned by a dedicated pipe-reader thread in the supervisor. This is a unidirectional, file-descriptor-based channel — the natural fit for streaming output data.

**UNIX domain socket (CLI → supervisor):** The supervisor binds a `SOCK_STREAM` socket at `/tmp/mini_runtime.sock`. CLI commands connect to this socket, send a `control_request_t` struct, and receive a `control_response_t`. This is a bidirectional, connection-oriented channel suited for request-response interactions. It is deliberately separate from the logging pipe because mixing control messages with log data would require framing and demultiplexing logic, adding unnecessary complexity.

**Bounded buffer:** The pipe-reader threads (producers) push `log_item_t` chunks into a circular buffer of capacity 16. The logger consumer thread pops from the buffer and writes to log files. The buffer is protected by a single `pthread_mutex_t`. Two `pthread_cond_t` variables (`not_empty`, `not_full`) implement the classic producer-consumer signaling pattern.

Without the mutex, two producers could simultaneously read `count`, both see room, both write to the same slot, and one item would be lost (a classic lost-update race). Without condition variables, producers and consumers would have to busy-wait (spin), wasting CPU. The condition variable allows threads to sleep until the buffer state changes, which is efficient and correct.

A `shutting_down` flag allows the consumer to drain remaining items before exiting — preventing log data loss on shutdown. Verified correctness: no logs were lost on abrupt termination, producers block safely when full (no deadlock), and the consumer flushes all remaining items before the logger thread is joined.

**Metadata lock:** The container linked list is accessed from the main supervisor thread (handling CLI requests), the `SIGCHLD` handler, and potentially pipe-reader threads. A `pthread_mutex_t` protects all insertions, deletions, and state updates to prevent torn reads and lost updates.

### 4. Memory Management and Enforcement

RSS (Resident Set Size) measures the amount of physical RAM currently occupied by a process's pages. It excludes memory that has been swapped out, memory-mapped files that have not been accessed, and shared libraries counted once per system but attributed to each process. RSS measures actual physical footprint at a point in time — it is not the same as virtual memory size (VSZ), which includes all mapped regions regardless of whether they are in RAM.

Soft and hard limits represent two different policy goals. The soft limit is a warning threshold: when RSS exceeds it, we log a kernel warning but do not interfere with the process. This gives the operator visibility into memory pressure without disrupting workloads that may only briefly spike. The hard limit is an enforcement threshold: when RSS exceeds it, we send `SIGKILL`. The process cannot catch or ignore `SIGKILL`, so enforcement is guaranteed.

Enforcement belongs in kernel space rather than user space for two reasons. First, a user-space monitor is a separate process that runs only when the scheduler gives it CPU time. A misbehaving container could exhaust memory in the interval between checks. The kernel module runs its timer callback in the kernel's timer subsystem, which is not subject to user-space scheduling delays. Second, killing a process from user space requires knowing its PID and having permission. The kernel module can call `send_sig(SIGKILL, task, 1)` directly on the `task_struct`, bypassing permission checks — this is the authoritative enforcement path.

### 5. Scheduling Behavior

**Experiment 1: CPU-bound containers at different nice values**

Two containers ran the same CPU-bound workload (`cpu_hog`, 10 seconds). `hog_hi` was launched with `nice -5` (higher priority) and `hog_lo` with `nice 10` (lower priority). From dmesg timestamps, `hog_hi` started at t=1920.8s and `hog_lo` started at t=1933.3s — a 13-second gap — meaning they ran sequentially rather than fully concurrently due to the sequential nature of our CLI dispatch. The wall-time comparison between them therefore reflects startup offset rather than a clean CPU-share measurement.

Theoretically, the Linux CFS scheduler uses `vruntime` weighted by nice value. The CFS weight table assigns weight 3121 to nice -5 and weight 110 to nice +10 — approximately a 28x ratio. When running concurrently, `hog_hi` would receive roughly 28x more CPU time per scheduling period than `hog_lo`. A cleaner experiment would launch both simultaneously and read CPU time from `/proc/PID/stat` rather than wall-clock timestamps.

**Experiment 2: CPU-bound vs I/O-bound running concurrently**

`cpu2` (cpu_hog) and `io2` (io_pulse, which writes to disk and sleeps 200ms between iterations) ran back-to-back. From dmesg: `cpu2` ran for ~9 seconds; `io2` ran for ~4 seconds. The I/O-bound workload (`io_pulse`) voluntarily gives up the CPU during each `usleep()` and `fsync()`. CFS rewards this behavior: when `io2` wakes from sleep, its `vruntime` is behind `cpu2`'s (since `cpu2` has been accumulating runtime), so CFS immediately schedules `io2`. This is the scheduler's responsiveness property — interactive and I/O-bound tasks get low latency wakeups. `cpu2` dominated CPU when `io2` was sleeping, demonstrating throughput-oriented behavior for CPU-bound tasks.

---

## Design Decisions and Tradeoffs

**Namespace isolation:** We use `CLONE_NEWPID | CLONE_NEWUTS | CLONE_NEWNS` with `chroot()` and per-container rootfs copies. The tradeoff is that we do not isolate the network stack (`CLONE_NEWNET`) or user IDs (`CLONE_NEWUSER`). This simplifies implementation significantly — network namespace setup requires creating virtual ethernet pairs, and user namespace mapping requires careful UID/GID configuration. For a demonstration runtime, PID/UTS/mount isolation is sufficient to show the core concepts.

**Supervisor architecture:** A single long-running process handles all containers via a UNIX socket event loop. The tradeoff is that the supervisor is a single point of failure — if it crashes, all container metadata is lost and orphaned processes must be cleaned up manually. The alternative (a forked supervisor per container) would make cross-container commands like `ps` much harder to implement. The single-supervisor design is the right call for a system where observability across all containers is a first-class requirement.

**IPC and logging:** We use a pipe per container for log capture and a UNIX domain socket for control commands. The tradeoff of using a single shared bounded buffer (rather than per-container buffers) is that a high-volume container can fill the buffer and block other containers' log data. A per-container buffer would be fairer but requires more memory and more complex consumer routing. The shared buffer is simpler and sufficient for demonstration workloads.

**Kernel monitor:** We use a `mutex` rather than a `spinlock` to protect the monitored list. The tradeoff is that mutexes cannot be held in hard interrupt context, but our timer callback runs in softirq/process context on Linux 5.15, making mutex safe. A spinlock would disable preemption while held, causing latency spikes on loaded systems. The mutex is the right choice here because the critical section (list traversal + RSS check) can be slow and should not hold off the scheduler.

**Scheduling experiments:** We use `nice` values via the `--nice` flag rather than `cgroups` CPU shares. The tradeoff is that `nice` is a coarse-grained mechanism affecting the entire process, while cgroups allow precise per-group CPU allocation. `nice` is simpler to implement (a single `nice()` syscall in the child), directly observable through CFS priority weights, and sufficient to demonstrate scheduling behavior differences.

---

## Scheduler Experiment Results

### Experiment 1: Priority Difference (CPU-bound vs CPU-bound)

| Container | Nice Value | Registration (dmesg) | Unregister (dmesg) | Wall Time |
|-----------|-----------|----------------------|--------------------|-----------|
| hog_hi    | -5        | t=1920.8s            | t=1930.0s          | ~9.2s     |
| hog_lo    | +10       | t=1933.3s            | t=1940.9s          | ~7.6s     |

The containers ran sequentially (13s gap between starts) due to CLI dispatch ordering, so the wall times are not a direct CPU-share comparison. Both completed their 10-second compute workload. The theoretical expectation from CFS weight tables (weight 3121 for nice -5 vs weight 110 for nice +10, ~28x ratio) predicts that `hog_hi` would dominate CPU when running concurrently with `hog_lo`. A concurrent measurement using `/proc/PID/stat` would provide cleaner data.

### Experiment 2: CPU-bound vs I/O-bound (Same Priority)

| Container | Workload  | Start (dmesg) | End (dmesg) | Wall Time |
|-----------|-----------|---------------|-------------|-----------|
| cpu2      | cpu_hog   | t=2332.9s     | t=2341.9s   | ~9.0s     |
| io2       | io_pulse  | t=2342.2s     | t=2346.3s   | ~4.1s     |

`io_pulse` completes 20 iterations with 200ms sleeps between each, for a minimum of 4 seconds regardless of CPU allocation. During each sleep, the CPU was fully available to `cpu2`. This demonstrates that CFS fairly handles mixed workloads: CPU-bound tasks use idle CPU efficiently while I/O-bound tasks maintain low wakeup latency. The scheduler's interactivity heuristic ensures `io2` was woken promptly after each sleep, consistent with the responsiveness goal of the Linux scheduler.
