/*
 * LASER_DAQ - MCXN947 Laser Sensor Data Acquisition
 *
 * 目标: 通过 CAN 总线接入激光传感器，捕获测量数据
 *       在 LCD 屏幕上实时显示采集状态
 *       通过网络（Ethernet/ESP32）将数据上传至服务器
 *       数据用于后续机器学习模型训练
 *
 * FRDM-MCXN947 (Cortex-M33, 150MHz)
 *
 * CAN 协议: 对齐 GD32 GSK5G_MCU 的 can_cmd.h
 *   - MCXN947 node_id = 33 (主控端)
 *   - GD32F407  node_id = 2  (被控端)
 *   - CAN ID = (func_code << 7) | (node_id & 0x7F)
 */

#include <zephyr/kernel.h>             /* k_msleep, k_msgq 等内核 API */
#include <zephyr/device.h>             /* device_is_ready, DEVICE_DT_GET */
#include <zephyr/devicetree.h>         /* DT_NODELABEL 等设备树宏 */
#include <zephyr/drivers/can.h>        /* CAN 驱动 API */
#include <zephyr/logging/log.h>        /* 日志模块 */
#include <zephyr/fs/fs.h>			   /* 文件系统 API */

#include <zephyr/storage/flash_map.h>  /* flash_map API, 分区管理 */
#include <ff.h>                        /* FatFS API (f_mount/f_open/f_write 等) */
#include <zephyr/usb/usbd.h>           /* 新一代 USB 设备栈 */
#include <zephyr/usb/class/usbd_msc.h> /* USB Mass Storage Class */
#include <zephyr/usb/bos.h>            /* USB BOS 描述符 */

#include "Proto/can_proto.h"            /* CAN 协议层 */
#include "Proto/can_sniffer.h"           /* CAN 嗅探模块 */
#include "Proto/can_collect.h"           /* CAN 数据采集模块 */

LOG_MODULE_REGISTER(laser_daq, LOG_LEVEL_INF); /* 注册日志模块 "laser_daq" */

/* ---- 设备树设备引用 ---- */

/* 通过设备树别名 can0 获取 CAN 设备指针 */
#define CAN_DEV DEVICE_DT_GET(DT_ALIAS(can0))

/* ---- USB Mass Storage 逻辑单元注册 ---- */

USBD_DEFINE_MSC_LUN(nand, "NAND", "Zephyr", "LASER DAQ", "1.00"); /* 注册 MSC LUN */

/* ---- USB 设备上下文 ---- */

USBD_DEVICE_DEFINE(laser_usbd,                        /* 设备上下文名 */
		   DEVICE_DT_GET(DT_NODELABEL(zephyr_udc0)), /* UDC 设备 */
		   0x2FE3, 0x0008);                     /* VID=Zephyr, PID=0008 */

/* USB 字符串描述符 */
USBD_DESC_LANG_DEFINE(laser_lang);                    /* 语言描述符 */
USBD_DESC_MANUFACTURER_DEFINE(laser_mfr, "LASER DAQ");  /* 制造商 */
USBD_DESC_PRODUCT_DEFINE(laser_product, "LASER DAQ");    /* 产品名 */

/* USB 配置描述符 */
USBD_DESC_CONFIG_DEFINE(laser_fs_cfg, "FS MSC");      /* 全速配置描述符 */
USBD_DESC_CONFIG_DEFINE(laser_hs_cfg, "HS MSC");      /* 高速配置描述符 */

/* USB 配置 (全速 + 高速) */
USBD_CONFIGURATION_DEFINE(laser_fs_config, 0, 100, &laser_fs_cfg);  /* 全速 */
USBD_CONFIGURATION_DEFINE(laser_hs_config, 0, 100, &laser_hs_cfg);  /* 高速 */

/* ---- 全局状态 ---- */

static FATFS fat_fs;                     /* FatFS 文件系统对象 (持久化) */

/* ---- 子系统初始化 ---- */

static int init_can_collector(void);    /* CAN 数据采集子系统 */
static int init_storage(void);          /* FatFS 存储子系统 */
static int init_usb_msc(void);          /* USB MSC 初始化 */
static int init_lcd_display(void);      /* LCD 显示子系统 */
static int init_network_upload(void);   /* 网络上传子系统 */

/* ---- 线程入口 ---- */

void can_rx_thread(void *, void *, void *);  /* CAN 接收处理线程 */

/* 线程栈定义 */
K_THREAD_DEFINE(can_rx_tid, 2048,       /* 线程 ID + 栈大小 2048 字节 */
		can_rx_thread, NULL, NULL, NULL, /* 入口函数 + 参数（无） */
		7, 0, 0);                       /* 优先级 7, 无延迟启动 */

/* ================================================================
 * USB Mass Storage 初始化
 * ================================================================ */

static int init_usb_msc(void)
{
	int err;                                      /* 返回值 */

	/* 注册 USB 字符串描述符 */
	err = usbd_add_descriptor(&laser_usbd, &laser_lang); /* 语言 */
	if (err) { return err; }
	err = usbd_add_descriptor(&laser_usbd, &laser_mfr);  /* 制造商 */
	if (err) { return err; }
	err = usbd_add_descriptor(&laser_usbd, &laser_product); /* 产品名 */
	if (err) { return err; }

	/* 注册全速配置 + 高速配置 + 自动注册 MSC 类 */
	err = usbd_add_configuration(&laser_usbd, USBD_SPEED_FS, &laser_fs_config);
	if (err) { return err; }
	err = usbd_register_all_classes(&laser_usbd, USBD_SPEED_FS, 1, NULL);
	if (err) { return err; }

	if (usbd_caps_speed(&laser_usbd) == USBD_SPEED_HS) {
		err = usbd_add_configuration(&laser_usbd, USBD_SPEED_HS,
					     &laser_hs_config); /* 高速配置 */
		if (err) { return err; }
		err = usbd_register_all_classes(&laser_usbd, USBD_SPEED_HS,
						1, NULL);   /* 注册 MSC 类 */
		if (err) { return err; }
	}

	/* 初始化并启用 USB 设备 */
	err = usbd_init(&laser_usbd);                 /* 初始化 USBD 栈 */
	if (err) { return err; }
	err = usbd_enable(&laser_usbd);               /* 启用 USB, 开始枚举 */
	if (err) { return err; }

	return 0;                                     /* 成功 */
}

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

	/* 2. FatFS 存储子系统 — W25Q64JV NOR Flash 8 MB */
	if (init_storage() < 0) {
		LOG_ERR("Storage init failed");         /* 存储初始化失败 */
	}

	/* 3. USB Mass Storage — HS USB, 暴露 /NAND: 为 U 盘 */
	if (init_usb_msc() < 0) {
		LOG_ERR("USB MSC init failed");         /* USB 初始化失败 */
	}

	/* 4. LCD 显示子系统 */
	if (init_lcd_display() < 0) {
		LOG_ERR("LCD display init failed");
	}

	/* 5. CAN 嗅探 — 全量捕获总线帧, 按 node_id 存入 CSV */
	if (can_sniffer_init() < 0) {
		LOG_ERR("CAN sniffer init failed");       /* 嗅探初始化失败 */
	}

	/* 6. CAN 采集 — 主动轮询各 K64 测量值, 窄表 CSV */
	if (can_collect_init(CAN_DEV) < 0) {
		LOG_ERR("CAN collect init failed");       /* 采集初始化失败 */
	}

	/* 7. 网络上传子系统 */
	if (init_network_upload() < 0) {
		LOG_ERR("Network upload init failed");
	}

	LOG_INF("All subsystems initialized, entering main loop");  /* 就绪 */

	while (1) {
		/* 主循环: 轮询各 K64 采集数据 + 批量刷盘 */
		can_collect_poll(CAN_DEV);                /* 发送查询指令 */
		can_sniffer_flush();                      /* 嗅探数据写 CSV */
		can_collect_flush();                      /* 采集数据写 CSV */
		k_msleep(COLLECT_POLL_PERIOD_MS);         /* 轮询间隔 */
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

	/* 步骤2: 设置为环回模式, 进行自检 */
	int ret = can_set_mode(CAN_DEV, CAN_MODE_LOOPBACK); /* 环回模式 */
	if (ret != 0) {
		LOG_ERR("CAN set loopback mode failed: %d", ret); /* 模式设置失败 */
		return -1;
	}

	/* 步骤3: 初始化协议层 (配置 RX 滤波器, 设置 node_id=33) */
	ret = can_proto_init(CAN_DEV);
	if (ret < 0) {
		LOG_ERR("CAN proto init failed: %d", ret); /* 协议初始化失败 */
		return -1;
	}

	/* 步骤4: 启动 CAN 控制器 */
	ret = can_start(CAN_DEV);
	if (ret != 0) {
		LOG_ERR("CAN start failed: %d", ret);  /* 启动失败 */
		return -1;
	}

	/* 步骤5: 环回自检 — TX→RX 内部回环, 验证通信通路 */
	can_proto_loopback_test(CAN_DEV);                /* 发送 3 帧测试数据 */

	/* 步骤6: 切换到正常模式 */
	can_stop(CAN_DEV);                               /* 先停止 */
	ret = can_set_mode(CAN_DEV, CAN_MODE_NORMAL);    /* 正常模式 */
	if (ret != 0) {
		LOG_ERR("CAN set normal mode failed: %d", ret); /* 切换失败 */
		return -1;
	}
	ret = can_start(CAN_DEV);                        /* 重新启动 */
	if (ret != 0) {
		LOG_ERR("CAN restart failed: %d", ret);    /* 重启失败 */
		return -1;
	}

	LOG_INF("CAN collector: flexcan0 started, 1Mbps, node_id=%d, waiting for frames",
		g_can_node_id);                           /* 打印节点 ID */
	return 0;  /* 成功 */
}

/* ================================================================
 * CAN 接收线程 — 协议派发
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

		/* 嗅探 + 采集: 原始帧和结构化数据并存 */
		can_sniffer_feed(&frame);                 /* 原始嗅探 */
		can_collect_feed(&frame);                 /* 采集解析 */
	}
}

/* ================================================================
 * FatFS + USB MSC 存储子系统 — W25Q64JV NOR Flash 8 MB
 * ================================================================ */

/* 递归遍历目录, 打印文件/子目录 */
static void print_dir(const char *path, int depth)
{
	struct fs_dir_t dir;
	fs_dir_t_init(&dir);                          /* 初始化目录对象 */
	int ret = fs_opendir(&dir, path);             /* 打开目录 */
	if (ret < 0) {
		LOG_WRN("Cannot open %s: %d", path, ret); /* 打开失败 */
		return;
	}

	while (1) {
		struct fs_dirent entry = { 0 };           /* 目录项 */
		ret = fs_readdir(&dir, &entry);           /* 读取一条 */
		if (ret < 0 || entry.name[0] == '\0') {
			break;                                /* 读完或出错 */
		}

		/* 跳过 . 和 .. */
		if (entry.name[0] == '.' && (entry.name[1] == '\0' ||
		    (entry.name[1] == '.' && entry.name[2] == '\0'))) {
			continue;
		}

		/* 缩进打印 */
		for (int i = 0; i < depth; i++) {
			printk("  ");                         /* 每层缩进2空格 */
		}
		printk("%c %8zu %s\n",
		       (entry.type == FS_DIR_ENTRY_DIR) ? 'D' : 'F', /* 类型 */
		       entry.size,                         /* 大小 */
		       entry.name);                        /* 名称 */

		/* 递归进入子目录 */
		if (entry.type == FS_DIR_ENTRY_DIR) {
			char sub_path[280];                   /* 子目录路径 */
			snprintf(sub_path, sizeof(sub_path), "%s/%s", path, entry.name);
			print_dir(sub_path, depth + 1);       /* 递归遍历 */
		}
	}
	fs_closedir(&dir);                            /* 关闭目录 */
}

static int init_storage(void)
{
	LOG_INF("Storage: mounting FatFS on W25Q64JV..."); /* 开始挂载 */

	int ret;                                      /* 返回值 */

	/* 配置挂载参数 */
	struct fs_mount_t mp = {
		.type = FS_FATFS,                         /* 文件系统类型: FatFS */
		.fs_data = &fat_fs,                       /* FatFS 对象 */
		.storage_dev = (void *)PARTITION_ID(storage_partition), /* Flash 分区 */
		.mnt_point = "/NAND:",                    /* 挂载点 */
	};

	ret = fs_mount(&mp);                          /* 执行挂载 */
	if (ret < 0) {
		LOG_ERR("FatFS mount failed: %d", ret);   /* 挂载失败 */
		return -1;
	}
	LOG_INF("FatFS mounted at /NAND:");           /* 挂载成功 */

	/* 打印文件系统信息 */
	struct fs_statvfs stat;
	ret = fs_statvfs("/NAND:", &stat);            /* 查询文件系统状态 */
	if (ret == 0) {
		LOG_INF("FS: block=%luB total=%lu free=%lu",
			stat.f_bsize,                         /* 块大小 */
			stat.f_blocks,                        /* 总块数 */
			stat.f_bfree);                        /* 空闲块数 */
	}

	/* 递归遍历并打印文件系统内容 */
	LOG_INF("--- /NAND: contents ---");           /* 列表头 */
	print_dir("/NAND:/", 0);                      /* 从根目录开始 (尾随斜杠) */
	LOG_INF("--- end of list ---");

	LOG_INF("Storage init done, USB MSC ready");   /* 初始化完成 */
	return 0;
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
