/*
 * can_proto.c — CAN 协议层实现
 *
 * MCXN947 作为主控端 (node_id=33), GD32 作为被控端 (node_id=2)
 * 协议格式: CAN ID = (func_code << 7) | (node_id & 0x7F)
 */

#include "can_proto.h"               /* 协议头文件 */

#include <stdbool.h>                 /* bool 类型 */
#include <zephyr/kernel.h>           /* k_sleep, k_uptime_get 等 */
#include <zephyr/logging/log.h>      /* LOG_INF, LOG_ERR 等 */

LOG_MODULE_REGISTER(can_proto, LOG_LEVEL_INF); /* 注册日志模块 "can_proto" */

/* ---- 全局变量 ---- */

uint8_t g_can_node_id = LASER_NODE_ID_DEFAULT; /* 默认节点 ID = 33 */

/* CAN RX 消息队列 — 线程间传递接收到的 CAN 帧 */
K_MSGQ_DEFINE(can_rx_msgq, sizeof(struct can_frame), CAN_RX_QUEUE_SIZE, 4);

/* ---- 内部函数声明 ---- */

static void handle_setpara(const struct device *dev, struct can_frame *frame);  /* 处理 FUNC_SETPARA */
static void handle_setswit(const struct device *dev, struct can_frame *frame);  /* 处理 FUNC_SETSWIT */
static void handle_setcurr(const struct device *dev, struct can_frame *frame);  /* 处理 FUNC_SETCURR */
static void handle_settemp(const struct device *dev, struct can_frame *frame);  /* 处理 FUNC_SETTEMP */
static void handle_setregs(const struct device *dev, struct can_frame *frame);  /* 处理 FUNC_SETREGS */
static void handle_setfile(const struct device *dev, struct can_frame *frame);  /* 处理 FUNC_SETFILE */
static void handle_heartbeat(const struct device *dev, struct can_frame *frame); /* 处理 FUNC_HEARTBT */

/* ---- RX 滤波器 ID — 当前 filter_id ---- */

static int s_rx_filter_id = -1;      /* RX 滤波器 ID, -1 表示未注册 */
static enum can_state s_can_state = CAN_STATE_STOPPED; /* 当前 CAN 控制器状态 */
static bool s_state_logged = false;   /* 首次错误已打印, 避免刷屏 */

/* ---- CAN 状态变化回调 ---- */

static void can_state_change_cb(const struct device *dev, enum can_state state,
				struct can_bus_err_cnt err_cnt, void *user_data)
{
	(void)user_data;                              /* 未使用 */
	s_can_state = state;                          /* 更新全局状态 */

	switch (state) {
	case CAN_STATE_ERROR_ACTIVE:
		if (!s_state_logged) {
			LOG_INF("CAN bus OK (error-active)"); /* 总线恢复正常 */
			s_state_logged = false;               /* 复位日志标记 */
		}
		break;
	case CAN_STATE_ERROR_WARNING:
		s_state_logged = true;                    /* 标记已打印 */
		LOG_WRN("CAN bus warning: TX_ERR=%d RX_ERR=%d",
			err_cnt.tx_err_cnt, err_cnt.rx_err_cnt); /* 错误计数 */
		break;
	case CAN_STATE_ERROR_PASSIVE:
		LOG_WRN("CAN bus error-passive: TX_ERR=%d RX_ERR=%d",
			err_cnt.tx_err_cnt, err_cnt.rx_err_cnt); /* 被动错误 */
		break;
	case CAN_STATE_BUS_OFF:
		LOG_ERR("CAN bus-off! TX_ERR=%d", err_cnt.tx_err_cnt); /* 总线关闭 */
		break;
	default:
		break;
	}
}

/* ---- 总线状态查询 ---- */

bool can_proto_is_bus_ok(const struct device *can_dev)
{
	(void)can_dev;                                /* 保留参数, 未来扩展 */
	/* 只有 error-active 状态才允许发送 */
	return (s_can_state == CAN_STATE_ERROR_ACTIVE);
}

/* ================================================================
 * can_proto_init — 初始化协议层
 * ================================================================ */

int can_proto_init(const struct device *can_dev)
{
	if (!device_is_ready(can_dev)) {
		LOG_ERR("CAN device not ready");              /* 设备不可用 */
		return -1;
	}

	LOG_INF("CAN proto init: node_id=%d", g_can_node_id);  /* 打印节点 ID */

	/* 注册状态变化回调: 监控 bus-off / error-passive 等事件 */
	can_set_state_change_callback(can_dev, can_state_change_cb, NULL); /* 状态回调 */

	/* 注册 RX 滤波器: 全量接收 (嗅探模式) */
	const struct can_filter rx_filter = {
		.flags = 0,                               /* 标准帧 */
		.id = 0,                                  /* 接收所有 ID */
		.mask = 0,                                /* 不过滤任何位 */
	};
	s_rx_filter_id = can_add_rx_filter_msgq(can_dev,
						&can_rx_msgq, &rx_filter); /* 注册滤波器 */
	if (s_rx_filter_id < 0) {
		LOG_ERR("CAN proto add RX filter failed: %d", s_rx_filter_id); /* 失败 */
		return -1;
	}

	return 0;                                     /* 成功 */
}

/* ================================================================
 * can_proto_set_node_id — 运行时修改节点 ID
 * ================================================================ */

void can_proto_set_node_id(const struct device *can_dev, uint8_t node_id)
{
	if (node_id > 127) {
		LOG_ERR("Invalid node_id: %d (max 127)", node_id); /* 无效 ID */
		return;
	}

	g_can_node_id = node_id;                      /* 更新全局节点 ID */
	LOG_INF("CAN node_id changed to: %d", node_id); /* 日志 */

	/* 删除旧滤波器, 重建新滤波器 */
	if (s_rx_filter_id >= 0) {
		can_remove_rx_filter(can_dev, s_rx_filter_id); /* 移除旧滤波器 */
	}

	const struct can_filter rx_filter = {
		.flags = 0,                               /* 标准帧 */
		.id = g_can_node_id & 0x7F,               /* 新 node_id */
		.mask = 0x7F,                             /* 只检查低 7 位 */
	};
	s_rx_filter_id = can_add_rx_filter_msgq(can_dev,
						&can_rx_msgq, &rx_filter); /* 注册新滤波器 */
}

/* ================================================================
 * can_proto_dispatch — CAN 帧派发
 * ================================================================ */

void can_proto_dispatch(const struct device *can_dev, struct can_frame *frame)
{
	/* 提取 func_code: 高 4 位 */
	uint8_t func_code = (frame->id >> 7) & 0x0F;   /* 功能码 = CAN ID[10:7] */

	/* 校验: 低 7 位必须是本节点 ID */
	uint8_t rx_node_id = frame->id & 0x7F;         /* 接收到的节点 ID */
	if (rx_node_id != g_can_node_id) {
		return;                                   /* 不是发给本节点的, 忽略 */
	}

	/* 根据 func_code 路由 */
	switch (func_code) {
	case FUNC_SETPARA:
		handle_setpara(can_dev, frame);           /* 设参数 */
		break;
	case FUNC_SETSWIT:
		handle_setswit(can_dev, frame);           /* 设开关 */
		break;
	case FUNC_SETCURR:
		handle_setcurr(can_dev, frame);           /* 设电流 */
		break;
	case FUNC_SETTEMP:
		handle_settemp(can_dev, frame);           /* 设温度 */
		break;
	case FUNC_SETREGS:
		handle_setregs(can_dev, frame);           /* 设寄存器 */
		break;
	case FUNC_SETFILE:
		handle_setfile(can_dev, frame);           /* 设文件 */
		break;
	case FUNC_HEARTBT:
		handle_heartbeat(can_dev, frame);         /* 心跳 */
		break;
	default:
		LOG_DBG("Unhandled func_code: 0x%X", func_code); /* 未处理的功能码 */
		break;
	}
}

/* ================================================================
 * can_proto_send_heartbeat — 发送心跳帧
 * ================================================================ */

int can_proto_send_heartbeat(const struct device *can_dev)
{
	/* 总线未就绪 (无其他节点 ACK) 时不发心跳, 避免 TX 错误累积 */
	if (!can_proto_is_bus_ok(can_dev)) {
		return -EAGAIN;                           /* 总线未就绪 */
	}

	struct can_frame frame;                       /* 心跳帧 */
	uint8_t data = 0x05;                          /* CANopen NMT state: Operational */

	frame.id = CAN_ID(FUNC_HEARTBT);              /* ID = (14 << 7) | node_id */
	frame.dlc = 1;                                /* 1 字节数据 */
	frame.flags = 0;                              /* 标准帧 */
	frame.data[0] = data;                         /* 状态字节 */

	return can_send(can_dev, &frame, K_MSEC(100), NULL, NULL); /* 发送 */
}

/* ================================================================
 * can_proto_send_frame — 发送通用数据帧
 * ================================================================ */

int can_proto_send_frame(const struct device *can_dev, uint8_t func_code,
			 const uint8_t *data, uint8_t dlc)
{
	struct can_frame frame;                       /* 发送帧 */

	frame.id = CAN_ID(func_code);                 /* 拼接 CAN ID */
	frame.dlc = (dlc > 8) ? 8 : dlc;              /* 裁剪 DLC 到 8 */
	frame.flags = 0;                              /* 标准帧 */
	if (data != NULL) {
		memcpy(frame.data, data, frame.dlc);      /* 拷贝数据 */
	} else {
		memset(frame.data, 0, frame.dlc);         /* 零填充 */
	}

	return can_send(can_dev, &frame, K_MSEC(100), NULL, NULL); /* 发送 */
}

/* ================================================================
 * can_proto_loopback_test — 环回自检
 * ================================================================ */

int can_proto_loopback_test(const struct device *can_dev)
{
	struct can_frame frame;                       /* 发送帧缓冲区 */
	int ret;                                      /* 返回值 */

	printk("\n=== CAN Loopback Test Start (node_id=%d) ===\n", g_can_node_id); /* 测试横幅 */

	/* 测试帧1: SETPARA — 8 字节递增数据 */
	frame.id = CAN_ID(FUNC_SETPARA);              /* ID = SETPARA | node_id */
	frame.dlc = 8;                                /* 8 字节数据 */
	frame.flags = 0;                              /* 标准帧 */
	for (int i = 0; i < 8; i++) {
		frame.data[i] = 0xA0 + i;                 /* 数据: A0 A1 A2 A3 A4 A5 A6 A7 */
	}
	ret = can_send(can_dev, &frame, K_MSEC(100), NULL, NULL); /* 发送 */
	printk("  TX frame1: ID=0x%03X DLC=%d [%02X %02X %02X %02X %02X %02X %02X %02X] ret=%d\n",
	       frame.id, frame.dlc,                    /* 打印发送的帧 */
	       frame.data[0], frame.data[1], frame.data[2], frame.data[3],
	       frame.data[4], frame.data[5], frame.data[6], frame.data[7], ret);

	/* 测试帧2: SETCURR — 4 字节 */
	frame.id = CAN_ID(FUNC_SETCURR);              /* ID = SETCURR | node_id */
	frame.dlc = 4;                                /* 4 字节数据 */
	frame.data[0] = 0x00; frame.data[1] = 0x64;  /* 电流 = 100 (0x0064) */
	frame.data[2] = 0x00; frame.data[3] = 0x32;  /* 电流 = 50  (0x0032) */
	ret = can_send(can_dev, &frame, K_MSEC(100), NULL, NULL); /* 发送 */
	printk("  TX frame2: ID=0x%03X DLC=%d [%02X %02X %02X %02X] ret=%d\n",
	       frame.id, frame.dlc,
	       frame.data[0], frame.data[1], frame.data[2], frame.data[3], ret);

	/* 测试帧3: HEARTBT — 1 字节心跳 */
	frame.id = CAN_ID(FUNC_HEARTBT);              /* ID = HEARTBT | node_id */
	frame.dlc = 1;                                /* 1 字节 */
	frame.data[0] = 0x05;                         /* NMT state: Operational */
	ret = can_send(can_dev, &frame, K_MSEC(100), NULL, NULL); /* 发送 */
	printk("  TX frame3: ID=0x%03X DLC=%d [%02X] ret=%d\n",
	       frame.id, frame.dlc, frame.data[0], ret);

	/* 等待环回帧被 RX 线程接收并处理 */
	k_msleep(200);                                /* 给 RX 线程 200ms 处理 */
	printk("=== CAN Loopback Test End ===\n\n");  /* 测试结束 */

	return 0;                                     /* 成功 */
}

/* ================================================================
 * 内部处理函数 (占位 — 后续根据实际传感器逻辑填充)
 * ================================================================ */

static void handle_setpara(const struct device *dev, struct can_frame *frame)
{
	LOG_INF("SETPARA: DLC=%d [%02X %02X %02X %02X %02X %02X %02X %02X]",
		frame->dlc,                               /* 数据长度 */
		frame->data[0], frame->data[1],           /* 数据字节 */
		frame->data[2], frame->data[3],
		frame->data[4], frame->data[5],
		frame->data[6], frame->data[7]);
	/* TODO: 解析激光传感器参数并应用 */
}

static void handle_setswit(const struct device *dev, struct can_frame *frame)
{
	LOG_INF("SETSWIT: DLC=%d data=%02X", frame->dlc, frame->data[0]); /* 开关控制 */
	/* TODO: 控制激光开关 */
}

static void handle_setcurr(const struct device *dev, struct can_frame *frame)
{
	LOG_INF("SETCURR: DLC=%d [%02X %02X]", frame->dlc,
		frame->data[0], frame->data[1]);          /* 电流设置 */
	/* TODO: 设置激光电流 */
}

static void handle_settemp(const struct device *dev, struct can_frame *frame)
{
	LOG_INF("SETTEMP: DLC=%d [%02X %02X]", frame->dlc,
		frame->data[0], frame->data[1]);          /* 温度设置 */
	/* TODO: 设置激光温度 */
}

static void handle_setregs(const struct device *dev, struct can_frame *frame)
{
	LOG_INF("SETREGS: DLC=%d addr=%02X val=%02X", frame->dlc,
		frame->data[0], frame->data[1]);          /* 寄存器操作 */
	/* TODO: 读写传感器寄存器 */
}

static void handle_setfile(const struct device *dev, struct can_frame *frame)
{
	LOG_INF("SETFILE: DLC=%d", frame->dlc);       /* 文件传输 */
	/* TODO: 处理固件/参数文件传输 */
}

static void handle_heartbeat(const struct device *dev, struct can_frame *frame)
{
	uint8_t remote_node_id = frame->id & 0x7F;    /* 发送端节点 ID */
	LOG_DBG("Heartbeat from node %d, state=%02X",
		remote_node_id, frame->data[0]);          /* 心跳源 + 状态 */
	/* TODO: 更新 GD32 在线状态表 */
}
