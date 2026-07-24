/*
 * can_collect.h — CAN 测量数据主动采集模块
 *
 * MCXN947 通过 SETREGS 配置 K64 的 Monitor 周期寄存器,
 * K64 自动周期性广播 RPTCURR/RPTTEMP (主动上报),
 * MCXN947 接收解析后以窄表格式 (node_id,slot,func,tp,val_float) 存入 CSV
 */

#ifndef CAN_COLLECT_H
#define CAN_COLLECT_H

#include <stdint.h>                     /* uint8_t, uint32_t */
#include <zephyr/drivers/can.h>         /* struct can_frame */

/* 轮询间隔 (ms) — 用于节点发现和配置检查 */
#define COLLECT_POLL_PERIOD_MS 500

/* 最大可发现节点数 */
#define COLLECT_MAX_NODES 16            /* 最多同时采集 16 个 K64 */

/* 每节点最大 slot 数 */
#define COLLECT_MAX_SLOTS 6

/* 默认主动上报周期 (ms) — 可通过 Period=N 串口命令修改, 重启生效 */
#define COLLECT_REPORT_PERIOD_DEFAULT 500    /* 每 500ms 上报一次 */
extern uint32_t g_collect_period_ms;          /* 当前使用的上报周期 */

/* ---- K64 操作码 (对齐 GSK5G_MCU GS_REGDEF.h) ---- */

#define OP_S_TEMP   5                   /* 写温度参数 (配置 Monitor 周期) */
#define OP_R_TEMP   6                   /* 读温度参数 (读回 Monitor 周期) */
#define OP_S_CURR   7                   /* 写电流参数 (配置 Monitor 周期) */
#define OP_R_CURR   8                   /* 读电流参数 (读回 Monitor 周期) */

/* ---- Monitor 周期寄存器 typecode (对齐 K64 GS_REGDEF.h) ---- */

/* 电流 Monitor (OP_S_CURR=7 / OP_R_CURR=8) */
#define TC_MON_C_IREAD       100        /* LD 电流原始值 */
#define TC_MON_C_IREAD_SAMP  101        /* LD 电流采样值 */
#define TC_MON_C_LDV         102        /* LD 电压 */
#define TC_MON_C_LDPS        103        /* LD 功率 I×V */
#define TC_MON_C_LDP         104        /* LD 光功率 */
#define TC_MON_C_DRIV        105        /* 驱动电压 */
#define TC_MON_C_VCE         106        /* VCE 电压 */
#define TC_MON_C_ISET        107        /* 电流目标值 */
#define TC_MON_C_SW          108        /* 开关状态 */

/* 温度 Monitor (OP_S_TEMP=5 / OP_R_TEMP=6) */
#define TC_MON_T_T1          100        /* T1 温度原始值 */
#define TC_MON_T_T1_SAMP     101        /* T1 温度采样值 */
#define TC_MON_T_T2          102        /* T2 温度/湿度 */
#define TC_MON_T_T3          103        /* T3 温度 */
#define TC_MON_T_TEC_DUTY    104        /* TEC 占空比 */
#define TC_MON_T_TEC_I       105        /* TEC 电流 */
#define TC_MON_T_TEC_V       106        /* TEC 电压 */
#define TC_MON_T_TEC_P       107        /* TEC 功率 */
#define TC_MON_T_SW          108        /* 开关状态 */

/* ---- 主动上报 TP 类型码 (对齐 K64 GS_TYPEDEF.h) ---- */

#define TP_NOSAMP    0                  /* 原始值 (无采样) */
#define TP_TEMPSW    5                  /* 温度开关状态 */
#define TP_CURSW     7                  /* 电流开关状态 */
#define TP_IREAL     8                  /* LD 电流实际值 */
#define TP_LDV       9                  /* LD 电压 */
#define TP_PSUMP     10                 /* LD 功率 I×V */
#define TP_P         11                 /* LD 光功率 */
#define TP_DIRV      12                 /* 驱动电压 */
#define TP_VCE       13                 /* VCE 电压 */
#define TP_ISET      14                 /* 电流目标值 */
#define TP_TREAL     21                 /* T1 温度 */
#define TP_T2REAL    22                 /* T2 温度 */
#define TP_T3REAL    23                 /* T3 温度 */
#define TP_TECDUTY   24                 /* TEC 占空比 */
#define TP_TECI      25                 /* TEC 电流 */
#define TP_TECV      26                 /* TEC 电压 */
#define TP_TECP      27                 /* TEC 功率 */

/** 初始化采集模块 (创建 collect 目录, 加载周期配置, 启动序号) */
int can_collect_init(const struct device *can_dev);

/** 喂入 CAN 帧, 心跳发现 + 解析主动上报 */
void can_collect_feed(const struct can_frame *frame);

/** 轮询: 发现新节点时配置 Monitor 周期, 已配置节点跳过 */
void can_collect_poll(const struct device *can_dev);

/** 刷盘: 将缓存的采集数据写入窄表 CSV, 同一批次共享 sample_seq */
void can_collect_flush(void);

/** 自定义上报周期: 写入配置文件, 需重启生效 */
void can_collect_set_period(uint32_t period_ms);

/** 查询当前上报周期 (ms) */
uint32_t can_collect_get_period(void);

/** 启动数据采集: 重置 seq, 新 CSV 文件, 开始写盘 */
void can_collect_start(void);

/** 停止数据采集: flush 缓冲后暂停写盘 */
void can_collect_stop(void);

/** 查询采集是否活跃 */
bool can_collect_is_active(void);

#endif /* CAN_COLLECT_H */
