/*
 * can_collect.c — CAN 测量数据主动采集模块实现
 *
 * 模拟主控向各 K64 发读指令, 解析 float 测量值, 窄表 CSV 存储
 */

#include "can_collect.h"                 /* 采集模块头文件 */
#include "can_proto.h"                   /* CAN_ID 宏, 功能码枚举 */

#include <zephyr/kernel.h>               /* k_uptime_get, k_mutex */
#include <zephyr/fs/fs.h>                /* fs_open/fs_write/fs_close */
#include <zephyr/logging/log.h>          /* LOG_INF/LOG_ERR */
#include <string.h>                      /* memset, memcpy */
#include <stdio.h>                       /* snprintf */

LOG_MODULE_REGISTER(can_collect, LOG_LEVEL_INF); /* 注册日志模块 */

/* ---- K64 协议常量 (对齐 GSK5G_MCU proto_def.h) ---- */

#define OP_R_TEMP   6                     /* 读温度参数 */
#define OP_R_CURR   8                     /* 读电流参数 */

/* ---- 动态发现节点列表 (从心跳帧自动发现) ---- */

static uint8_t s_active_nodes[COLLECT_MAX_NODES]; /* 活跃节点列表 */
static int s_active_count;                        /* 当前活跃节点数 */
static bool s_node_added[128];                    /* 已加入列表的节点 */

/* ---- 采集条目缓冲 ---- */

#define COLLECT_BUF_SIZE  128            /* 最大缓存条目数 */

struct collect_entry {
	uint32_t uptime;                      /* 时间戳 (ms) */
	uint8_t node_id;                      /* K64 设备 ID */
	uint8_t slot;                         /* 槽位号 */
	uint8_t func;                         /* 功能码 (5=RPTCURR, 7=RPTTEMP) */
	uint8_t tc;                           /* typecode */
	uint8_t opcode;                       /* 操作码 */
	float val_float;                      /* 解析后的浮点值 */
};

static struct collect_entry s_cbuf[COLLECT_BUF_SIZE]; /* 采集缓冲 */
static int s_cbuf_count;                 /* 当前条目数 */
static K_MUTEX_DEFINE(s_cbuf_lock);      /* 缓冲互斥锁 */

static int s_boot_seq;                   /* 本次启动文件序号 */

/* ---- 内部函数 ---- */

static void send_query(const struct device *can_dev,
		       uint8_t target_node, uint8_t slot,
		       uint8_t func_code, uint8_t tc, uint8_t opcode); /* 发送查询 */
static uint8_t s_query_phase;            /* 0=TEMP, 1=CURR 交替查询 */

/* ================================================================
 * can_collect_init — 初始化
 * ================================================================ */

int can_collect_init(const struct device *can_dev)
{
	(void)can_dev;                            /* 保留参数 */

	memset(s_cbuf, 0, sizeof(s_cbuf));        /* 清零缓冲 */
	s_cbuf_count = 0;                         /* 重置计数 */

	/* 创建 collect 目录 */
	fs_mkdir("/NAND:/collect");               /* 忽略 EEXIST */

	/* 扫描已有 csv 取最大序号 */
	s_boot_seq = 1;
	struct fs_dir_t dir;
	fs_dir_t_init(&dir);
	if (fs_opendir(&dir, "/NAND:/collect/") == 0) {
		while (1) {
			struct fs_dirent entry = { 0 };
			if (fs_readdir(&dir, &entry) < 0 || entry.name[0] == '\0') break;
			int seq;
			if (sscanf(entry.name, "data_%d.csv", &seq) == 1) {
				if (seq >= s_boot_seq) s_boot_seq = seq + 1;
			}
		}
		fs_closedir(&dir);
	}

	s_query_phase = 0;                        /* 从温度开始 */
	LOG_INF("CAN collect started, boot_seq=%d", s_boot_seq);
	return 0;
}

/* ================================================================
 * can_collect_feed — 解析回复帧
 * ================================================================ */

void can_collect_feed(const struct can_frame *frame)
{
	uint8_t func = (frame->id >> 7) & 0x0F;   /* 功能码 */
	uint8_t node_id = frame->id & 0x7F;         /* 来源设备 */
	uint8_t dlc = frame->dlc;                   /* 数据长度 */

	/* 心跳帧 → 自动发现新节点 */
	if (func == FUNC_HEARTBT) {
		if (!s_node_added[node_id] && s_active_count < COLLECT_MAX_NODES) {
			s_active_nodes[s_active_count++] = node_id;
			s_node_added[node_id] = true;
			LOG_INF("Collect: discovered node %u via heartbeat", node_id);
		}
		return;
	}

	/* 只处理采集相关的回复帧 (RPTCURR / RPTTEMP, 且 DLC=8) */
	if (dlc != 8) return;
	if (func != FUNC_RPTCURR && func != FUNC_RPTTEMP) return;

	/* 提取 proto_code_t (data[0:3]) */
	uint8_t f = frame->data[0];                 /* func (应与 CAN ID 一致) */
	uint8_t tc = frame->data[1];                /* typecode */
	uint8_t opcode = frame->data[2];            /* 操作码 */
	uint8_t slot = frame->data[3];              /* 槽位 */

	/* 校验 func 一致性 */
	if (f != func) return;

	/* 提取 float 值 (data[4:7], IEEE 754) */
	float val;
	memcpy(&val, &frame->data[4], sizeof(val)); /* 拷贝 4 字节为 float */

	/* 缓冲条目 */
	k_mutex_lock(&s_cbuf_lock, K_FOREVER);
	if (s_cbuf_count < COLLECT_BUF_SIZE) {
		struct collect_entry *e = &s_cbuf[s_cbuf_count];
		e->uptime = k_uptime_get();
		e->node_id = node_id;
		e->slot = slot;
		e->func = func;
		e->tc = tc;
		e->opcode = opcode;
		e->val_float = val;
		s_cbuf_count++;
	}
	k_mutex_unlock(&s_cbuf_lock);
}

/* ================================================================
 * can_collect_poll — 发送查询指令
 * ================================================================ */

void can_collect_poll(const struct device *can_dev)
{
	s_query_phase = !s_query_phase;           /* 交替 TEMP ↔ CURR */

	if (s_query_phase) {
		/* 查询温度: OP_R_TEMP(6), tc=T_READ1(2) */
		for (int n = 0; n < s_active_count; n++) {
			uint8_t node = s_active_nodes[n];
			for (int s = 0; s < COLLECT_MAX_SLOTS; s++) {
				send_query(can_dev, node, s,
					   FUNC_RPTTEMP, 2, OP_R_TEMP); /* T1 实际温度 */
			}
		}
	} else {
		/* 查询电流: OP_R_CURR(8), tc=C_READ(2) */
		for (int n = 0; n < s_active_count; n++) {
			uint8_t node = s_active_nodes[n];
			for (int s = 0; s < COLLECT_MAX_SLOTS; s++) {
				send_query(can_dev, node, s,
					   FUNC_RPTCURR, 2, OP_R_CURR); /* LD 电流实际值 */
			}
		}
	}
}

/* ================================================================
 * send_query — 构造并发送一条查询帧
 * ================================================================ */

static void send_query(const struct device *can_dev,
		       uint8_t target_node, uint8_t slot,
		       uint8_t func_code, uint8_t tc, uint8_t opcode)
{
	struct can_frame frame;

	frame.id = (FUNC_SETREGS << 7) | (target_node & 0x7F); /* SETREGS→目标 */
	frame.dlc = 8;                            /* 8 字节 */
	frame.flags = 0;                          /* 标准帧 */

	/* 构造 proto_code_t (data[0:3]) */
	frame.data[0] = FUNC_SETREGS;             /* func */
	frame.data[1] = tc;                       /* typecode */
	frame.data[2] = opcode;                   /* OP_R_CURR / OP_R_TEMP */
	frame.data[3] = slot;                     /* 槽位 */
	frame.data[4] = 0;                        /* 读指令, data[4:7] 无意义 */
	frame.data[5] = 0;
	frame.data[6] = 0;
	frame.data[7] = 0;

	can_send(can_dev, &frame, K_MSEC(50), NULL, NULL); /* 发送 (短超时) */
}

/* ================================================================
 * can_collect_flush — 窄表 CSV 刷盘
 * ================================================================ */

void can_collect_flush(void)
{
	if (s_cbuf_count == 0) return;            /* 无数据 */

	/* 拷贝缓冲到本地 */
	struct collect_entry local[COLLECT_BUF_SIZE];
	int count;

	k_mutex_lock(&s_cbuf_lock, K_FOREVER);
	count = s_cbuf_count;
	memcpy(local, s_cbuf, count * sizeof(struct collect_entry));
	s_cbuf_count = 0;                         /* 清零 */
	k_mutex_unlock(&s_cbuf_lock);

	/* 文件路径 */
	char file_path[48];
	snprintf(file_path, sizeof(file_path),
		 "/NAND:/collect/data_%04d.csv", s_boot_seq); /* 启动序号 */

	struct fs_file_t f;
	fs_file_t_init(&f);
	int ret;

	struct fs_dirent entry;
	if (fs_stat(file_path, &entry) == 0) {
		ret = fs_open(&f, file_path, FS_O_RDWR);
		if (ret == 0) fs_seek(&f, 0, FS_SEEK_END);
	} else {
		ret = fs_open(&f, file_path, FS_O_CREATE | FS_O_WRITE);
		/* 写 CSV 头 */
		const char *header = "uptime,node_id,slot,func,tc,opcode,val_float,val_hex\n";
		fs_write(&f, header, strlen(header));
	}
	if (ret < 0) {
		LOG_ERR("Collect open %s failed: %d", file_path, ret);
		return;
	}

	/* 逐行写入窄表 */
	char line[128];                           /* 行缓冲 */
	for (int i = 0; i < count; i++) {
		struct collect_entry *e = &local[i];
		uint32_t raw;
		memcpy(&raw, &e->val_float, sizeof(raw)); /* float→hex */

		int len = snprintf(line, sizeof(line),
			"%u,%u,%u,%s,%u,%u,%.4f,0x%08X\n",
			e->uptime,                        /* 时间戳 */
			e->node_id,                       /* K64 设备 ID */
			e->slot,                          /* 槽位 */
			(e->func == FUNC_RPTCURR) ? "RPTCURR" :
			(e->func == FUNC_RPTTEMP) ? "RPTTEMP" : "???", /* 功能码 */
			e->tc,                            /* typecode */
			e->opcode,                        /* 操作码 */
			(double)e->val_float,             /* 浮点值 */
			raw);                             /* 原始 hex */
		if (len > 0 && len < (int)sizeof(line)) {
			fs_write(&f, line, len);
		}
	}

	fs_close(&f);
	LOG_INF("Collect: flushed %d entries to %s", count, file_path);
}
