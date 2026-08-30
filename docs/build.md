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

## 发布材料

源码发布必须保留与版本对应的 Git commit/tag、LICENSE 和 third_party/manifest.json。新增第三方依赖前，先记录固定版本、来源、SHA-256、许可证和启用特性，再进入产品构建。
