# ESP32-S3 开发环境配置指南 (macOS + ESP-IDF 6.0 + VS Code)

## 1. 目标

本文档描述当前项目推荐的开发环境：

- 操作系统：macOS
- IDE：VS Code
- SDK：通过 Espressif Install Manager 安装的全局 `ESP-IDF 6.0`

本项目不再依赖仓库内的本地 `esp-idf-v5.2.1` checkout。

## 2. 系统要求

- macOS 12 或更高版本
- 至少 10GB 可用磁盘空间
- 建议 8GB 以上内存
- 稳定网络连接

## 3. 安装基础工具

如未安装 Homebrew：

```bash
/bin/bash -c "$(curl -fsSL https://raw.githubusercontent.com/Homebrew/install/HEAD/install.sh)"
```

安装常用命令行工具：

```bash
brew install git wget cmake ninja ccache minicom
brew install --cask visual-studio-code
```

说明：

- Python 和 ESP-IDF 工具链由 Install Manager 管理，不再手动在仓库内安装一份 ESP-IDF
- `minicom` 仅用于串口备选调试，日常优先用 VS Code 的 ESP-IDF 集成能力

## 4. 安装和选择 ESP-IDF 6.0

### 4.1 使用 Espressif Install Manager

如果你已经通过 Install Manager 安装了 ESP-IDF 6.0，本节的目标只是确认它处于可用状态。

推荐路径：

1. 打开 VS Code
2. 安装 Espressif 官方扩展 `ESP-IDF`
3. 打开命令面板，执行 `ESP-IDF: Configure ESP-IDF Extension`
4. 选择由 Install Manager 安装的 `ESP-IDF 6.0`
5. 等待扩展完成工具链、Python 环境和路径配置

### 4.2 验证环境

在 VS Code 中执行：

1. `ESP-IDF: Doctor Command`
2. `ESP-IDF: Open ESP-IDF Terminal`

然后在该终端中运行：

```bash
idf.py --version
```

预期输出类似：

```text
ESP-IDF v6.0
```

如果终端里没有 `idf.py`，优先检查 VS Code 扩展配置，而不是在仓库里再 clone 一份 ESP-IDF。

## 5. 推荐 VS Code 扩展

主流推荐组合：

| 扩展 | 说明 |
|------|------|
| `espressif.esp-idf-extension` | 官方必装，负责 ESP-IDF 安装、配置、构建、烧录、监控、调试 |
| `ms-vscode.cpptools` | 最通用的 C/C++ 语言服务 |

可选：

| 扩展 | 说明 |
|------|------|
| `llvm-vs-code-extensions.vscode-clangd` | 如你偏好 clangd，可作为 `cpptools` 的替代 |
| `ms-vscode.cmake-tools` | 对通用 CMake 工程有帮助，但 ESP-IDF 项目不是必需 |

建议：

- 大多数开发者使用 `ESP-IDF + cpptools`
- 不建议在同一工作区同时重度依赖 `cpptools` 和 `clangd`

## 6. 项目打开方式

当前仓库根目录就是项目目录：

```bash
cd ~/Documents/workspace/personal/esp32
code .
```

不要再使用旧文档中的 `esp32-dashboard` 子目录示例。

## 7. 推荐工作流

### 7.1 日常开发

1. 在 VS Code 中打开本仓库
2. 执行 `ESP-IDF: Open ESP-IDF Terminal`
3. 在该终端中运行 ESP-IDF 命令

示例：

```bash
idf.py set-target esp32s3
idf.py build
```

### 7.2 常用命令

```bash
idf.py set-target esp32s3
idf.py menuconfig
idf.py build
idf.py -p /dev/cu.usbserial-XXXX flash
idf.py -p /dev/cu.usbserial-XXXX monitor
idf.py -p /dev/cu.usbserial-XXXX flash monitor
```

### 7.3 VS Code 常用入口

| 命令 | 功能 |
|------|------|
| `ESP-IDF: Build your project` | 编译 |
| `ESP-IDF: Flash your project` | 烧录 |
| `ESP-IDF: Monitor your device` | 查看串口日志 |
| `ESP-IDF: Full Clean` | 清理构建目录 |
| `ESP-IDF: Open ESP-IDF Terminal` | 打开已配置环境的终端 |

## 8. 常见问题

### 8.1 `idf.py` 不可用

症状：

- `command not found: idf.py`
- VS Code 终端里找不到 ESP-IDF 工具链

处理方式：

1. 重新运行 `ESP-IDF: Configure ESP-IDF Extension`
2. 执行 `ESP-IDF: Doctor Command`
3. 确认当前选中的版本是 Install Manager 管理的 `ESP-IDF 6.0`
4. 使用 `ESP-IDF: Open ESP-IDF Terminal`，不要依赖旧的 `get_idf` alias

### 8.2 串口设备找不到

症状：

- `/dev/cu.usbserial-*` 不存在

处理方式：

```bash
ls /dev/cu.*
```

然后检查：

- USB 线是否支持数据传输
- 板卡是否正确进入下载模式
- macOS 系统报告里是否识别出 USB 串口设备

### 8.3 烧录失败：连接超时

症状：

- `Timed out waiting for packet header`

处理方式：

```bash
idf.py -p /dev/cu.usbserial-XXXX -b 115200 flash
```

必要时手动按住 `BOOT` 再按 `RESET` 进入下载模式。

### 8.4 头文件或组件索引异常

症状：

- VS Code 提示找不到头文件
- IntelliSense 失效

处理方式：

```bash
idf.py fullclean
idf.py set-target esp32s3
idf.py build
```

然后重载 VS Code 窗口。

### 8.5 Python 或工具链异常

症状：

- 扩展报 Python 环境错误
- 工具链路径错乱

处理方式：

优先在 VS Code 中重新执行：

- `ESP-IDF: Configure ESP-IDF Extension`
- `ESP-IDF: Doctor Command`

不要回退到仓库内手动安装旧版 ESP-IDF。

## 9. 项目约定

- ESP-IDF 由全局 Install Manager 管理，不提交到本仓库
- 仓库内不再保留本地 `esp-idf-v5.2.1` checkout
- 若未来需要锁定更具体的 ESP-IDF 小版本，应在文档和固件工程配置中一起更新

## 10. 参考资源

- ESP-IDF 版本说明: https://docs.espressif.com/projects/esp-idf/en/stable/esp32/versions.html
- ESP-IDF 6.0 迁移指南: https://docs.espressif.com/projects/esp-idf/en/stable/esp32/migration-guides/release-6.x/6.0/index.html
- ESP-IDF VS Code 扩展安装文档: https://docs.espressif.com/projects/vscode-esp-idf-extension/en/latest/installation.html
- ESP-IDF VS Code Profiles: https://docs.espressif.com/projects/vscode-esp-idf-extension/en/latest/additionalfeatures/esp-idf-profiles.html
- Waveshare ESP32-S3-Touch-LCD-3.49 Wiki: https://www.waveshare.com/wiki/ESP32-S3-Touch-LCD-3.49

## 11. 更新日志

| 日期 | 版本 | 变更 |
|------|------|------|
| 2026-04-07 | 2.0 | 切换到通过 Install Manager 管理的全局 ESP-IDF 6.0 工作流 |
| 2025-04-06 | 1.0 | 初始版本，基于本地 ESP-IDF v5.2.1 checkout |
