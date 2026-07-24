# MCXN947 串口指令参考

串口参数：115200 8N1

---

## 指令列表

| 指令 | 参数 | 说明 |
|------|------|------|
| `CollectStart` | 无 | 启动数据采集，创建新 CSV 文件 |
| `CollectStop` | 无 | 停止数据采集，flush 缓冲区 |
| `Collect?` | 无 | 查询采集状态（ACTIVE / STOPPED） |
| `Period=xxx` | 50~60000 | 设置上报周期（ms），需重启生效 |
| `Period?` | 无 | 查询当前上报周期 |
| `EraseFlash` | 无 | 擦除并格式化 NOR Flash，自动冷重启 |

---

## 指令详解

### `CollectStart`

```
> CollectStart
Collect started, boot_seq=2
```

- 重置 `sample_seq` 为 0
- 自动递增 `boot_seq`，创建新 CSV（如 `data_0002.csv`）
- **心跳发现和 Monitor 配置始终运行**，不受开关影响

### `CollectStop`

```
> CollectStop
Collect stopped (seq=42)
```

- flush 缓冲中剩余数据到 CSV
- 暂停写盘，CAN 数据不再缓冲

### `Collect?`

```
> Collect?
Collect: ACTIVE (500ms)
```

- 显示当前状态和上报周期

### `Period=xxx`

```
> Period=200
Period set to 200 ms (reboot required)
```

- 写入 `/NAND:/collect/period.cfg`
- 有效范围：50 ~ 60000 ms
- **运行时不生效**，需重启后加载

### `Period?`

```
> Period?
Period: 500 ms (default 500)
```

- 显示当前生效的上报周期

### `EraseFlash`

```
> EraseFlash

=== EraseFlash START ===
Unmount: 0
Format: 0 (OK)
Rebooting in 1s...
```

- 卸载 FatFS → 格式化 → 冷重启
- 重启后自动挂载并重建 `sniff/`、`collect/` 目录
- **所有数据将丢失，谨慎使用**

---

## 典型工作流

```
# 1. 上电，等待激光器出光
Collect?                              # 确认 STOPPED

# 2. 开始采集
CollectStart                          # 启动，新文件 data_0001.csv

# 3. 可选：调整上报频率（需要提前设置好）
Period=200                            # 写配置
# 重启 MCXN947...

# 4. 数小时后停止
CollectStop                           # flush 并停止

# 5. 通过 USB MSC 导出 CSV 文件
```
