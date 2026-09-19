# 本地构建

当前仓库已经提供 host 和 AArch64 两个 CMake 配置。目标板不参与编译，也不需要在启动时下载依赖；主机上可以完整运行协议模拟器、认证 Web 服务和发布包冒烟测试。

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

预览页面由 `uhf-gatewayd` 的 C++ 服务直接提供，不是静态页面截图或 Python 假服务。登录、服务端会话、CSRF、可选改密、登出、总览、实时 WebSocket、配置、网络事务、IEC 61850 状态、日志、存储和维护页面均走实际路由。启动开发服务：

    bash tools/run-web-local.sh

在 Windows 浏览器打开 Linux 开发机的局域网地址，例如：

    http://192.168.30.13:8080/

本机网卡地址可能不同，可用 ip -br addr 查看。开发服务绑定所有接口，页面和 API 使用 HTTP 开发模式，只应在可信的开发网络中使用；看完后在启动终端按 Ctrl-C 停止。若只想本机监听，可设置 UHF_WEB_LISTEN=127.0.0.1:8080。

首次启动会在 `build/dev-state/initial-password` 写入一次性默认密码 `admin`（文件权限
0600），登录账号为 `admin`，管理员可在用户管理页主动修改密码；改密成功
会删除该一次性密码文件。运行时哈希保存在 `build/dev-state/auth.json`（权限
0600），开发服务停止后可删除整个 `build/` 目录重新初始化。正式发布安装由
`uhf-auth-init` 预置认证状态，并将一次性密码保留为 root-only 文件。

该交互式开发服务仍用于旧 PD1000 兼容页面。正式 v3 主程序的三 PTY 自动模拟入口为：

    ctest --test-dir build/host --output-on-failure -R '^uhf_v3_gateway_e2e$'

该测试实际启动 `uhf-gatewayd --v3`，通过受保护临时文件提供测试身份/密钥/激活码，并贯通三路 PTY、采集调度、HTTP/WebSocket、配置热加载和 Modbus/TCP；它不会读取本机 485，也不会修改产品 `/etc` 配置。`tools/run-web-local.sh` 使用 PD1000 PTY 模拟器；
`--http-recovery` 是仅供可信本地开发网络使用的明文恢复模式。产品服务默认启用
HTTPS，启动时生成自签名证书，证书替换、业务配置、网络事务、Modbus、IEC 61850
和持久化均可在 host 测试中验证；真实串口、电气收发、双网口链路和板级厂商网络
文件格式仍需目标板阶段验收。

`tools/preview-web.sh` 现在只是上述 C++ 开发服务的兼容入口，不再启动无认证的
Python 静态服务器。

## 发布包

执行 `bash tools/build-local.sh` 会先运行完整 host CTest，再构建并检查 AArch64
产物。配置完成的 host 构建也可以用以下命令生成离线发布包：

    bash packaging/build-release.sh --build-dir build/host --version VERSION --output dist

发布包包含二进制、离线 Web 资源、默认配置/schema、v3 tty 配置模板、IEC 模型、许可证清单、systemd、
rsyslog、logrotate、DHCP hook、认证初始化工具、预检脚本和原子安装脚本。安装到
真实目标板前必须通过目标架构、端口、串口、磁盘、NTP 及受保护旧进程检查；当前远程
机器关闭时只进行上述本地构建和模拟器验证。

## PD1000 主机模拟器

没有 485 接线时，可以用 Linux PTY 验证后续采集代码的正常 Modbus 路径：

    python3 tools/pd1000-sim/pd1000_sim.py \
        --device /dev/pts/XX \
        --fixture tests/fixtures/pd1000_request_plan.txt

通常直接运行 `ctest --test-dir build/host --output-on-failure` 即可执行自动 PTY
测试；模拟器只用于研发机，不安装到目标板。

## 发布材料

源码发布必须保留与版本对应的 Git commit/tag、LICENSE 和 third_party/manifest.json。新增第三方依赖前，先记录固定版本、来源、SHA-256、许可证和启用特性，再进入产品构建。
