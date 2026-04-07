# ESP32-S3 开发环境配置指南 (macOS + ESP-IDF + VS Code)

## 1. 系统要求

- **操作系统**: macOS 12+ (Monterey 或更新)
- **磁盘空间**: 至少 10GB 可用空间
- **内存**: 建议 8GB+
- **网络**: 稳定的互联网连接（下载大量依赖）

---

## 2. 安装 Homebrew（如未安装）

```bash
/bin/bash -c "$(curl -fsSL https://raw.githubusercontent.com/Homebrew/install/HEAD/install.sh)"
```

---

## 3. 安装必要依赖

```bash
# 基础工具
brew install git wget flex bison gperf cmake ninja ccache

# Python 3（ESP-IDF 需要）
brew install python@3.11

# 串口工具（用于查看日志和烧录）
brew install minicom

# 可选：图形化串口工具
brew install --cask serial
```

---

## 4. 安装 ESP-IDF

### 4.1 创建安装目录

```bash
mkdir -p ~/esp
cd ~/esp
```

### 4.2 克隆 ESP-IDF 仓库

```bash
# 使用 v5.2.1 稳定版（推荐）
git clone -b v5.2.1 --recursive https://github.com/espressif/esp-idf.git esp-idf-v5.2.1

# 或者使用最新 master（可能不稳定）
# git clone --recursive https://github.com/espressif/esp-idf.git
```

### 4.3 运行安装脚本

```bash
cd ~/esp/esp-idf-v5.2.1
./install.sh esp32s3
```

> 此步骤会下载约 2GB 的工具链和包，耗时 10-30 分钟，请耐心等待。

### 4.4 设置环境变量

添加到 `~/.zshrc` 或 `~/.bash_profile`：

```bash
# ESP-IDF
alias get_idf='. $HOME/esp/esp-idf-v5.2.1/export.sh'
```

然后重新加载配置：

```bash
source ~/.zshrc
```

### 4.5 验证安装

```bash
get_idf
idf.py --version
```

应输出类似：`ESP-IDF v5.2.1`

---

## 5. 配置 VS Code

### 5.1 安装 VS Code

```bash
brew install --cask visual-studio-code
```

### 5.2 安装必要插件

在 VS Code 扩展面板搜索并安装：

| 插件名 | 用途 |
|--------|------|
| **ESP-IDF** | 官方 ESP-IDF 插件（必需） |
| **C/C++** | Microsoft 官方 C/C++ 扩展 |
| **CMake** | CMake 语法支持 |
| **CMake Tools** | CMake 项目支持 |

### 5.3 配置 ESP-IDF 插件

1. 按 `Cmd+Shift+P` 打开命令面板
2. 输入 `ESP-IDF: Configure ESP-IDF Extension`
3. 选择配置方式：
   - **ADVANCED** → 使用已有 ESP-IDF
4. 配置选项：
   - ESP-IDF Path: `~/esp/esp-idf-v5.2.1`
   - ESP-IDF Tools Path: `~/.espressif`
   - Python 路径: `/opt/homebrew/bin/python3` (Apple Silicon) 或 `/usr/local/bin/python3` (Intel)

### 5.4 验证插件配置

1. 按 `Cmd+Shift+P`
2. 输入 `ESP-IDF: Doctor Command`
3. 检查输出，确认无错误

---

## 6. 项目初始化

### 6.1 克隆本项目

```bash
cd ~/Documents/workspace/personal/esp32
# 或者你的项目路径
git clone <your-repo-url> esp32-dashboard
cd esp32-dashboard
```

### 6.2 初始化 ESP-IDF 环境

```bash
get_idf
```

### 6.3 配置项目

```bash
idf.py set-target esp32s3
idf.py menuconfig
```

在 menuconfig 中需要配置：

```
→ Serial flasher config
  → 'idf.py monitor' baud rate → 115200

→ Component config
  → LVGL configuration
    → [ ] LV_USE_PERF_MONITOR (性能监控，调试用)
    → [ ] LV_USE_MEM_MONITOR (内存监控，调试用)

→ Wi-Fi
  → [ ] Enable Wi-Fi debug logs (调试用)
```

### 6.4 保存配置

按 `S` 保存，然后按 `Q` 退出。

---

## 7. 构建和烧录

### 7.1 完整构建流程

```bash
# 1. 确保 ESP-IDF 环境已激活
get_idf

# 2. 编译项目
idf.py build

# 3. 连接 ESP32 到电脑（Type-C 线）

# 4. 检查串口设备
ls /dev/cu.*
# 应该能看到类似 /dev/cu.usbserial-XXXX 或 /dev/cu.wchusbserialXXXX

# 5. 烧录固件
idf.py -p /dev/cu.usbserial-XXXX flash

# 6. 查看串口日志
idf.py -p /dev/cu.usbserial-XXXX monitor

# 或者组合命令（编译+烧录+监控）
idf.py -p /dev/cu.usbserial-XXXX flash monitor
```

### 7.2 退出监控

按 `Ctrl+]` 退出串口监控。

---

## 8. VS Code 工作流

### 8.1 打开项目

```bash
code ~/Documents/workspace/personal/esp32/esp32-dashboard
```

### 8.2 使用状态栏按钮

VS Code 底部状态栏提供快捷操作：

| 按钮 | 功能 |
|------|------|
| 🚀 ESP-IDF Build | 编译项目 |
| 📤 Flash | 烧录固件 |
| 📺 Monitor | 打开串口监控 |
| 🧹 Full Clean | 清理构建文件 |

### 8.3 快捷键

| 快捷键 | 功能 |
|--------|------|
| `Cmd+Shift+P` → `ESP-IDF: Build` | 编译 |
| `Cmd+Shift+P` → `ESP-IDF: Flash` | 烧录 |
| `Cmd+Shift+P` → `ESP-IDF: Monitor` | 监控日志 |

---

## 9. 常见问题解决

### 9.1 串口设备找不到

**症状**: `/dev/cu.usbserial-XXXX` 不存在

**解决**:
```bash
# 1. 安装 CH340/CP2102 驱动（根据你的 USB 转串芯片）
# ESP32-S3 通常使用 USB OTG，不需要额外驱动

# 2. 检查系统报告
# 左上角  → 关于本机 → 系统报告 → USB
# 查看是否有 "USB Serial" 或 "Silicon Labs" 设备

# 3. 使用 socat 创建虚拟串口（高级）
brew install socat
```

### 9.2 权限问题

**症状**: `Permission denied: /dev/cu.usbserial-XXXX`

**解决**:
```bash
# 临时方案
sudo chmod 666 /dev/cu.usbserial-XXXX

# 永久方案（推荐）
# 创建 udev 规则（虽然 macOS 不用 udev，但可添加用户到 dialout 组）
sudo dseditgroup -o edit -a $USER -t user dialout
# 重新登录后生效
```

### 9.3 编译错误：找不到头文件

**症状**: `fatal error: xxx.h: No such file or directory`

**解决**:
```bash
# 1. 清理并重新配置
idf.py fullclean
idf.py set-target esp32s3
idf.py build

# 2. 检查 submodules 是否完整
cd ~/esp/esp-idf-v5.2.1
git submodule update --init --recursive
```

### 9.4 Python 版本问题

**症状**: `ModuleNotFoundError` 或 Python 版本不匹配

**解决**:
```bash
# 检查 Python 版本
python3 --version  # 需要 3.8+

# 重新安装 ESP-IDF Python 依赖
cd ~/esp/esp-idf-v5.2.1
./install.sh esp32s3 --reinstall

# 如果使用 pyenv，确保系统 Python 可用
pyenv global system
```

### 9.5 烧录失败：连接超时

**症状**: `Failed to connect to ESP32: Timed out waiting for packet header`

**解决**:
```bash
# 1. 按住 BOOT 键，然后按 RESET 键，进入下载模式
# 2. 或者使用自动下载电路（ESP32-S3 开发板通常支持）
idf.py -p /dev/cu.usbserial-XXXX flash

# 3. 降低烧录波特率
idf.py -p /dev/cu.usbserial-XXXX -b 115200 flash
```

### 9.6 macOS 安全提示

**症状**: 无法打开来自未知开发者的工具

**解决**:
- 系统偏好设置 → 安全性与隐私 → 通用 → 允许
- 或按住 Control 键点击应用，选择"打开"

---

## 10. 项目特定配置

### 10.1 WiFi 配置

在项目根目录创建 `wifi_config.txt`（不要提交到 git）：

```
WIFI_SSID=your_wifi_name
WIFI_PASSWORD=your_wifi_password
```

然后在 `menuconfig` 中：
```
→ Example Connection Configuration
  → [ ] Connect using Ethernet
  → [*] Connect using Wi-Fi interface
  → Wi-Fi SSID → 输入你的 SSID
  → Wi-Fi Password → 输入密码
```

### 10.2 调试配置

在 `.vscode/launch.json` 中添加：

```json
{
  "version": "0.2.0",
  "configurations": [
    {
      "name": "GDB",
      "type": "cppdbg",
      "request": "launch",
      "program": "${workspaceFolder}/build/esp32-dashboard.elf",
      "cwd": "${workspaceFolder}",
      "miDebuggerPath": "~/.espressif/tools/xtensa-esp-elf-gdb/12.1_20231023/xtensa-esp-elf-gdb/bin/xtensa-esp32s3-elf-gdb",
      "setupCommands": [
        {
          "text": "target remote :3333"
        }
      ]
    }
  ]
}
```

---

## 11. 推荐工作流

### 日常开发循环

```bash
# 1. 打开终端，激活环境
get_idf

# 2. 进入项目目录
cd ~/Documents/workspace/personal/esp32/esp32-dashboard

# 3. 使用 VS Code 编辑代码
code .

# 4. 编译（在 VS Code 终端或命令行）
idf.py build

# 5. 烧录并监控
idf.py -p /dev/cu.usbserial-XXXX flash monitor
```

### Git 提交前检查

```bash
# 1. 确保能编译通过
idf.py build

# 2. 清理构建文件（减小仓库大小）
# 这些文件在 .gitignore 中，不需要提交
```

---

## 12. 参考资源

### 官方文档
- [ESP-IDF 编程指南](https://docs.espressif.com/projects/esp-idf/en/latest/esp32s3/)
- [ESP32-S3 技术参考手册](https://www.espressif.com/sites/default/files/documentation/esp32-s3_technical_reference_manual_en.pdf)
- [Waveshare ESP32-S3-Touch-LCD-3.49 Wiki](https://www.waveshare.net/wiki/ESP32-S3-Touch-LCD-3.49)

### 社区资源
- [ESP32 论坛](https://esp32.com/)
- [LVGL 文档](https://docs.lvgl.io/)

### 工具
- [ESP-IDF VS Code 插件文档](https://github.com/espressif/vscode-esp-idf-extension/blob/master/docs/tutorial/toc.md)

---

## 13. 更新日志

| 日期 | 版本 | 变更 |
|------|------|------|
| 2025-04-06 | 1.0 | 初始版本，基于 ESP-IDF v5.2.1 |

---

*如有问题，请检查 ESP-IDF 官方文档或提交 Issue。*
