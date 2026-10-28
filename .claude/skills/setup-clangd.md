---
name: setup-clangd
description: 为嵌入式 C/C++ 项目 (Zephyr/Bare-Metal) 配置 clangd 代码补全和 IntelliSense
category: configuration
tags: [clangd, vscode, embedded, zephyr, lsp]
trigger: 用户说"配置 clangd"、"clangd 补全"、"clangd 不工作"、"代码提示不弹"
---

# clangd 配置技能

为嵌入式 C/C++ 工程 (Zephyr RTOS / ARM GCC / CMake) 配置 clangd 作为 VSCode 的 C 语言服务器，
替代 Microsoft C/C++ (cpptools) 扩展，实现代码补全、跳转、悬停提示等功能。

## 适用场景

- Zephyr RTOS 项目
- ARM Cortex-M 嵌入式项目（CMake + arm-none-eabi-gcc / arm-zephyr-eabi-gcc）
- 任何有 `compile_commands.json` 的 C 项目
- 从 cpptools 迁移到 clangd 的项目

## 前置条件

1. 系统已安装 clangd: `sudo apt install clangd`（或 VSCode clangd 扩展自带捆绑版）
2. VSCode 已安装 clangd 扩展: `llvm-vs-code-extensions.vscode-clangd`
3. 项目能编译生成 `compile_commands.json`

## 配置结构

一个完整的 clangd 工程包含三层配置：

```
工程根目录/
├── .clangd              # clangd 服务端配置（YAML）
├── .vscode/
│   └── settings.json    # VSCode 工作区配置（JSON）
└── build/
    └── compile_commands.json  # CMake 生成的编译数据库
```

---

## 一、.clangd 文件

`.clangd` 是 clangd 服务端配置文件，控制编译参数、诊断、索引等行为。

### 嵌入式项目模板 (Zephyr / ARM GCC)

```yaml
CompileFlags:
  CompilationDatabase: build/
  Remove:                    # 移除 clang 不识别的 GCC 专用标志
    - "-fno-printf-return-value"
    - "-mfp16-format=ieee"
    - "-fno-reorder-functions"
    - "-specs=*"             # clang 不支持 specs 文件
    - "-mcmse"               # ARM CMSE, clang 可能不支持
    - "-mtp=*"               # GCC thread pointer model

Diagnostics:
  Suppress:
    - "drv_unknown_argument"  # 抑制未知参数警告

Index:
  Background: Build          # 后台构建索引，不影响补全

InlayHints:
  Enabled: Yes               # 内联类型提示
  ParameterNames: Yes         # 参数名提示
  DeducedTypes: Yes           # 推导类型提示

Hover:
  ShowAKA: Yes               # 显示类型别名
```

### 关键字段说明

| 字段 | 说明 |
|------|------|
| `CompilationDatabase` | 指向 `compile_commands.json` 的目录 |
| `Remove` | 移除 clang 不认识的 GCC 标志，防止解析失败 |
| `Diagnostics.Suppress` | 抑制 clang 不认识的 `-W` 或 `-f` 标志警告 |
| `Index.Background` | `Build` 后台构建索引；`Skip` 禁用 |

### 如何找到需要移除的标志

```bash
# 运行 clangd 诊断，观察报 unknown argument 的标志
clangd --check=src/main.c 2>&1 | grep "unknown argument"
```

---

## 二、VSCode 工作区设置 (.vscode/settings.json)

```jsonc
{
  // 禁用 cpptools（如果已安装）- 防止扩展冲突
  "C_Cpp.intelliSenseEngine": "disabled",

  // clangd 补全配置
  "clangd.serverCompletionRanking": true,
  "clangd.arguments": [
    "--limit-results=200",           // 每次补全最多返回 200 条
    "--completion-style=detailed"     // 详细补全信息
  ],

  // 补全触发器调优 - 消除延迟，删除重输后立即触发
  "editor.quickSuggestionsDelay": 0,
  "editor.suggest.snippetsPreventQuickSuggestions": false,
  "[c]": {
    "editor.quickSuggestions": {
      "other": "on",
      "comments": "off",
      "strings": "off"
    }
  }
}
```

### VSCode 设置说明

| 设置 | 默认值 | 推荐值 | 原因 |
|------|--------|--------|------|
| `C_Cpp.intelliSenseEngine` | `Default` | `disabled` | 防止 cpptools 抢占 clangd |
| `clangd.serverCompletionRanking` | `true` | `true` | 服务端排序比客户端更可靠 |
| `editor.quickSuggestionsDelay` | `10` | `0` | 消除延迟，输入即触发补全 |
| `editor.suggest.snippetsPreventQuickSuggestions` | `true` | `false` | 不让 snippet 拦截普通补全 |

> **注意**: `clangd.path` 通常不需要设置，扩展会自动使用捆绑版 clangd。
> 若需使用系统版: `"clangd.path": "/usr/bin/clangd"`

---

## 三、VSCode Profile 配置

如果使用 VSCode Profile 隔离不同工程环境：

1. 确保 Zephyr/嵌入式 Profile 中**不安装** `ms-vscode.cpptools`
2. 确保安装了 `llvm-vs-code-extensions.vscode-clangd`
3. 清理 profile settings.json 中残留的 `C_Cpp.*` 设置
4. 推荐在 profile 中设置 `"[c]": { "editor.defaultFormatter": "llvm-vs-code-extensions.vscode-clangd" }`

### 检查 Profile 扩展

```bash
# 找到你的 profile 目录
ls ~/.config/Code/User/profiles/

# 检查某个 profile 的扩展列表
cat ~/.config/Code/User/profiles/<profile-id>/extensions.json | grep -E "cpptools|clangd"
```

---

## 四、验证 clangd 是否正常工作

### 1. 命令行验证

```bash
cd <工程目录>
clangd --check=src/main.c
```

预期输出应包含：
```
Building preamble...    ✓
Building AST...         ✓
Indexing AST...         ✓
```

**不要**出现 `unknown argument` 错误。如有，将对应标志加入 `.clangd` 的 `Remove` 列表。

### 2. VSCode 内验证

- 打开 `main.c`，输入已知符号（如 `LOG_INF`），应弹出补全列表
- 鼠标悬停在符号上，应显示类型信息（设置了 `ShowAKA: Yes`）
- `Ctrl+Click` 应能跳转到定义

### 3. 查看 clangd 日志

VSCode → `View` → `Output` → 下拉选 `clangd`

确认日志中有：
```
Loaded compilation database from build/compile_commands.json
Built preamble of size XXXXX for file src/main.c
```

---

## 五、常见问题排查

### Q1: 补全完全不出

1. 检查 cpptools 是否还在运行: `ps aux | grep cpptools`
2. 如果 cpptools 在运行 → 从该 Profile 卸载或设为 `C_Cpp.intelliSenseEngine: disabled`
3. 重载 VSCode: `Ctrl+Shift+P` → `Developer: Reload Window`

### Q2: 刚重启有补全，过一会没了

**根因**: 某次补全返回 0 结果后，VSCode 补全触发器进入冷却。

**修复**: 在 `.vscode/settings.json` 中设置:
```json
"editor.quickSuggestionsDelay": 0,
"editor.suggest.snippetsPreventQuickSuggestions": false
```

### Q3: 删除后重新输入，补全不弹

同上 Q2，VSCode 的 10ms 延迟配合 clangd preamble 重建窗口导致。`quickSuggestionsDelay: 0` 可修复。

### Q4: clangd 报 "unknown argument"

将报错的 flag 加入 `.clangd` 的 `CompileFlags.Remove` 列表。

常见需移除的 ARM GCC 标志：
```
-mfp16-format=ieee
-fno-reorder-functions
-fno-printf-return-value
-specs=*
-mcmse
-mtp=*
```

### Q5: 头文件找不到 / "file not found"

1. 确认 `compile_commands.json` 已生成（CMake 项目需先 configure）
2. 确认 `.clangd` 中 `CompilationDatabase` 路径正确
3. 确认包含路径使用了 `-I` 而非 GCC 的 `-isystem`

### Q6: 多窗口 / Profile 环境

不会互相影响。每个 VSCode Profile 有独立的扩展集合，不同窗口的 cpptools/clangd 互不干扰。

---

## 六、从 cpptools 迁移检查清单

- [ ] 清理 `.vscode/c_cpp_properties.json`（可删除，clangd 不使用）
- [ ] 清理 `.vscode/settings.json` 中所有 `C_Cpp.*` 设置
- [ ] 在 Profile 的 settings.json 中移除 `C_Cpp.autocompleteAddParentheses` 等设置
- [ ] 在 Profile 中卸载或禁用 cpptools 扩展
- [ ] 确认只有 clangd 扩展在处理 C/C++ 文件
- [ ] 重载 VSCode 窗口
- [ ] 用 `clangd --check` 验证
