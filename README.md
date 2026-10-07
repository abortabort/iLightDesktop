# iLight Desktop

使用 C++17 和 Qt 5.15.2 开发的 Windows 蓝牙灯控程序，界面采用 QDialog。

## 功能

- 搜索经典蓝牙与 iLight BLE 设备，支持输入 MAC 地址连接。
- Windows 原生 RFCOMM / SPP 连接，传输连接失败时回退 BLE。
- 开关、16 级亮度、RGB 调色、专用白光和灯具支持的冷暖白调节。
- 常亮、音乐律动、彩虹、呼吸、烛光五种灯效。
- 连接后读取灯具状态，反馈亮度、颜色和灯效。
- Windows 音箱配对、A2DP 音频服务启用和声音设置入口。
- 通信日志和无设备界面演示。

## 使用

先断开手机与灯具的连接，打开程序，搜索设备或输入地址，点击「连接灯具」。默认地址为 `C9:77:01:9C:8F:0D`，程序保存最近使用的地址。

颜色和灯效控件始终显示，无需选择设备类型。选择白光时使用白光通道，选择颜色或灯效时切回彩光通道。冷暖白控件仅在使用白光且灯具反馈支持时显示。

选择白色色块，或将 RGB 三个分量设为相同的非零值时，使用专用白光通道。RGB 均为 255 时设置白光亮度为 16。目标地址 `C9:77:01:9C:8F:0D` 按实机反馈交换红、绿通道，其他地址保持通用 RGB 顺序。

点击「连接音箱」启动 Windows 配对并启用音频服务，完成系统提示后，在「打开声音设置」中选择 iLight 输出。灯控和音频是独立连接；音频服务启用成功不等于音箱已连接。

「命令已发送」表示命令交给传输层；「灯具反馈」表示收到设备状态。灯光预览不代表实际灯具响应。日志为程序目录中的 `ilight.log`，配置为 `ilight.ini`，该目录需要可写。

```powershell
.\iLightDesktop.exe --connect C9:77:01:9C:8F:0D
```

## 构建

环境：Visual Studio 2019、MSVC v142 x64、C++17、动态运行库 `/MD`、Qt 5.15.2 动态版、CMake 3.21 或更新版本。vcpkg 目标 triplet 为 `x64-windows-static-md`，host 为 `x64-windows`。项目仅使用 Qt 模块。

```powershell
git clone https://github.com/abortabort/iLightDesktop.git
cd iLightDesktop
.\build.ps1 -QtPrefix 'D:\libs\Qt\5.15.2\msvc2019_64' -VcpkgRoot 'D:\libs\vcpkg'
```

脚本配置、编译并调用 windeployqt，生成 `dist\iLightDesktop` 和 `dist\iLightDesktop-windows-x64.zip`。Qt 与 vcpkg 路径按本机安装位置调整。已配置工程可使用 `-SkipConfigure`。

也可使用 CMake 预设：

```powershell
$env:VCPKG_ROOT = 'D:\libs\vcpkg'
$env:Qt5_DIR = 'D:\libs\Qt\5.15.2\msvc2019_64'
cmake --preset windows-vs2019
cmake --build --preset release
```

## 源码结构

| 文件 | 职责 |
| --- | --- |
| `src/maindialog.cpp` | 连接、灯控、预览和反馈界面 |
| `src/bluetoothclient.cpp` | 设备发现与连接生命周期 |
| `src/nativerfcomm.cpp` | Windows 原生 SPP 通信 |
| `src/nativeble.cpp` | Windows 原生 GATT 通信 |
| `src/nativeblescanner.cpp` | Windows 原生 BLE 扫描 |
| `src/nativeaudio.cpp` | 音箱配对与音频服务启用 |
| `src/session.cpp` | 初始化、握手和状态读取 |
| `src/protocol.cpp` | 帧编解码及业务协议 |

## 当前边界

Release 已通过编译。已在目标灯具上验证连接、状态回复和开关控制；颜色、亮度与灯效根据实机反馈调整，其他型号仍需确认兼容性。音箱配对与 A2DP 服务启用已实现，实际音频播放尚未验证。

不包括内置音乐播放器、闹钟、OTA 和分组控制。音乐播放依赖 Windows 音频输出。

## 协议与依赖

SPP UUID 为 `00001101-0000-1000-8000-00805f9b34fb`，发送 key 为 `0x5181`，接收 key 为 `0x4181`。BLE 通知服务 / 特征为 `6666` / `8888`，写入服务 / 特征为 `7777` / `8877`，采用 Bluetooth 基础 UUID；初始化字节为 `3031323334353637`，写入按实际 MTU 分块。普通业务命令使用明文内层协议，WA/LX 用于对应固件的握手。

音箱配对使用 Windows BluetoothAuthenticateDeviceEx，音频服务启用使用 BluetoothSetServiceState，Audio Sink UUID 为 `0000110b-0000-1000-8000-00805f9b34fb`。

Qt 动态链接，部署目录附带 Qt LGPLv3 许可。Qt 5.15.2 源码可从 [Qt 官方归档](https://download.qt.io/archive/qt/5.15/5.15.2/single/) 获取。
