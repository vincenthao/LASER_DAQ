/*
 * can_sniffer.c — CAN 总线嗅探模块实现
 *
 * 全量捕获 CAN 帧, 按 node_id 分目录 → CSV, 每秒刷盘
 */

#include "can_sniffer.h"                 /* 嗅探模块头文件 */

#include <zephyr/kernel.h>               /* k_uptime_get, k_mutex */
#include <zephyr/fs/fs.h>                /* fs_open/fs_write/fs_close */
#include <zephyr/logging/log.h>          /* LOG_INF, LOG_ERR */
#include <string.h>                      /* memset, memcpy */
#include <stdio.h>                       /* snprintf */

LOG_MODULE_REGISTER(can_sniffer, LOG_LEVEL_INF); /* 注册日志模块 */

/* ---- 环形缓冲区结构 (per node_id) ---- */

struct sniff_buf {
	struct can_frame frames[SNIFF_BUF_SIZE]; /* 帧缓冲区 */
	int head;                                /* 写入位置 */
	int count;                               /* 当前帧数 */
	bool dirty;                              /* 有新数据等待刷盘 */
};

/* 最多支持 16 个 CAN 节点 (实际总线上不会超过这个数) */
#define SNIFF_MAX_NODES 16
static struct sniff_buf s_bufs[SNIFF_MAX_NODES];
static K_MUTEX_DEFINE(s_lock);           /* 缓冲区互斥锁 */

/* ---- 内部函数 ---- */

static void ensure_node_dir(uint8_t node_id);        /* 确保目录存在 */
static void flush_node(uint8_t node_id);              /* 刷单个 node 缓冲区到 CSV */

/* ================================================================
 * can_sniffer_init — 初始化
 * ================================================================ */

int can_sniffer_init(void)
{
	memset(s_bufs, 0, sizeof(s_bufs));        /* 清零所有缓冲区 */

	/* 创建根目录下的 sniff 文件夹 */
	int ret = fs_mkdir("/NAND:/sniff");       /* 创建 sniff 目录 */
	if (ret < 0 && ret != -EEXIST) {
		LOG_ERR("mkdir /NAND:/sniff failed: %d", ret);
	}

	LOG_INF("CAN sniffer started (flush via main loop)"); /* 就绪 */
	return 0;                                 /* 成功 */
}

/* ================================================================
 * can_sniffer_feed — 喂入一帧
 * ================================================================ */

void can_sniffer_feed(const struct can_frame *frame)
{
	uint8_t node_id = frame->id & 0x7F;       /* 提取低 7 位 node_id */
	if (node_id >= SNIFF_MAX_NODES) {
		return;                               /* 超过最大支持节点数, 丢弃 */
	}

	k_mutex_lock(&s_lock, K_FOREVER);         /* 加锁 */

	struct sniff_buf *b = &s_bufs[node_id];   /* 对应 node 的缓冲区 */

	/* 缓冲区满则丢弃最旧的帧 */
	if (b->count >= SNIFF_BUF_SIZE) {
		b->head = (b->head + 1) % SNIFF_BUF_SIZE; /* 覆盖最旧 */
		b->count--;                           /* 先减, 后面会加回来 */
	}

	/* 写入帧到环形缓冲区 */
	int idx = (b->head + b->count) % SNIFF_BUF_SIZE; /* 计算写入位置 */
	memcpy(&b->frames[idx], frame, sizeof(struct can_frame)); /* 拷贝帧 */
	b->count++;                               /* 帧计数 +1 */
	b->dirty = true;                          /* 标记有新数据 */

	/* 首次收到该节点的帧时打印提示 */
	bool first_frame = (b->count == 1);       /* 是否首帧 */
	k_mutex_unlock(&s_lock);                  /* 解锁 */

	if (first_frame) {
		LOG_INF("Sniff: new node %u (ID=0x%03X)", node_id, frame->id); /* 新增节点 */
	}
}

/* ================================================================
 * can_sniffer_flush — 批量刷盘 (主循环调用)
 * ================================================================ */

void can_sniffer_flush(void)
{
	for (int i = 0; i < SNIFF_MAX_NODES; i++) {
		k_mutex_lock(&s_lock, K_FOREVER);     /* 加锁 */
		bool needs_flush = s_bufs[i].dirty;   /* 是否需要刷 */
		k_mutex_unlock(&s_lock);              /* 解锁 */

		if (needs_flush) {
			flush_node(i);                    /* 刷盘 */
		}
	}
}

/* ================================================================
 * ensure_node_dir — 确保 /NAND:/sniff/<node_id> 目录存在
 * ================================================================ */

static void ensure_node_dir(uint8_t node_id)
{
	char path[32];                            /* 目录路径 */
	snprintf(path, sizeof(path), "/NAND:/sniff/%u", node_id); /* 拼接路径 */
	int ret = fs_mkdir(path);                 /* 创建目录 */
	if (ret < 0 && ret != -EEXIST) {
		LOG_ERR("mkdir %s failed: %d", path, ret); /* 创建失败 */
	}
}

/* ================================================================
 * flush_node — 将指定 node_id 缓冲区刷到 CSV 文件
 * ================================================================ */

static void flush_node(uint8_t node_id)
{
	struct can_frame local_buf[SNIFF_BUF_SIZE]; /* 本地缓冲区 */
	int count;                                /* 待写入帧数 */

	/* 加锁拷贝缓冲区到本地, 快速释放锁 */
	k_mutex_lock(&s_lock, K_FOREVER);
	struct sniff_buf *b = &s_bufs[node_id];
	count = b->count;
	if (count == 0) {
		k_mutex_unlock(&s_lock);
		return;
	}
	/* 按顺序拷贝 */
	for (int i = 0; i < count; i++) {
		int src = (b->head + i) % SNIFF_BUF_SIZE;
		memcpy(&local_buf[i], &b->frames[src], sizeof(struct can_frame));
	}
	b->head = 0;                              /* 重置缓冲区 */
	b->count = 0;                             /* 清空 */
	b->dirty = false;                         /* 已刷盘 */
	k_mutex_unlock(&s_lock);

	/* 确保目录存在 */
	ensure_node_dir(node_id);

	/* 打开 CSV 文件 (追加模式) */
	char file_path[48];                       /* 文件完整路径 */
	snprintf(file_path, sizeof(file_path),
		 "/NAND:/sniff/%u/sniff.csv", node_id); /* 拼接路径 */

	struct fs_file_t f;
	int ret = fs_open(&f, file_path,
			  FS_O_CREATE | FS_O_WRITE | FS_O_APPEND); /* 创建+追加 */
	if (ret < 0) {
		LOG_ERR("Sniff open %s failed: %d", file_path, ret);
		return;
	}

	/* 逐帧写入 CSV */
	char line[SNIFF_CSV_LINE_MAX];            /* CSV 行缓冲区 */
	for (int i = 0; i < count; i++) {
		struct can_frame *frame = &local_buf[i]; /* 当前帧 */
		uint32_t uptime = k_uptime_get();       /* 时间戳 (ms) */

		int len = snprintf(line, sizeof(line),
			"%u,0x%03X,%u,%02X,%02X,%02X,%02X,%02X,%02X,%02X,%02X\n",
			uptime,                              /* 时间戳 */
			frame->id,                           /* CAN ID (hex) */
			frame->dlc,                          /* DLC */
			frame->data[0], frame->data[1],      /* 数据字节 0-7 */
			frame->data[2], frame->data[3],
			frame->data[4], frame->data[5],
			frame->data[6], frame->data[7]);
		if (len > 0 && len < (int)sizeof(line)) {
			fs_write(&f, line, len);            /* 写入 CSV 行 */
		}
	}

	fs_close(&f);                             /* 关闭文件 */
	LOG_INF("Sniff: flushed %d frames to %s", count, file_path); /* 刷盘日志 */
}
