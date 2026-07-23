该文档是当前项目会使用到的基于can的协议说明，k64从机端视角，代码参考：/home/johnny/Projects/K64/K64Project/Sources/APIClasses/CanbusAPI.cpp，/home/johnny/Projects/K64/K64Project/Sources/GS_TYPEDEF.h已经k64项目的`fetch_report`函数

# 功能码

## `FUNC_HEARTBT` 14  K64心跳包

## `FUNC_RPTREGS` 9  Opcode读取

## `FUNC_SETREGS` 10  Opcode设置

可以通过opcode设置数据采集上报内容的时间：

opcode为7，typcode：

`R_C_MONC_WITHSAMP`101  LD实测电流值

`R_C_MONLDV` 102  LD实测电压值

`R_C_MONLDPS` 103  LD实测功耗

`R_C_MONLDP` 104  LD实测功率

`R_C_MONDRIV` 105  驱动实测电压

`R_C_MONVCE` 106  VCE实测电压

`R_C_MONISET` 107  电流目标值

`R_C_MONSW` 108  电流开关状态值

opcode为5，typcde：

`R_T_MONT1_WITHSAMP` 101 第一温度实测值

`R_T_MONT2` 102  第二温度实测值

`R_T_MONT3` 103  第三温度实测值

`R_T_MONTEC` 104 TEC温控PWM强度

`R_T_MONTECI` 105 TEC电流实测值

`R_T_MONTICV` 106 TEC电压实测值

`R_T_MONTPS` 107 TEC功耗实测值

`R_T_MONSW` 108 温控开关状态值

## `FUNC_RPTCURR` 5  电流数据上报

typecode：

`TP_IREAL` 8  LD实测电流值

`TP_LDV` 9  LD实测电压值

`TP_PSUMP` 10  LD实测功耗

`TP_P` 11 LD实测功率

`TP_DIRV` 12  驱动实测电压

`TP_VCE` 13  VCE实测电压

`TP_ISET` 14  电流目标值

`TP_CURSW` 7  电流开关状态值

## `FUNC_RPTTEMP` 7 温度数据上报

typecode：

`TP_TREAL` 21  第一温度实测值

`TP_T2REAL` 22 第二温度实测值

`TP_TECDUTY` 24 TEC温控PWM强度

`TP_T3REAL` 23 第三温度实测值

`TP_TECI` 25  TEC电流实测值

`TP_TECV` 26  TEC电压实测值

`TP_TECP` 27 TEC功耗实测值

`TP_TEMPSW` 5 温控开关状态值
