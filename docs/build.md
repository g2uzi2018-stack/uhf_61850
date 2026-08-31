# 本地构建

当前仓库已经提供 host 和 AArch64 两个 CMake 配置。目标板不参与编译，也不需要在启动时下载依赖。

## 依赖

- CMake 3.22 或更新版本
- 支持 C++17 的 host 编译器和 Make
- OpenSSL 3 开发文件（host 构建使用 `libssl-dev`）
- AArch64 构建需要 aarch64-linux-gnu-g++、对应的 sysroot 和 arm64 `libssl-dev`

在 Debian/Ubuntu 开发机上可安装交叉工具链：

    sudo apt-get install gcc-aarch64-linux-gnu g++-aarch64-linux-gnu libssl-dev

交叉 sysroot 还必须提供 `/usr/aarch64-linux-gnu/include/openssl` 和
`/usr/aarch64-linux-gnu/lib/libcrypto.so`。目标板/发行版提供的 arm64 OpenSSL 3
开发包应安装到该 sysroot；CMake 会固定从 `/usr/aarch64-linux-gnu` 查找，避免误链
host 库。

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

用户管理页面现在由 uhf-gatewayd 的 C++ 开发 HTTP 服务提供，登录、服务端会话、CSRF、首次改密和登出接口已经接入。启动开发服务：

    bash tools/run-web-local.sh

在 Windows 浏览器打开 Linux 开发机的局域网地址，例如：

    http://192.168.30.13:8080/

本机网卡地址可能不同，可用 ip -br addr 查看。开发服务绑定所有接口，页面和 API 使用 HTTP 开发模式，只应在可信的开发网络中使用；看完后在启动终端按 Ctrl-C 停止。若只想本机监听，可设置 UHF_WEB_LISTEN=127.0.0.1:8080。

首次启动会在 `build/dev-state/initial-password` 生成一次性随机密码（文件权限
0600），登录账号为 `admin`。浏览器打开登录页后，首次登录必须修改密码；改密成功
会删除该一次性密码文件。运行时哈希保存在 `build/dev-state/auth.json`（权限
0600），开发服务停止后可删除整个 `build/` 目录重新初始化。

该开发服务不读取 485、不连接设备，也不会修改产品配置文件；当前健康接口会返回
`degraded`（硬件采集尚未实现）和 `web_auth=ready`。HTTPS、证书替换、业务配置和
硬件协议仍属于后续增量，不能把当前开发服务当作产品发布包。

`tools/preview-web.sh` 现在只是上述 C++ 开发服务的兼容入口，不再启动无认证的
Python 静态服务器。

## PD1000 主机模拟器

没有 485 接线时，可以用 Linux PTY 验证后续采集代码的正常 Modbus 路径：

    python3 tools/pd1000-sim/pd1000_sim.py \
        --device /dev/pts/XX \
        --fixture tests/fixtures/pd1000_request_plan.txt

通常直接运行 `ctest --test-dir build/host --output-on-failure` 即可执行自动 PTY
测试；模拟器只用于研发机，不安装到目标板。

## 发布材料

源码发布必须保留与版本对应的 Git commit/tag、LICENSE 和 third_party/manifest.json。新增第三方依赖前，先记录固定版本、来源、SHA-256、许可证和启用特性，再进入产品构建。
