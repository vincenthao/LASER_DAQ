/*
 * can_sniffer.h — CAN 总线嗅探模块
 *
 * 全量捕获 CAN 总线上所有帧, 按 node_id 分目录存入 FatFS CSV 文件
 * 缓冲策略: 内存环形缓冲区 + 定时器 1s 批量刷盘
 */

#ifndef CAN_SNIFFER_H
#define CAN_SNIFFER_H

#include <stdint.h>                     /* uint8_t, uint32_t 等 */
#include <zephyr/drivers/can.h>         /* struct can_frame */

/* 环形缓冲区大小 (每 node_id 最大缓存帧数) */
#define SNIFF_BUF_SIZE  32

/* CSV 一行的最大字节数 */
#define SNIFF_CSV_LINE_MAX 128

/** 初始化嗅探模块 (创建 sniff 文件夹) */
int can_sniffer_init(void);

/** 喂入一帧, 存入对应 node_id 的缓冲区 */
void can_sniffer_feed(const struct can_frame *frame);

/** 批量刷盘: 将所有脏缓冲区的帧写入 CSV 文件 (主循环调用) */
void can_sniffer_flush(void);

#endif /* CAN_SNIFFER_H */
