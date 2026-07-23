/*
 * can_collect.c — CAN 测量数据主动采集模块实现
 *
 * 通过 SETREGS 配置 K64 Monitor 周期 → K64 自动广播 RPTCURR/RPTTEMP
 * MCXN947 接收解析 float 测量值, 窄表 CSV 存储
 */

#include "can_collect.h"                 /* 采集模块头文件 */
#include "can_proto.h"                   /* CAN_ID 宏, 功能码枚举 */

#include <zephyr/kernel.h>               /* k_uptime_get, k_mutex */
#include <zephyr/fs/fs.h>                /* fs_open/fs_write/fs_close */
#include <zephyr/logging/log.h>          /* LOG_INF/LOG_ERR */
#include <string.h>                      /* memset, memcpy */
#include <stdio.h>                       /* snprintf */

LOG_MODULE_REGISTER(can_collect, LOG_LEVEL_INF); /* 注册日志模块 */

/* ---- 动态发现节点列表 (从心跳帧自动发现) ---- */

static uint8_t s_active_nodes[COLLECT_MAX_NODES]; /* 活跃节点列表 */
static int s_active_count;                        /* 当前活跃节点数 */
static bool s_node_added[128];                    /* 已加入列表的节点 */
static bool s_node_configured[128];               /* 已下发 Monitor 配置 */

/* ---- 采集条目缓冲 ---- */

#define COLLECT_BUF_SIZE  128            /* 最大缓存条目数 */

struct collect_entry {
	uint32_t uptime;                      /* 时间戳 (ms) */
	uint8_t node_id;                      /* K64 设备 ID */
	uint8_t slot;                         /* 槽位号 */
	uint8_t func;                         /* 功能码 (5=RPTCURR,7=RPTTEMP,9=RPTREGS) */
	uint8_t tp;                           /* TP 类型码 (主动上报) 或 tc (RPTREGS 响应) */
	uint8_t opcode;                       /* 操作码 (RPTREGS 响应时有效) */
	float val_float;                      /* 解析后的浮点值 */
};

static struct collect_entry s_cbuf[COLLECT_BUF_SIZE]; /* 采集缓冲 */
static int s_cbuf_count;                 /* 当前条目数 */
static K_MUTEX_DEFINE(s_cbuf_lock);      /* 缓冲互斥锁 */

static int s_boot_seq;                   /* 本次启动文件序号 */

/* ---- 内部函数 ---- */

static void send_setregs(const struct device *can_dev,
			 uint8_t target_node, uint8_t slot,
			 uint8_t tc, uint8_t opcode,
			 uint32_t data_val);               /* 发送 SETREGS 写指令 */

static void configure_node(const struct device *can_dev,
			   uint8_t node_id);                /* 配置一个节点的全部 Monitor */

static const char *tp_name(uint8_t tp);                  /* TP 码 → 名称 */

/* ---- 主动上报帧中 TP 码 → 描述字符串 ---- */

static const char *tp_name(uint8_t tp)
{
	switch (tp) {
	case TP_NOSAMP:   return "RAW";          /* 原始值 */
	case TP_TEMPSW:   return "T_SW";         /* 温度开关状态 */
	case TP_CURSW:    return "C_SW";         /* 电流开关状态 */
	case TP_IREAL:    return "I_ACT";         /* LD 电流实际值 */
	case TP_LDV:      return "LDV";           /* LD 电压 */
	case TP_PSUMP:    return "PSUMP";         /* LD 功率 I×V */
	case TP_P:        return "LDP";           /* LD 光功率 */
	case TP_DIRV:     return "DRIV";          /* 驱动电压 */
	case TP_VCE:      return "VCE";           /* VCE 电压 */
	case TP_ISET:     return "I_TGT";         /* 电流目标值 */
	case TP_TREAL:    return "T1";            /* T1 温度 */
	case TP_T2REAL:   return "T2";            /* T2 温度 */
	case TP_T3REAL:   return "T3";            /* T3 温度 */
	case TP_TECDUTY:  return "TEC_DUTY";      /* TEC 占空比 */
	case TP_TECI:     return "TEC_I";         /* TEC 电流 */
	case TP_TECV:     return "TEC_V";         /* TEC 电压 */
	case TP_TECP:     return "TEC_P";         /* TEC 功率 */
	default:          return "???";          /* 未知 */
	}
}

/* ================================================================
 * can_collect_init — 初始化
 * ================================================================ */

int can_collect_init(const struct device *can_dev)
{
	(void)can_dev;                            /* 保留参数 */

	memset(s_cbuf, 0, sizeof(s_cbuf));        /* 清零缓冲 */
	s_cbuf_count = 0;                         /* 重置计数 */
	memset(s_node_configured, 0, sizeof(s_node_configured)); /* 清配置标记 */

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

	LOG_INF("CAN collect started, boot_seq=%d, mode=config+listen", s_boot_seq);
	return 0;
}

/* ================================================================
 * can_collect_feed — 解析 CAN 帧 (心跳 / 主动上报 / RPTREGS)
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

	/* 只处理: RPTCURR(5) / RPTTEMP(7) 主动上报 + RPTREGS(9) 读响应 */
	if (func != FUNC_RPTCURR && func != FUNC_RPTTEMP && func != FUNC_RPTREGS) return;
	if (dlc < 6) return;                      /* 最短 6 字节 */

	uint8_t tp;                               /* TP 类型码 (主动上报) 或 tc (RPTREGS) */
	uint8_t opcode = 0;                       /* 操作码 */
	uint8_t slot;                             /* 槽位号 */
	float val;                                /* 测量值 */

	if ((func == FUNC_RPTCURR || func == FUNC_RPTTEMP) && dlc == 6) {
		/*
		 * 主动上报帧 (DLC=6):
		 *   data[0:3] = float 值 (IEEE 754)
		 *   data[4]   = slot
		 *   data[5]   = TP 类型码 (TP_IREAL / TP_TREAL / ...)
		 */
		memcpy(&val, &frame->data[0], sizeof(val)); /* float */
		slot = frame->data[4];                /* slot */
		tp = frame->data[5];                  /* TP 类型码 */

		/* TP 白名单: 只收 MCXN947 配置过的类型, 过滤主控遗留的旧配置上报 */
		if (func == FUNC_RPTCURR) {
			if (tp != TP_IREAL && tp != TP_ISET) return;  /* 仅收实测电流 + 目标值 */
		} else { /* FUNC_RPTTEMP */
			if (tp != TP_TREAL && tp != TP_T2REAL &&
			    tp != TP_T3REAL && tp != TP_TECDUTY) return; /* 仅收 T1/T2/T3/TEC */
		}
		opcode = 0;                           /* 主动上报无 opcode */
	} else {
		/*
		 * RPTREGS 响应 或 DLC=8 帧:
		 *   data[1] = tc (typecode, 寄存器地址)
		 *   data[2] = opcode
		 *   data[3] = slot
		 *   data[4:7] = float 或 uint32 值
		 *   注意: data[0] 不校验 (K64 可能填 0 或 FUNC_SETREGS)
		 */
		memcpy(&val, &frame->data[4], sizeof(val)); /* float */
		tp = frame->data[1];                  /* 复用 tp 字段存 tc */
		opcode = frame->data[2];              /* 操作码 */
		slot = frame->data[3];                /* slot */
	}

	/* 缓冲条目 */
	k_mutex_lock(&s_cbuf_lock, K_FOREVER);
	if (s_cbuf_count < COLLECT_BUF_SIZE) {
		struct collect_entry *e = &s_cbuf[s_cbuf_count];
		e->uptime = k_uptime_get();
		e->node_id = node_id;
		e->slot = slot;
		e->func = func;
		e->tp = tp;                           /* 主动上报:TP码, RPTREGS:tc */
		e->opcode = opcode;
		e->val_float = val;
		s_cbuf_count++;
	}
	k_mutex_unlock(&s_cbuf_lock);
}

/* ================================================================
 * can_collect_poll — 检查新节点并下发 Monitor 配置
 *
 * 改前: 每 500ms 交替发 TEMP/CURR 读查询 (OP_R_*)
 * 改后: 发现新节点 → 一次性配置 Monitor 周期 → 后续只监听
 * ================================================================ */

void can_collect_poll(const struct device *can_dev)
{
	/* 空总线: 无活跃节点时不发任何帧, 避免无人 ACK 产生错误中断 */
	if (s_active_count == 0) return;

	for (int n = 0; n < s_active_count; n++) {
		uint8_t node = s_active_nodes[n];
		if (!s_node_configured[node]) {
			configure_node(can_dev, node);    /* 下发 Monitor 周期配置 */
			s_node_configured[node] = true;
			LOG_INF("Collect: configured node %u monitors", node);
		}
	}
}

/* ================================================================
 * configure_node — 配置一个 K64 节点的全部 Monitor 周期
 *
 * 每 slot 配置 6 项:
 *   电流: tc=101(实测I), tc=107(目标I)
 *   温度: tc=101(T1), tc=102(T2), tc=103(T3), tc=104(TEC占空比)
 * 共 6 slot × 6 Monitor = 36 条 SETREGS
 * ================================================================ */

static void configure_node(const struct device *can_dev, uint8_t node_id)
{
	uint32_t period = COLLECT_REPORT_PERIOD_MS; /* 500ms */

	for (int s = 0; s < COLLECT_MAX_SLOTS; s++) {
		/* 电流 Monitor: OP_S_CURR=7 */
		send_setregs(can_dev, node_id, s,
			     TC_MON_C_IREAD_SAMP, OP_S_CURR, period); /* LD 实测电流 */
		send_setregs(can_dev, node_id, s,
			     TC_MON_C_ISET, OP_S_CURR, period);       /* 电流目标值 */

		/* 温度 Monitor: OP_S_TEMP=5 */
		send_setregs(can_dev, node_id, s,
			     TC_MON_T_T1_SAMP, OP_S_TEMP, period);    /* 第一温度实测值 */
		send_setregs(can_dev, node_id, s,
			     TC_MON_T_T2, OP_S_TEMP, period);         /* 第二温度实测值 */
		send_setregs(can_dev, node_id, s,
			     TC_MON_T_T3, OP_S_TEMP, period);         /* 第三温度实测值 */
		send_setregs(can_dev, node_id, s,
			     TC_MON_T_TEC_DUTY, OP_S_TEMP, period);   /* TEC 温控 PWM 强度 */
	}
}

/* ================================================================
 * send_setregs — 构造并发送一条 SETREGS 写指令
 *
 * CAN ID:  (FUNC_SETREGS << 7) | target_node
 * data[0]  = slot     (channel 号)
 * data[1]  = tc       (typecode, 寄存器地址)
 * data[2]  = opcode   (OP_S_CURR=7 / OP_S_TEMP=5 等)
 * data[3]  = 0        (保留)
 * data[4:7] = data_val (uint32 LE, Monitor 周期 ms)
 * ================================================================ */

static void send_setregs(const struct device *can_dev,
			 uint8_t target_node, uint8_t slot,
			 uint8_t tc, uint8_t opcode,
			 uint32_t data_val)
{
	struct can_frame frame;

	frame.id = (FUNC_SETREGS << 7) | (target_node & 0x7F); /* SETREGS→目标 */
	frame.dlc = 8;                            /* 8 字节 */
	frame.flags = 0;                          /* 标准帧 */

	frame.data[0] = slot;                     /* channel/slot */
	frame.data[1] = tc;                       /* typecode */
	frame.data[2] = opcode;                   /* OP_S_CURR / OP_S_TEMP */
	frame.data[3] = 0;                        /* 保留 */
	/* data[4:7] = data_val, little-endian */
	frame.data[4] = (uint8_t)(data_val & 0xFF);
	frame.data[5] = (uint8_t)((data_val >> 8) & 0xFF);
	frame.data[6] = (uint8_t)((data_val >> 16) & 0xFF);
	frame.data[7] = (uint8_t)((data_val >> 24) & 0xFF);

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
		const char *header = "uptime,node_id,slot,func,tp,tp_name,opcode,val_float,val_hex\n";
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

		/* 功能码名称 */
		const char *fname =
			(e->func == FUNC_RPTCURR) ? "RPTCURR" :
			(e->func == FUNC_RPTTEMP) ? "RPTTEMP" :
			(e->func == FUNC_RPTREGS) ? "RPTREGS" : "???";

		/* tp 列: 主动上报填 TP 名, RPTREGS 填 tc 号 */
		const char *tlabel;
		if (e->func == FUNC_RPTCURR || e->func == FUNC_RPTTEMP) {
			tlabel = tp_name(e->tp);          /* 主动上报: TP 名称 */
		} else {
			tlabel = NULL;                    /* RPTREGS: 只用数字 */
		}

		int len;
		if (tlabel) {
			len = snprintf(line, sizeof(line),
				"%u,%u,%u,%s,%u,%s,%u,%.4f,0x%08X\n",
				e->uptime, e->node_id, e->slot, fname,
				e->tp, tlabel, e->opcode,
				(double)e->val_float, raw);
		} else {
			len = snprintf(line, sizeof(line),
				"%u,%u,%u,%s,%u,,%u,%.4f,0x%08X\n",
				e->uptime, e->node_id, e->slot, fname,
				e->tp, e->opcode,
				(double)e->val_float, raw);
		}
		if (len > 0 && len < (int)sizeof(line)) {
			fs_write(&f, line, len);
		}
	}

	fs_close(&f);
	LOG_INF("Collect: flushed %d entries to %s", count, file_path);
}
