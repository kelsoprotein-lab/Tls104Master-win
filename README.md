# Tls104Master-win

跨平台 IEC 60870-5-104 主站，基于 C++ + WebView2 实现。支持 Windows、macOS 和 Linux。

## 功能特性

- **多从站管理** - 支持同时连接多个 IEC 104 从站
- **TLS 安全连接** - 支持 SSL/TLS 加密通信 (TLS 1.2/1.3)
- **实时监控** - 遥信、遥测数据实时显示
- **报文监控** - 原始报文十六进制显示
- **遥控操作** - 支持单点、双点遥控及调节命令
- **连接管理** - 支持连接/断开站点
- **时钟同步** - 发送时钟同步命令
- **计数器读取** - 读取计数器/累计量数据
- **多 ASDU 类型** - 支持 Step Position、Bitstring、Counter、Protection Event
- **现代 UI** - 基于 Vue.js + Element Plus（可通过浏览器或 WebView 窗口访问）

## 系统要求

### Windows
- Windows 10/11 (需要 WebView2 运行时)
- CMake >= 3.14
- C++17 编译器 (MSVC, MinGW, 或 Clang)
- lib60870 依赖
- mbedTLS (可选，用于 TLS 支持)

### macOS
- macOS 10.14+
- CMake >= 3.14
- C++17 编译器 (Clang)
- lib60870 依赖
- mbedTLS (可选，用于 TLS 支持)

### Linux
- Ubuntu 20.04+ / Debian 11+ / Fedora 35+ 或其他主流 Linux 发行版
- CMake >= 3.14
- C++17 编译器 (GCC >= 7 或 Clang >= 5)
- lib60870 依赖（自动下载）
- mbedTLS（自动下载，用于 TLS 支持）
- 可选 GUI: libgtk-3-dev, libwebkit2gtk-4.0-dev

## 构建步骤

### 1. 安装依赖

**Windows:**
```bash
# 安装 Visual Studio Build Tools
# 下载并安装: https://visualstudio.microsoft.com/visual-cpp-build-tools/

# 安装 CMake
# 下载: https://cmake.org/download/
```

**macOS:**
```bash
# 使用 Homebrew 安装
brew install cmake
```

**Linux (Ubuntu/Debian):**
```bash
# 安装编译工具和 CMake
sudo apt-get update
sudo apt-get install -y cmake build-essential

# 可选：安装 GUI 依赖
# sudo apt-get install -y libgtk-3-dev libwebkit2gtk-4.0-dev
```

**Linux (Fedora/RHEL):**
```bash
# 安装编译工具和 CMake
sudo dnf install cmake gcc-c++ make

# 可选：安装 GUI 依赖
# sudo dnf install gtk3-devel webkit2gtk4.0-devel
```

### 2. 编译

```bash
# 创建构建目录
mkdir build
cd build

# 配置 (Windows - Visual Studio)
cmake .. -G "Visual Studio 17 2022" -A x64

# 配置 (Windows - Ninja)
cmake .. -G Ninja -DCMAKE_BUILD_TYPE=Release

# 配置 (macOS)
cmake .. -G Ninja -DCMAKE_BUILD_TYPE=Release

# 配置 (Linux)
cmake .. -G "Unix Makefiles" -DCMAKE_BUILD_TYPE=Release

# 编译
cmake --build . --config Release
```

### 3. 运行

```bash
# 编译完成后，运行可执行文件
./tls104_master_win
```

程序会自动打开系统浏览器访问 http://localhost:19876

### 可选：构建原生窗口模式

使用 CMake 选项 `ENABLE_WEBVIEW_GUI=ON` 构建独立窗口版本：

```bash
cmake .. -G Ninja -DENABLE_WEBVIEW_GUI=ON -DCMAKE_BUILD_TYPE=Release
cmake --build . --config Release
```

然后使用 `--gui` 参数运行：
```bash
./tls104_master_win --gui
```

## 下载

预编译的可执行文件可从 [GitHub Releases](../../releases/latest) 页面下载：

| 平台 | 架构 | 文件 |
|------|------|------|
| Windows | x64 | `tls104_master_win-<version>-windows-x64.zip` |
| macOS | Universal (arm64 + x86_64) | `tls104_master_win-<version>-macos-universal.tar.gz` |
| Linux | amd64 | `tls104_master_win-<version>-linux-amd64.tar.gz` |
| Linux | arm64 | `tls104_master_win-<version>-linux-arm64.tar.gz` |

其中 `<version>` 为版本号，如 `v1.0.13`。下载后解压即可运行，无需安装。

## 使用方法

1. 启动程序
2. 在浏览器中打开 http://localhost:19876
3. 点击"添加从站"按钮
4. 输入从站地址、端口、TLS 配置等
5. 点击"连接"建立连接
6. 点击"总召唤"获取数据
7. 监控实时数据变化

## 命令行选项

```
-p <port>        HTTP 服务器端口 (默认: 19876)
--no-browser     启动时不自动打开浏览器
--gui           使用原生窗口模式 (需要 ENABLE_WEBVIEW_GUI)
--help          显示帮助
```

## REST API

程序提供 HTTP API 用于程序化控制：

### 站点管理
```
GET    /api/stations              # 获取所有站点列表
POST   /api/stations              # 添加站点
DELETE /api/stations/:id         # 删除站点
POST   /api/stations/:id/connect # 连接站点
POST   /api/stations/:id/disconnect # 断开站点
```

### 控制命令
```
POST   /api/stations/:id/interrogation  # 总召唤
POST   /api/stations/:id/clock-sync      # 时钟同步
POST   /api/stations/:id/counter         # 计数器读取
POST   /api/stations/:id/control         # 遥控命令
```

### WebSocket 事件

连接 ws://localhost:19876 可接收实时事件：
- `connection` - 连接状态变化
- `digital` - 遥信数据
- `telemetry` - 遥测数据
- `packet` - 原始报文
- `control_result` - 遥控结果

## 项目结构

```
Tls104Master-win/
├── src/
│   ├── main.cpp           # 主入口
│   ├── platform/          # 跨平台 Socket
│   ├── ipc/              # IPC 桥接
│   ├── http/             # HTTP 服务器 + WebSocket
│   ├── iec104/           # IEC 104 连接管理
│   └── gui/              # WebView 窗口 (ENABLE_WEBVIEW_GUI)
├── web/
│   └── index.html        # 前端界面
├── certs/                # TLS 证书目录
├── CMakeLists.txt
└── README.md
```

## 技术栈

- **后端**: C++17, lib60870, mbedTLS
- **前端**: Vue 3, Element Plus
- **桌面**: WebView2 (Windows) / WebKit (macOS Linux)
- **构建**: CMake

## 许可证

Apache License 2.0
