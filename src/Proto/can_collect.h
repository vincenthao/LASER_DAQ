/*
 * can_collect.h — CAN 测量数据主动采集模块
 *
 * MCXN947 模拟主控, 向各 K64 设备发送读指令, 解析回复帧中的测量值,
 * 以窄表格式 (node_id,slot,func,tc,val_float) 存入 CSV
 */

#ifndef CAN_COLLECT_H
#define CAN_COLLECT_H

#include <stdint.h>                     /* uint8_t, uint32_t */
#include <zephyr/drivers/can.h>         /* struct can_frame */

/* 轮询间隔 (ms) */
#define COLLECT_POLL_PERIOD_MS 500

/* 最大可发现节点数 */
#define COLLECT_MAX_NODES 16            /* 最多同时采集 16 个 K64 */

/* 每节点最大 slot 数 */
#define COLLECT_MAX_SLOTS 6

/** 初始化采集模块 (创建 collect 目录) */
int can_collect_init(const struct device *can_dev);

/** 喂入回复帧, 解析并缓存测量值 */
void can_collect_feed(const struct can_frame *frame);

/** 轮询: 发送查询指令到各 K64 节点 */
void can_collect_poll(const struct device *can_dev);

/** 刷盘: 将缓存的采集数据写入 CSV */
void can_collect_flush(void);

#endif /* CAN_COLLECT_H */
