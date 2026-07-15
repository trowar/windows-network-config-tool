# Windows 网络配置小工具

## 下载

- [下载 Windows x86_64 版 ZIP](https://github.com/trowar/windows-network-config-tool/raw/main/dist/WindowsNetworkConfigTool-win-x64.zip)
- [下载 Windows ARM64 版 ZIP](https://github.com/trowar/windows-network-config-tool/raw/main/dist/WindowsNetworkConfigTool-win-arm64.zip)

一个原生 Win32 小工具，用于临时调整 Windows 网络配置。

## 功能

方便快捷的修改windows的hosts以及更新默认网卡dns和关闭ipv6

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
