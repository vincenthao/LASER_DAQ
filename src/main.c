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

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

LOG_MODULE_REGISTER(laser_daq, LOG_LEVEL_INF);

/* 子系统初始化 */
static int init_can_collector(void);
static int init_lcd_display(void);
static int init_network_upload(void);

int main(void)
{
    printk("\n========================================\n");
    printk("  MCXN947 Laser Data Collector\n");
    printk("========================================\n\n");

    LOG_INF("System boot complete, initializing subsystems...");

    /* 1. CAN 数据采集子系统 */
    if (init_can_collector() < 0) {
        LOG_ERR("CAN collector init failed");
    }

    /* 2. LCD 显示子系统 */
    if (init_lcd_display() < 0) {
        LOG_ERR("LCD display init failed");
    }

    /* 3. 网络上传子系统 */
    if (init_network_upload() < 0) {
        LOG_ERR("Network upload init failed");
    }

    LOG_INF("All subsystems initialized, entering main loop");

    while (1) {
        /* TODO: 主循环 - 协调各子系统 */
        k_msleep(1000);
    }

    return 0;
}

/* ---- 子系统初始化（占位） ---- */

static int init_can_collector(void)
{
    LOG_INF("CAN collector: initializing CAN-FD for laser sensor...");
    /* TODO: 配置 CAN-FD，绑定激光传感器协议 */
    return 0;
}

static int init_lcd_display(void)
{
    LOG_INF("LCD display: initializing ST7796S via FlexIO...");
    /* TODO: 初始化 LCD，显示采集状态界面 */
    return 0;
}

static int init_network_upload(void)
{
    LOG_INF("Network upload: initializing Ethernet/ESP32...");
    /* TODO: 初始化网络栈，连接上传服务器 */
    return 0;
}
