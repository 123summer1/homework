/*
 * 链表搜索 + perf_event_open 统计
 *
 * 按课堂作业要求：使用单链表做顺序查找（不使用 HASH 加速），
 * 用 perf_event_open() 统计指令数、周期数、访存次数、L1 Cache 命中率。
 *
 * 构建/运行（Linux）：
 *   make
 *   ./linked_list_pmc [节点数] [查询次数]
 */
#define _GNU_SOURCE
#include <errno.h>
#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/syscall.h>
#include <unistd.h>
#include <linux/perf_event.h>

#ifndef SYS_perf_event_open
#define SYS_perf_event_open 298
#endif

struct node {
	int key;
	int pad;
	struct node *next;
};

static long perf_event_open_sys(struct perf_event_attr *attr, pid_t pid,
				int cpu, int group_fd, unsigned long flags)
{
	return syscall(SYS_perf_event_open, attr, pid, cpu, group_fd, flags);
}

struct counter {
	int fd;
	const char *name;
};

/* 打开单个事件：只统计本进程用户态（pid=0, cpu=-1） */
static int counter_open(struct counter *c, uint32_t type, uint64_t config,
			const char *name)
{
	struct perf_event_attr pe;

	memset(&pe, 0, sizeof(pe));
	pe.type = type;
	pe.size = sizeof(pe);
	pe.config = config;
	pe.disabled = 1;
	pe.exclude_kernel = 1;
	pe.exclude_hv = 1;
	pe.read_format = PERF_FORMAT_TOTAL_TIME_ENABLED |
			 PERF_FORMAT_TOTAL_TIME_RUNNING;

	c->fd = (int)perf_event_open_sys(&pe, 0, -1, -1, 0);
	c->name = name;
	if (c->fd < 0) {
		fprintf(stderr, "perf_event_open(%s) failed: %s\n",
			name, strerror(errno));
		return -1;
	}
	return 0;
}

static void counter_start(struct counter *c)
{
	ioctl(c->fd, PERF_EVENT_IOC_RESET, 0);
	ioctl(c->fd, PERF_EVENT_IOC_ENABLE, 0);
}

struct read_format3 {
	uint64_t value;
	uint64_t time_enabled;
	uint64_t time_running;
};

/* 读取计数值；事件多路复用时按 enabled/running 缩放 */
static uint64_t counter_read(struct counter *c)
{
	struct read_format3 v;
	ssize_t n = read(c->fd, &v, sizeof(v));

	if (n != (ssize_t)sizeof(v)) {
		fprintf(stderr, "read(%s) failed: %s\n", c->name, strerror(errno));
		exit(1);
	}
	if (v.time_running == 0)
		return 0;
	if (v.time_running < v.time_enabled)
		return (uint64_t)((double)v.value * (double)v.time_enabled /
				  (double)v.time_running);
	return v.value;
}

static void counter_stop(struct counter *c)
{
	ioctl(c->fd, PERF_EVENT_IOC_DISABLE, 0);
}

static struct node *build_list(uint64_t n)
{
	struct node *head = NULL, *p;

	for (uint64_t i = 0; i < n; i++) {
		p = (struct node *)malloc(sizeof(*p));
		if (p == NULL) {
			perror("malloc");
			exit(1);
		}
		p->key = (int)i;
		p->next = head;
		head = p;
	}
	return head;
}

/* 顺序查找：O(n)，全程指针追逐，不使用 HASH */
static __attribute__((noinline)) int list_search(struct node *head, int key)
{
	struct node *p = head;

	while (p != NULL && p->key != key)
		p = p->next;
	return p != NULL;
}

static uint64_t next_rand(uint64_t *state)
{
	*state = *state * 6364136223846793005ULL + 1442695040888963407ULL;
	return *state;
}

static void usage(const char *prog)
{
	fprintf(stderr, "usage: %s [nodes] [queries]\n", prog);
	fprintf(stderr, "  nodes   default 262144, 4 MB linked list\n");
	fprintf(stderr, "  queries default 4096\n");
}

int main(int argc, char **argv)
{
	uint64_t n = 262144;
	uint64_t q = 4096;
	uint64_t rnd = 88172645463325252ULL;
	uint64_t range;
	long found = 0;
	volatile long sink = 0;
	struct node *head, *p;

	if (argc > 1)
		n = strtoull(argv[1], NULL, 0);
	if (argc > 2)
		q = strtoull(argv[2], NULL, 0);
	if (n < 1024 || q == 0) {
		usage(argv[0]);
		return 1;
	}
	range = n + n / 8;	/* 约 1/9 的查询目标不存在 */

	head = build_list(n);
	printf("list: %" PRIu64 " nodes (%.1f MB, 16 bytes/node), "
	       "queries: %" PRIu64 "\n",
	       n, (double)n * sizeof(struct node) / (1024.0 * 1024.0), q);

	struct counter c_insn, c_cyc, c_mem_ref, c_mem_miss, c_l1_acc, c_l1_miss;
	int have_l1 = 1;

	if (counter_open(&c_insn, PERF_TYPE_HARDWARE,
			 PERF_COUNT_HW_INSTRUCTIONS, "instructions") < 0 ||
	    counter_open(&c_cyc, PERF_TYPE_HARDWARE,
			 PERF_COUNT_HW_CPU_CYCLES, "cycles") < 0 ||
	    counter_open(&c_mem_ref, PERF_TYPE_HARDWARE,
			 PERF_COUNT_HW_CACHE_REFERENCES, "cache-references") < 0 ||
	    counter_open(&c_mem_miss, PERF_TYPE_HARDWARE,
			 PERF_COUNT_HW_CACHE_MISSES, "cache-misses") < 0) {
		fprintf(stderr,
			"提示：确认 perf_event_paranoid 设置（<=2 或用 root），"
			"虚拟机需向客户机暴露 PMU（如 VMware 的 vpmc / KVM 的 -cpu host）。\n");
		return 1;
	}

	if (counter_open(&c_l1_acc, PERF_TYPE_HW_CACHE,
			 PERF_COUNT_HW_CACHE_L1D |
			 (PERF_COUNT_HW_CACHE_OP_READ << 8) |
			 (PERF_COUNT_HW_CACHE_RESULT_ACCESS << 16),
			 "L1-dcache-loads") < 0) {
		have_l1 = 0;
	} else if (counter_open(&c_l1_miss, PERF_TYPE_HW_CACHE,
				PERF_COUNT_HW_CACHE_L1D |
				(PERF_COUNT_HW_CACHE_OP_READ << 8) |
				(PERF_COUNT_HW_CACHE_RESULT_MISS << 16),
				"L1-dcache-load-misses") < 0) {
		have_l1 = 0;
		close(c_l1_acc.fd);
	}
	if (!have_l1)
		fprintf(stderr,
			"提示：当前环境不支持 L1D Cache 事件（虚拟 PMU 常见），"
			"L1 命中率将显示为 N/A；可在物理机或支持 L1D 事件的平台上运行。\n");

	/* 预热代码路径（不计入统计） */
	for (uint64_t i = 0; i < q / 8 + 1; i++) {
		int key = (int)(next_rand(&rnd) % range);
		sink += list_search(head, key);
	}

	counter_start(&c_insn);
	counter_start(&c_cyc);
	counter_start(&c_mem_ref);
	counter_start(&c_mem_miss);
	if (have_l1) {
		counter_start(&c_l1_acc);
		counter_start(&c_l1_miss);
	}

	for (uint64_t i = 0; i < q; i++) {
		int key = (int)(next_rand(&rnd) % range);
		found += list_search(head, key);
	}

	if (have_l1) {
		counter_stop(&c_l1_miss);
		counter_stop(&c_l1_acc);
	}
	counter_stop(&c_mem_miss);
	counter_stop(&c_mem_ref);
	counter_stop(&c_cyc);
	counter_stop(&c_insn);

	uint64_t insn = counter_read(&c_insn);
	uint64_t cyc = counter_read(&c_cyc);
	uint64_t mem_ref = counter_read(&c_mem_ref);
	uint64_t mem_miss = counter_read(&c_mem_miss);
	uint64_t l1_acc = 0, l1_miss = 0;

	if (have_l1) {
		l1_acc = counter_read(&c_l1_acc);
		l1_miss = counter_read(&c_l1_miss);
	}

	sink += found;

	printf("\n==== perf_event_open (pid-wide, exclude_kernel=1) ====\n");
	printf("instructions        : %20" PRIu64 "\n", insn);
	printf("cycles              : %20" PRIu64 "\n", cyc);
	printf("CPI                 : %20.3f\n",
	       insn ? (double)cyc / (double)insn : 0.0);
	printf("cache references    : %20" PRIu64 "   (访存次数)\n", mem_ref);
	printf("cache misses        : %20" PRIu64 "\n", mem_miss);
	if (have_l1) {
		printf("L1D loads           : %20" PRIu64 "\n", l1_acc);
		printf("L1D load misses     : %20" PRIu64 "\n", l1_miss);
		printf("L1 hit rate         : %20.2f%%\n",
		       l1_acc ? 100.0 * (double)(l1_acc - l1_miss) /
				(double)l1_acc : 0.0);
	} else {
		printf("L1D loads           :                  N/A\n");
		printf("L1D load misses     :                  N/A\n");
		printf("L1 hit rate         :                  N/A\n");
	}
	printf("cycles/query        : %20.2f\n", (double)cyc / (double)q);
	printf("found / queries     : %20ld / %" PRIu64 "\n", found, q);
	printf("(sink=%ld)\n", sink);

	for (p = head; p != NULL; ) {
		struct node *next = p->next;
		free(p);
		p = next;
	}
	close(c_insn.fd);
	close(c_cyc.fd);
	close(c_mem_ref.fd);
	close(c_mem_miss.fd);
	if (have_l1) {
		close(c_l1_acc.fd);
		close(c_l1_miss.fd);
	}
	return 0;
}
