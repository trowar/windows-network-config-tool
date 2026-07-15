# Windows 网络配置小工具

## 下载

- [下载 Windows x86_64 版 ZIP](https://github.com/trowar/windows-network-config-tool/raw/main/dist/WindowsNetworkConfigTool-win-x64.zip)
- [下载 Windows ARM64 版 ZIP](https://github.com/trowar/windows-network-config-tool/raw/main/dist/WindowsNetworkConfigTool-win-arm64.zip)

一个原生 Win32 小工具，用于临时调整 Windows 网络配置。

## 功能

- 自动请求管理员权限。
- 修改 hosts：
  - 点击 `修改 hosts` 后弹窗显示当前正在使用的 hosts 内容。
  - 确认后写回 hosts。
  - 如果当前 hosts 和启动时备份不一致，按钮显示 `恢复 hosts`。
- 修改 DNS：
  - 点击 `修改 DNS` 后弹窗输入 DNS。
  - 默认 DNS 为 `1.1.1.1` 和 `8.8.8.8`。
  - 修改成功后按钮变为 `恢复 DNS`。
  - 恢复时改回自动获取 DNS。
- 关闭 IPv6：
  - 点击 `关闭 IPv6` 会关闭当前默认出网网卡的 IPv6。
  - 成功后按钮变为 `恢复 IPv6`。
- 关闭软件时自动恢复已修改的 hosts、DNS 和 IPv6。
- 异常退出后，下次启动会优先使用 `latest-baseline` 恢复 hosts，避免把临时 hosts 误当成新备份。

## 构建

在 macOS 上使用 MinGW 交叉编译：

```bash
brew install mingw-w64
brew install zig
./build-macos.sh
```

## 说明

程序需要管理员权限，因为修改 hosts、DNS 和网卡 IPv6 绑定都需要提升权限。

hosts 备份文件位于：

```text
C:\ProgramData\TempWindowsHosts\
```
