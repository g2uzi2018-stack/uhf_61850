# 本地构建

当前仓库已经提供 host 和 AArch64 两个 CMake 配置。目标板不参与编译，也不需要在启动时下载依赖。

## 依赖

- CMake 3.22 或更新版本
- 支持 C++17 的 host 编译器和 Make
- AArch64 构建需要 aarch64-linux-gnu-g++ 及对应的 sysroot

在 Debian/Ubuntu 开发机上可安装交叉工具链：

    sudo apt-get install gcc-aarch64-linux-gnu g++-aarch64-linux-gnu

## 一键验证

在仓库根目录运行：

    bash tools/build-local.sh

脚本会配置并构建 host Debug、运行 CTest，再配置并构建 AArch64 Release，并用 readelf 检查产物架构。构建目录默认是 build/，可用 UHF_LOCAL_BUILD_ROOT 指定到工作区外的目录。

也可以分别使用 CMake preset：

    cmake --preset host-debug
    cmake --build --preset host-debug
    ctest --test-dir build/host --output-on-failure
    cmake --preset aarch64-release
    cmake --build --preset aarch64-release

format-check 是可选的 CMake 目标；安装 clang-format 后执行：

    cmake --build build/host --target format-check

## 界面预览

用户管理页面现在由 uhf-gatewayd 的 C++ 开发 HTTP 服务提供；账号、会话和密码 API 仍在后续安全增量中实现。启动开发服务：

    bash tools/run-web-local.sh

在 Windows 浏览器打开 Linux 开发机的局域网地址，例如：

    http://192.168.30.13:8080/

本机网卡地址可能不同，可用 ip -br addr 查看。开发服务绑定所有接口且没有认证，只应在可信的开发网络中使用；看完后在启动终端按 Ctrl-C 停止。若只想本机监听，可设置 UHF_WEB_LISTEN=127.0.0.1:8080。

该开发服务不读取 485、不连接设备，也不会修改配置文件；当前健康接口会返回 degraded，并标记 web_auth=pending。

## 发布材料

源码发布必须保留与版本对应的 Git commit/tag、LICENSE 和 third_party/manifest.json。新增第三方依赖前，先记录固定版本、来源、SHA-256、许可证和启用特性，再进入产品构建。
