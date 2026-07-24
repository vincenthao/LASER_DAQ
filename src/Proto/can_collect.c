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
#define PERIOD_CFG_PATH   "/NAND:/collect/period.cfg" /* 周期配置文件 */

struct collect_entry {
	uint32_t sample_seq;                  /* 采样批次号 (同批相同, 合并键) */
	uint8_t node_id;                      /* K64 设备 ID */
	uint8_t slot;                         /* 槽位号 */
	uint8_t func;                         /* 功能码 (5=RPTCURR, 7=RPTTEMP) */
	uint8_t tp;                           /* TP 类型码 */
	uint8_t opcode;                       /* 操作码 (主动上报固定 0) */
	float val_float;                      /* 解析后的浮点值 */
	uint32_t uptime;                      /* 时间戳 (ms), 辅助列 */
};

static struct collect_entry s_cbuf[COLLECT_BUF_SIZE]; /* 采集缓冲 */
static int s_cbuf_count;                 /* 当前条目数 */
static K_MUTEX_DEFINE(s_cbuf_lock);      /* 缓冲互斥锁 */

static int s_boot_seq;                   /* 本次启动文件序号 */
static uint32_t s_sample_seq;             /* 当前采样批次号 (每次 flush 递增) */

/* ---- 全局变量 ---- */

uint32_t g_collect_period_ms = COLLECT_REPORT_PERIOD_DEFAULT; /* 当前上报周期 */
static uint32_t s_sample_seq;             /* 当前采样批次号 (每次 flush 递增) */

/* ---- 内部函数 ---- */

static void send_setregs(const struct device *can_dev,
			 uint8_t target_node, uint8_t slot,
			 uint8_t tc, uint8_t opcode,
			 uint32_t data_val);               /* 发送 SETREGS 写指令 */

static void configure_node(const struct device *can_dev,
			   uint8_t node_id);                /* 配置一个节点的全部 Monitor */

static const char *tp_name(uint8_t tp);                  /* TP 码 → 名称 */

/* flush 本地缓冲区 — 静态分配, 避免栈溢出 (128×20B≈2.5KB) */
static struct collect_entry s_flush_buf[COLLECT_BUF_SIZE];

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
	s_sample_seq = 0;                         /* 重置批次号 */
	memset(s_node_configured, 0, sizeof(s_node_configured)); /* 清配置标记 */

	/* 创建 collect 目录 */
	fs_mkdir("/NAND:/collect");               /* 忽略 EEXIST */

	/* 加载周期配置文件 (不存在则保持默认 500ms) */
	{
		struct fs_file_t pf;
		fs_file_t_init(&pf);
		if (fs_open(&pf, PERIOD_CFG_PATH, FS_O_READ) == 0) {
			char buf[16];
			ssize_t n = fs_read(&pf, buf, sizeof(buf) - 1);
			if (n > 0) {
				buf[n] = '\0';
				uint32_t p;
				if (sscanf(buf, "%u", &p) == 1 && p >= 50 && p <= 60000) {
					g_collect_period_ms = p;  /* 有效范围 50ms~60s */
				}
			}
			fs_close(&pf);
		}
	}

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

	LOG_INF("CAN collect started, boot_seq=%d, period=%ums, mode=config+listen",
		s_boot_seq, g_collect_period_ms);
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

	/* 只处理: RPTCURR(5) / RPTTEMP(7) 主动上报 (RPTREGS 由主控处理, MCXN947 不采集) */
	if (func != FUNC_RPTCURR && func != FUNC_RPTTEMP) return;
	if (dlc < 6) return;                      /* 最短 6 字节 */

	uint8_t tp;                               /* TP 类型码 */
	uint8_t slot;                             /* 槽位号 */
	float val;                                /* 测量值 */

	/*
	 * 主动上报帧 (DLC=6):
	 *   data[0:3] = float 值 (IEEE 754)
	 *   data[4]   = slot
	 *   data[5]   = TP 类型码
	 */
	memcpy(&val, &frame->data[0], sizeof(val)); /* float */
	slot = frame->data[4];                    /* slot */
	tp = frame->data[5];                      /* TP 类型码 */

	/* TP 白名单: 只收 MCXN947 配置过的类型, 过滤主控遗留的旧配置上报 */
	if (func == FUNC_RPTCURR) {
		if (tp != TP_IREAL && tp != TP_ISET) return;  /* 仅收实测电流 + 目标值 */
	} else { /* FUNC_RPTTEMP */
		if (tp != TP_TREAL && tp != TP_T2REAL &&
		    tp != TP_T3REAL && tp != TP_TECDUTY) return; /* 仅收 T1/T2/T3/TEC */
	}

	/* 缓冲条目 */
	k_mutex_lock(&s_cbuf_lock, K_FOREVER);
	if (s_cbuf_count < COLLECT_BUF_SIZE) {
		struct collect_entry *e = &s_cbuf[s_cbuf_count];
		e->sample_seq = s_sample_seq;         /* 当前批次号 (flush 后递增) */
		e->node_id = node_id;
		e->slot = slot;
		e->func = func;
		e->tp = tp;                           /* TP 类型码 */
		e->opcode = 0;                        /* 主动上报无 opcode */
		e->val_float = val;
		e->uptime = k_uptime_get();           /* 时间戳辅助列 */
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
	uint32_t period = g_collect_period_ms;    /* 使用当前配置的周期 */

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

	/* 总线未就绪时不发, 避免空总线 TX 错误累积导致 bus-off */
	if (!can_proto_is_bus_ok(can_dev)) return;
	can_send(can_dev, &frame, K_MSEC(50), NULL, NULL); /* 发送 (短超时) */
}

/* ================================================================
 * can_collect_flush — 窄表 CSV 刷盘
 * ================================================================ */

void can_collect_flush(void)
{
	if (s_cbuf_count == 0) return;            /* 无数据 */

	/* 拷贝缓冲到静态缓冲区 (避免 2.5KB 栈分配导致溢出) */
	int count;

	k_mutex_lock(&s_cbuf_lock, K_FOREVER);
	count = s_cbuf_count;
	memcpy(s_flush_buf, s_cbuf, count * sizeof(struct collect_entry));
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
		/* 写 CSV 头: sample_seq 为主键, uptime 为辅助列 */
		const char *header = "sample_seq,node_id,slot,func,tp,tp_name,opcode,val_float,val_hex,uptime\n";
		fs_write(&f, header, strlen(header));
	}
	if (ret < 0) {
		LOG_ERR("Collect open %s failed: %d", file_path, ret);
		return;
	}

	/* 逐行写入窄表 */
	char line[128];                           /* 行缓冲 */
	for (int i = 0; i < count; i++) {
		struct collect_entry *e = &s_flush_buf[i];
		uint32_t raw;
		memcpy(&raw, &e->val_float, sizeof(raw)); /* float→hex */

		/* 功能码名称 */
		const char *fname =
			(e->func == FUNC_RPTCURR) ? "RPTCURR" :
			(e->func == FUNC_RPTTEMP) ? "RPTTEMP" : "???";

		/* tp 列: 主动上报填 TP 名称 */
		const char *tlabel = tp_name(e->tp);

		int len = snprintf(line, sizeof(line),
			"%u,%u,%u,%s,%u,%s,%u,%.4f,0x%08X,%u\n",
			e->sample_seq, e->node_id, e->slot, fname,
			e->tp, tlabel, e->opcode,
			(double)e->val_float, raw, e->uptime);
		if (len > 0 && len < (int)sizeof(line)) {
			fs_write(&f, line, len);
		}
	}

	fs_close(&f);
	s_sample_seq++;                           /* 批次号递增, 下一轮数据用新 seq */
	LOG_INF("Collect: flushed %d entries to %s (seq=%u)", count, file_path, s_sample_seq - 1);
}

/* ================================================================
 * can_collect_set_period — 自定义上报周期 (写配置文件, 重启生效)
 * ================================================================ */

void can_collect_set_period(uint32_t period_ms)
{
	if (period_ms < 50 || period_ms > 60000) {
		printk("Period out of range (50-60000 ms)\n");
		return;
	}

	struct fs_file_t f;
	fs_file_t_init(&f);
	int ret = fs_open(&f, PERIOD_CFG_PATH, FS_O_CREATE | FS_O_WRITE);
	if (ret < 0) {
		printk("Failed to write period config: %d\n", ret);
		return;
	}

	char buf[8];
	int len = snprintf(buf, sizeof(buf), "%u", period_ms);
	fs_write(&f, buf, len);
	fs_close(&f);

	printk("Period set to %u ms (reboot required)\n", period_ms);
}

/* ================================================================
 * can_collect_get_period — 查询当前上报周期
 * ================================================================ */

uint32_t can_collect_get_period(void)
{
	return g_collect_period_ms;
}
