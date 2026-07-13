# LASER DAQ

基于 Zephyr RTOS 的激光传感器数据采集固件，运行在 **NXP FRDM-MCXN947** 开发板上。

## 架构

```
激光传感器 —(CAN-FD)→ MCXN947 → LCD 实时显示
                              └→ Ethernet/ESP32 → 云端服务器 → ML 训练
```

## 目标板

- **FRDM-MCXN947** (NXP MCX N947, Cortex-M33, 150MHz)

## 子系统

| 子系统 | 说明 |
|--------|------|
| CAN 数据采集 | CAN-FD 接入激光传感器，捕获测量数据 |
| LCD 显示 | ST7796S 屏幕，通过 FlexIO 驱动，实时显示采集状态 |
| 网络上传 | Ethernet / ESP32 上传数据至服务器 |

## 构建

依赖 Zephyr RTOS + NXP MCUXpresso 工具链。

```bash
cmake --preset build -B build
cmake --build build
```

## 项目结构

```
LASER_DAQ/
├── src/main.c          # 主程序入口
├── CMakeLists.txt       # 构建脚本
├── CMakePresets.json    # CMake 预设（目标板/环境变量）
├── mcux_include.json    # MCUXpresso 环境变量
├── prj.conf             # Zephyr 内核配置
└── README.md
```
