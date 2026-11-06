/*
 * Copyright (c) 2026, the SerenityOS Linux GUI port contributors.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

// Per-process network byte counters, done with eBPF.
//
// /proc only knows bytes per interface (/proc/net/dev) or per socket queues, so
// "which process is using the network" needs kernel help. This is the same idea
// as /mnt/user/taskmgr's bpf/taskmgr.bpf.c, but hand-rolled: the program is
// encoded as raw eBPF instructions (no clang -target bpf, no libbpf, no BTF), it
// is attached to the tcp_sendmsg/tcp_recvmsg kprobes via perf_event_open, and the
// counters land in a BPF hash map keyed by pid.
//
// Once a second the map is dumped to /tmp/serenity-netprobe.txt:
//
//     pid rx_bytes tx_bytes
//
// The task manager reads that file. If anything here fails (no CAP_BPF, no
// tracefs, verifier rejection) the probe exits non-zero and the task manager
// simply falls back to interface-level statistics.

#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <linux/bpf.h>
#include <linux/perf_event.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <unistd.h>

#ifndef __NR_bpf
#    define __NR_bpf 321
#endif

#define OUTPUT_PATH "/tmp/serenity-netprobe.txt"
#define MAP_ENTRIES 8192

#define INSN(CODE, DST, SRC, OFF, IMM) \
    ((struct bpf_insn) { .code = (CODE), .dst_reg = (DST), .src_reg = (SRC), .off = (OFF), .imm = (IMM) })

// eBPF opcodes we need (see include/uapi/linux/bpf_common.h).
#define OP_MOV64_REG 0xbf /* BPF_ALU64 | BPF_MOV | BPF_X */
#define OP_MOV64_IMM 0xb7 /* BPF_ALU64 | BPF_MOV | BPF_K */
#define OP_ADD64_IMM 0x07 /* BPF_ALU64 | BPF_ADD | BPF_K */
#define OP_ADD64_REG 0x0f /* BPF_ALU64 | BPF_ADD | BPF_X */
#define OP_RSH64_IMM 0x77 /* BPF_ALU64 | BPF_RSH | BPF_K */
#define OP_LD_IMM64 0x18  /* BPF_LD | BPF_DW | BPF_IMM */
#define OP_LDX_DW 0x79    /* BPF_LDX | BPF_MEM | BPF_DW */
#define OP_STX_DW 0x7b    /* BPF_STX | BPF_MEM | BPF_DW */
#define OP_ST_W 0x63      /* BPF_STX | BPF_MEM | BPF_W */
#define OP_ST_DW 0x7a     /* BPF_ST | BPF_MEM | BPF_DW */
#define OP_JEQ_IMM 0x15   /* BPF_JMP | BPF_JEQ | BPF_K */
#define OP_CALL 0x85      /* BPF_JMP | BPF_CALL */
#define OP_EXIT 0x95      /* BPF_JMP | BPF_EXIT */

#define HELPER_MAP_LOOKUP 1
#define HELPER_GET_PID_TGID 14
#define HELPER_MAP_UPDATE 2

static int last_errno;
static volatile sig_atomic_t s_quit;

static void on_signal(int signal_number)
{
    (void)signal_number;
    s_quit = 1;
}

static int bpf_call(enum bpf_cmd cmd, union bpf_attr *attr)
{
    int fd = (int)syscall(__NR_bpf, cmd, attr, sizeof(*attr));
    if (fd < 0)
        last_errno = errno;
    return fd;
}

static int create_counter_map(void)
{
    union bpf_attr attr;
    memset(&attr, 0, sizeof(attr));
    attr.map_type = BPF_MAP_TYPE_HASH;
    attr.key_size = sizeof(uint32_t);
    attr.value_size = 2 * sizeof(uint64_t); // rx, tx
    attr.max_entries = MAP_ENTRIES;
    return bpf_call(BPF_MAP_CREATE, &attr);
}

// tcp_sendmsg(struct sock *sk, struct msghdr *msg, size_t size)
//   kprobe 程序的 r1 是 struct pt_regs *，第 3 个参数（rdx）在 +96 处。
//
// 栈布局：[fp-4]   = u32 pid (map key)
//          [fp-24]  = struct counters { u64 rx; u64 tx; }
//
// hash map 没有"不存在就自动创建"的语义：lookup 未命中会返回 NULL，所以第一次
// 见到某个 pid 时要自己 map_update_elem 建条目 —— 少了这一步，map 永远是空的。
static int load_counter_program(int map_fd, uint32_t value_offset)
{
    struct bpf_insn program[] = {
        INSN(OP_LDX_DW, 6, 1, 96, 0),                    // r6 = regs->dx (size)
        INSN(OP_CALL, 0, 0, 0, HELPER_GET_PID_TGID),     // r0 = pid << 32 | tid
        INSN(OP_RSH64_IMM, 0, 0, 0, 32),                 // r0 >>= 32 (pid)
        INSN(OP_ST_W, 10, 0, -4, 0),                     // *(u32 *)(fp - 4) = pid
        INSN(OP_ST_DW, 10, 0, -24, 0),                   // value.rx = 0
        INSN(OP_ST_DW, 10, 0, -16, 0),                   // value.tx = 0
        INSN(OP_LD_IMM64, 1, 1, 0, map_fd),              // r1 = map (pseudo fd)
        INSN(0, 0, 0, 0, 0),                             //   ...upper half
        INSN(OP_MOV64_REG, 2, 10, 0, 0),                 // r2 = fp
        INSN(OP_ADD64_IMM, 2, 0, 0, -4),                 // r2 += -4 (key ptr)
        INSN(OP_CALL, 0, 0, 0, HELPER_MAP_LOOKUP),       // r0 = lookup(map, &pid)
        INSN(OP_JEQ_IMM, 0, 0, 5, 0),                    // if (r0 == 0) goto insert
        INSN(OP_LDX_DW, 1, 0, (int)value_offset, 0),     // r1 = *(u64 *)(r0 + off)
        INSN(OP_ADD64_REG, 1, 6, 0, 0),                  // r1 += r6
        INSN(OP_STX_DW, 0, 1, (int)value_offset, 0),     // *(u64 *)(r0 + off) = r1
        INSN(OP_MOV64_IMM, 0, 0, 0, 0),                  // r0 = 0
        INSN(OP_EXIT, 0, 0, 0, 0),                       // exit
        // --- insert: 第一次见到这个 pid ---
        INSN(OP_MOV64_REG, 8, 10, 0, 0),                 // r8 = fp
        INSN(OP_ADD64_IMM, 8, 0, 0, -24 + (int)value_offset), // r8 = &value.field
        INSN(OP_STX_DW, 8, 6, 0, 0),                     // *r8 = size
        INSN(OP_LD_IMM64, 1, 1, 0, map_fd),              // r1 = map
        INSN(0, 0, 0, 0, 0),                             //   ...upper half
        INSN(OP_MOV64_REG, 2, 10, 0, 0),                 // r2 = fp
        INSN(OP_ADD64_IMM, 2, 0, 0, -4),                 // r2 = &key
        INSN(OP_MOV64_REG, 3, 10, 0, 0),                 // r3 = fp
        INSN(OP_ADD64_IMM, 3, 0, 0, -24),                // r3 = &value
        INSN(OP_MOV64_IMM, 4, 0, 0, 0),                  // r4 = BPF_ANY
        INSN(OP_CALL, 0, 0, 0, HELPER_MAP_UPDATE),       // update(map, &pid, &value, 0)
        INSN(OP_MOV64_IMM, 0, 0, 0, 0),                  // r0 = 0
        INSN(OP_EXIT, 0, 0, 0, 0),                       // exit
    };

    static char verifier_log[64 * 1024];
    memset(verifier_log, 0, sizeof(verifier_log));

    union bpf_attr attr;
    memset(&attr, 0, sizeof(attr));
    attr.prog_type = BPF_PROG_TYPE_KPROBE;
    attr.insn_cnt = sizeof(program) / sizeof(program[0]);
    attr.insns = (uint64_t)(uintptr_t)program;
    attr.license = (uint64_t)(uintptr_t) "GPL";
    attr.log_level = 1;
    attr.log_buf = (uint64_t)(uintptr_t)verifier_log;
    attr.log_size = sizeof(verifier_log);

    int fd = bpf_call(BPF_PROG_LOAD, &attr);
    if (fd < 0 && verifier_log[0] != '\0')
        fprintf(stderr, "netprobe: verifier said:\n%s\n", verifier_log);
    return fd;
}

static void remove_kprobe(char const* name);

// 复用一个已经存在的 kprobe：以前异常退出（或 tracefs 拒绝删除）时会留下同名
// 事件，此时 create 报 EEXIST，但事件本身是可用的，没必要整个采集器退出。
static int kprobe_exists(char const* name)
{
    char path[256];
    snprintf(path, sizeof(path), "/sys/kernel/tracing/events/kprobes/%s/id", name);
    return access(path, R_OK) == 0;
}

static int create_kprobe(char const* name, char const* function)
{
    // 上次异常退出可能留下同名 kprobe，先试着摘掉再注册。
    remove_kprobe(name);
    if (kprobe_exists(name))
        return 0;

    int fd = open("/sys/kernel/tracing/kprobe_events", O_WRONLY | O_APPEND);
    if (fd < 0)
        return -1;

    char line[256];
    int length = snprintf(line, sizeof(line), "p:%s %s\n", name, function);
    ssize_t written = write(fd, line, (size_t)length);
    close(fd);
    if (written == length)
        return 0;

    // EEXIST：另一个实例（或上次崩溃）已经注册好了，直接复用。
    if (kprobe_exists(name))
        return 0;
    return -1;
}

static void remove_kprobe(char const* name)
{
    int fd = open("/sys/kernel/tracing/kprobe_events", O_WRONLY | O_APPEND);
    if (fd < 0)
        return;
    char line[256];
    int length = snprintf(line, sizeof(line), "-:%s\n", name);
    (void)write(fd, line, (size_t)length);
    close(fd);
}

static int kprobe_id(char const* name)
{
    char path[256];
    snprintf(path, sizeof(path), "/sys/kernel/tracing/events/kprobes/%s/id", name);
    int fd = open(path, O_RDONLY);
    if (fd < 0)
        return -1;

    char buffer[32];
    ssize_t nread = read(fd, buffer, sizeof(buffer) - 1);
    close(fd);
    if (nread <= 0)
        return -1;
    buffer[nread] = '\0';
    return atoi(buffer);
}

// One perf event per CPU: attaching with pid=-1/cpu=N catches every task running
// on that CPU.
static int attach_program(int prog_fd, char const* kprobe_name, int cpu, int* out_fd)
{
    int id = kprobe_id(kprobe_name);
    if (id < 0) {
        fprintf(stderr, "netprobe: no tracepoint id for %s\n", kprobe_name);
        return -1;
    }

    struct perf_event_attr attr;
    memset(&attr, 0, sizeof(attr));
    attr.type = PERF_TYPE_TRACEPOINT;
    attr.size = sizeof(attr);
    attr.config = (uint64_t)id;
    attr.sample_period = 1;
    attr.wakeup_events = 1;

    int fd = (int)syscall(__NR_perf_event_open, &attr, -1, cpu, -1, 0);
    if (fd < 0) {
        fprintf(stderr, "netprobe: perf_event_open failed on cpu %d: %s\n", cpu, strerror(errno));
        return -1;
    }
    if (ioctl(fd, PERF_EVENT_IOC_SET_BPF, prog_fd) != 0) {
        // EEXIST = 这个 kprobe 的 prog 槽位已经填了（在 tracepoint 上，全局一份），
        // 说明程序已经生效，覆盖所有 CPU。
        if (errno != EEXIST) {
            fprintf(stderr, "netprobe: PERF_EVENT_IOC_SET_BPF failed: %s\n", strerror(errno));
            close(fd);
            return -1;
        }
    }
    if (ioctl(fd, PERF_EVENT_IOC_ENABLE, 0) != 0) {
        fprintf(stderr, "netprobe: PERF_EVENT_IOC_ENABLE failed: %s\n", strerror(errno));
        close(fd);
        return -1;
    }

    *out_fd = fd;
    return 0;
}

struct counters {
    uint64_t rx;
    uint64_t tx;
};

static int dump_map(int map_fd)
{
    char temporary[256];
    snprintf(temporary, sizeof(temporary), OUTPUT_PATH ".tmp");
    FILE* output = fopen(temporary, "w");
    if (!output)
        return -1;

    uint32_t key = 0;
    for (;;) {
        uint32_t next_key;
        union bpf_attr attr;
        memset(&attr, 0, sizeof(attr));
        attr.map_fd = (uint32_t)map_fd;
        attr.key = (uint64_t)(uintptr_t)&key;
        attr.next_key = (uint64_t)(uintptr_t)&next_key;
        if (bpf_call(BPF_MAP_GET_NEXT_KEY, &attr) != 0)
            break;

        struct counters value;
        memset(&attr, 0, sizeof(attr));
        attr.map_fd = (uint32_t)map_fd;
        attr.key = (uint64_t)(uintptr_t)&next_key;
        attr.value = (uint64_t)(uintptr_t)&value;
        if (bpf_call(BPF_MAP_LOOKUP_ELEM, &attr) == 0)
            fprintf(output, "%u %llu %llu\n", next_key, (unsigned long long)value.rx, (unsigned long long)value.tx);

        key = next_key;
    }

    fclose(output);
    rename(temporary, OUTPUT_PATH);
    return 0;
}

int main(void)
{
    int map_fd = create_counter_map();
    if (map_fd < 0) {
        fprintf(stderr, "netprobe: cannot create BPF map: %s (needs root, CAP_BPF)\n", strerror(last_errno));
        return 1;
    }

    int tx_program = load_counter_program(map_fd, sizeof(uint64_t));
    if (tx_program < 0) {
        fprintf(stderr, "netprobe: cannot load tx program: %s\n", strerror(last_errno));
        return 1;
    }
    int rx_program = load_counter_program(map_fd, 0);
    if (rx_program < 0) {
        fprintf(stderr, "netprobe: cannot load rx program: %s\n", strerror(last_errno));
        return 1;
    }

    if (create_kprobe("serenity_net_tx", "tcp_sendmsg") != 0 || create_kprobe("serenity_net_rx", "tcp_recvmsg") != 0) {
        fprintf(stderr, "netprobe: cannot register kprobes: %s\n", strerror(errno));
        return 1;
    }

    // 只挂一次。kprobe 的 BPF 程序槽位在 tracepoint 上（不是每个 perf event 一份），
    // 所以一次 attach 就覆盖全部 CPU；再往后每个 CPU 都会返回 EEXIST。
    int tx_fd = -1;
    int rx_fd = -1;
    if (attach_program(tx_program, "serenity_net_tx", 0, &tx_fd) != 0
        || attach_program(rx_program, "serenity_net_rx", 0, &rx_fd) != 0) {
        fprintf(stderr, "netprobe: cannot attach programs\n");
        return 1;
    }

    fprintf(stderr, "netprobe: attached (system-wide), writing %s\n", OUTPUT_PATH);

    signal(SIGTERM, on_signal);
    signal(SIGINT, on_signal);

    while (!s_quit) {
        dump_map(map_fd);
        sleep(1);
    }

    // 退出前把 kprobe 摘掉，免得下次启动时重名注册失败。
    remove_kprobe("serenity_net_tx");
    remove_kprobe("serenity_net_rx");
    unlink(OUTPUT_PATH);
    return 0;
}
