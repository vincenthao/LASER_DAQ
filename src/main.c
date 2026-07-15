/*
 * LASER_DAQ - MCXN947 Laser Sensor Data Acquisition
 *
 * 目标: 通过 CAN 总线接入激光传感器，捕获测量数据
 *       在 LCD 屏幕上实时显示采集状态
 *       通过网络（Ethernet/ESP32）将数据上传至服务器
 *       数据用于后续机器学习模型训练
 *
 * FRDM-MCXN947 (Cortex-M33, 150MHz)
 */

#include <zephyr/kernel.h>             /* k_msleep, k_msgq 等内核 API */
#include <zephyr/device.h>             /* device_is_ready, DEVICE_DT_GET */
#include <zephyr/devicetree.h>         /* DT_NODELABEL 等设备树宏 */
#include <zephyr/drivers/can.h>        /* CAN 驱动 API */
#include <zephyr/logging/log.h>        /* 日志模块 */

LOG_MODULE_REGISTER(laser_daq, LOG_LEVEL_INF); /* 注册日志模块 "laser_daq" */

/* ---- 设备树设备引用 ---- */

/* 通过设备树别名 can0 获取 CAN 设备指针 */
#define CAN_DEV DEVICE_DT_GET(DT_ALIAS(can0))

/* ---- 消息队列（CAN RX 缓冲区） ---- */

/* 消息队列容量: 最多缓存 16 帧 */
#define CAN_RX_QUEUE_SIZE 16

/* CAN 帧消息队列（线程间传递接收到的帧） */
K_MSGQ_DEFINE(can_rx_msgq, sizeof(struct can_frame), CAN_RX_QUEUE_SIZE, 4);

/* ---- 子系统初始化 ---- */

static int init_can_collector(void);    /* CAN 数据采集子系统 */
static int init_lcd_display(void);      /* LCD 显示子系统 */
static int init_network_upload(void);   /* 网络上传子系统 */

/* ---- 线程入口 ---- */

void can_rx_thread(void *, void *, void *);  /* CAN 接收处理线程 */

/* 线程栈定义 */
K_THREAD_DEFINE(can_rx_tid, 1024,       /* 线程 ID + 栈大小 1024 字节 */
		can_rx_thread, NULL, NULL, NULL, /* 入口函数 + 参数（无） */
		7, 0, 0);                       /* 优先级 7, 无延迟启动 */

/* ================================================================
 * main — 系统入口
 * ================================================================ */

int main(void)
{
	printk("\n========================================\n");  /* 启动横幅 */
	printk("  MCXN947 Laser Data Collector\n");
	printk("========================================\n\n");

	LOG_INF("System boot complete, initializing subsystems...");  /* 日志: 启动完成 */

	/* 1. CAN 数据采集子系统 — flexcan0 @ 1Mbps, 70% 采样点 */
	if (init_can_collector() < 0) {
		LOG_ERR("CAN collector init failed");   /* 初始化失败日志 */
	}

	/* 2. LCD 显示子系统 */
	if (init_lcd_display() < 0) {
		LOG_ERR("LCD display init failed");
	}

	/* 3. 网络上传子系统 */
	if (init_network_upload() < 0) {
		LOG_ERR("Network upload init failed");
	}

	LOG_INF("All subsystems initialized, entering main loop");  /* 就绪 */

	while (1) {
		/* TODO: 主循环 — 协调各子系统，处理数据流水线 */
		k_msleep(1000);  /* 休眠 1 秒，让出 CPU */
	}

	return 0;
}

/* ================================================================
 * CAN 数据采集子系统
 * ================================================================ */

static int init_can_collector(void)
{
	LOG_INF("CAN collector: initializing flexcan0...");  /* 开始初始化 */

	/* 步骤1: 检查设备是否就绪（设备树已配置 + 驱动已加载） */
	if (!device_is_ready(CAN_DEV)) {
		LOG_ERR("CAN device not ready");      /* 设备不可用 */
		return -1;
	}
	LOG_INF("CAN device found: %s", CAN_DEV->name);  /* 打印设备名 */

	/* 步骤2: 设置 CAN 控制器为正常模式（非环回/监听） */
	int ret = can_set_mode(CAN_DEV, CAN_MODE_NORMAL);
	if (ret != 0) {
		LOG_ERR("CAN set mode failed: %d", ret);  /* 模式设置失败 */
		return -1;
	}

	/* 步骤3: 注册 RX 滤波器 + 消息队列（接收标准帧） */
	const struct can_filter rx_filter = {
		.flags = 0,                      /* 标准帧 (IDE=0), 数据帧 */
		.id = 0,                          /* 接收所有 ID */
		.mask = 0,                        /* 不过滤任何位 */
	};
	ret = can_add_rx_filter_msgq(CAN_DEV, &can_rx_msgq, &rx_filter);
	if (ret < 0) {
		LOG_ERR("CAN add RX filter failed: %d", ret);  /* 滤波器失败 */
		return -1;
	}

	/* 步骤4: 启动 CAN 控制器 */
	ret = can_start(CAN_DEV);
	if (ret != 0) {
		LOG_ERR("CAN start failed: %d", ret);  /* 启动失败 */
		return -1;
	}

	LOG_INF("CAN collector: flexcan0 started, 1Mbps, waiting for frames");
	return 0;  /* 成功 */
}

/* ================================================================
 * CAN 接收线程 — 处理来自传感器的 CAN 帧
 * ================================================================ */

void can_rx_thread(void *arg1, void *arg2, void *arg3)
{
	(void)arg1; (void)arg2; (void)arg3;        /* 未使用参数 */

	struct can_frame frame;                      /* CAN 帧缓冲区 */

	while (1) {
		/* 阻塞等待 CAN 帧（永不超时） */
		int ret = k_msgq_get(&can_rx_msgq, &frame, K_FOREVER);
		if (ret != 0) {
			LOG_WRN("CAN RX queue read error: %d", ret);  /* 队列错误 */
			continue;
		}

		/* 打印接收到的帧信息 */
		if (frame.dlc > 0) {
			LOG_INF("CAN RX: ID=0x%03X DLC=%d [%02X %02X %02X %02X %02X %02X %02X %02X]",
				frame.id,          /* CAN ID */
				frame.dlc,         /* 数据长度 */
				frame.data[0], frame.data[1],   /* 数据字节 0-7 */
				frame.data[2], frame.data[3],
				frame.data[4], frame.data[5],
				frame.data[6], frame.data[7]);
		}

		/* TODO: 根据 CAN ID 解析激光传感器协议 */
	}
}

/* ================================================================
 * CAN 发送函数 — 发送一帧到总线
 * ================================================================ */

int can_send_frame(uint32_t id, const uint8_t *data, uint8_t dlc)
{
	struct can_frame frame;          /* 发送帧结构 */

	frame.id = id;                   /* 标准帧 ID (11-bit) */
	frame.dlc = dlc;                 /* 数据长度 (0-8) */
	frame.flags = 0;                 /* 标准帧, 无 RTR/FD/BRS 标志 */
	memcpy(frame.data, data, dlc);   /* 拷贝数据 */

	return can_send(CAN_DEV, &frame, K_MSEC(100), NULL, NULL);
}

/* ================================================================
 * LCD 显示子系统（占位）
 * ================================================================ */

static int init_lcd_display(void)
{
	LOG_INF("LCD display: initializing ST7796S via FlexIO...");
	/* TODO: 初始化 LCD，显示采集状态界面 */
	return 0;  /* 占位返回成功 */
}

/* ================================================================
 * 网络上传子系统（占位）
 * ================================================================ */

static int init_network_upload(void)
{
	LOG_INF("Network upload: initializing Ethernet/ESP32...");
	/* TODO: 初始化网络栈，连接上传服务器 */
	return 0;  /* 占位返回成功 */
}
