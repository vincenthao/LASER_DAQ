/*
 * can_proto.h — CAN 协议层头文件
 *
 * CAN ID (11-bit) = (func_code << 7) | (node_id & 0x7F)
 * 对齐 GD32 GSK5G_MCU 的 can_cmd.h 协议格式
 *
 * MCXN947 作为主控端, node_id = 33
 * GD32F407 作为被控端, node_id = 2 (K64 身份)
 */

#ifndef CAN_PROTO_H
#define CAN_PROTO_H

#include <stdint.h>              /* uint8_t, uint32_t 等类型 */
#include <zephyr/drivers/can.h>  /* struct can_frame, can_filter */

/* ---- 节点 ID ---- */

#define LASER_NODE_ID_DEFAULT 33 /* MCXN947 默认 CAN 节点 ID */

/* ---- CAN ID 宏 ---- */

#define CAN_ID(func) (((uint32_t)(func) << 7) | (g_can_node_id & 0x7F)) /* 生成 11-bit CAN 标识符 */

/* ---- 功能码枚举 (对齐 GD32 can_cmd.h) ---- */

enum can_func_code {
	FUNC_SETPARA     = 0,   /* 0x0: 设参数 */
	FUNC_RPTALAM     = 1,   /* 0x1: 上报告警 */
	FUNC_RESERVD     = 2,   /* 0x2: 保留 */
	FUNC_RPTSWIT     = 3,   /* 0x3: 上报开关 */
	FUNC_SETSWIT     = 4,   /* 0x4: 设开关 */
	FUNC_RPTCURR     = 5,   /* 0x5: 上报电流 */
	FUNC_SETCURR     = 6,   /* 0x6: 设电流 */
	FUNC_RPTTEMP     = 7,   /* 0x7: 上报温度 */
	FUNC_SETTEMP     = 8,   /* 0x8: 设温度 */
	FUNC_RPTREGS     = 9,   /* 0x9: 上报寄存器 */
	FUNC_SETREGS     = 10,  /* 0xA: 设寄存器 */
	FUNC_RPTFILE     = 11,  /* 0xB: 上报文件/参数 */
	FUNC_SETFILE     = 12,  /* 0xC: 设文件/参数 */
	FUNC_RPTPDPW     = 13,  /* 0xD: 上报 PD 功率 */
	FUNC_HEARTBT     = 14,  /* 0xE: 心跳 */
	FUNC_RPTALAMDATA = 15,  /* 0xF: 告警数据 */
};

/* ---- 全局变量 ---- */

extern uint8_t g_can_node_id;    /* 当前节点 ID, 默认 LASER_NODE_ID_DEFAULT */

/* ---- CAN RX 消息队列 (供 can_proto_init 和 RX 线程共用) ---- */

#define CAN_RX_QUEUE_SIZE 16     /* 消息队列容量: 最多缓存 16 帧 */
extern struct k_msgq can_rx_msgq; /* CAN 帧消息队列 (定义在 can_proto.c) */

/* ---- API ---- */

/** 初始化 CAN 协议层: 设置节点 ID, 配置滤波器 */
int can_proto_init(const struct device *can_dev);

/** 设置节点 ID (运行时修改, 重建滤波器) */
void can_proto_set_node_id(const struct device *can_dev, uint8_t node_id);

/** CAN 帧派发: 根据 func_code 路由到对应处理函数 */
void can_proto_dispatch(const struct device *can_dev, struct can_frame *frame);

/** 发送心跳帧 */
int can_proto_send_heartbeat(const struct device *can_dev);

/** 发送通用数据帧 */
int can_proto_send_frame(const struct device *can_dev, uint8_t func_code,
			 const uint8_t *data, uint8_t dlc);

/** 环回自检: 发送 3 帧测试数据, 验证 TX→RX 通路 */
int can_proto_loopback_test(const struct device *can_dev);

/** 查询 CAN 总线是否就绪 (error-active 状态, 可以正常收发) */
bool can_proto_is_bus_ok(const struct device *can_dev);

#endif /* CAN_PROTO_H */
