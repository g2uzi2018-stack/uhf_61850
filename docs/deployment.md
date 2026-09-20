# 部署简明流程

本文只记录发布包到目标板的最小安全流程。目标板不参与编译；不要把 SSH 私钥、初始密码、真实设备日志或配置备份提交到仓库。

## 1. 本地验证与打包

在仓库根目录运行完整本地验证：

```sh
bash tools/build-local.sh
```

用 AArch64 构建目录生成发布包：

```sh
bash packaging/build-release.sh \
  --build-dir build/aarch64 \
  --version VERSION \
  --output dist
sha256sum dist/uhf-gateway-VERSION.tar.gz
```

`VERSION` 只能包含字母、数字、点、下划线和短横线。发布包内的 `RELEASE` 文件会记录对应源码提交。

## 2. 上传与预检

先上传到目标板临时目录，再校验 SHA-256：

```sh
scp -i KEY -P PORT dist/uhf-gateway-VERSION.tar.gz root@HOST:/tmp/
ssh -i KEY -p PORT root@HOST '
set -eu
cd /tmp
sha256sum uhf-gateway-VERSION.tar.gz
rm -rf uhf-deploy-VERSION
mkdir uhf-deploy-VERSION
tar -xzf uhf-gateway-VERSION.tar.gz -C uhf-deploy-VERSION
bash /tmp/uhf-deploy-VERSION/uhf-gateway-VERSION/preflight.sh \
  --release /tmp/uhf-deploy-VERSION/uhf-gateway-VERSION \
  --allow-current-product
'
```

升级已有产品时，预检看到当前 `uhf-gatewayd` 占用 TCP `102`、`502`、`8080` 是正常的，必须由 `--allow-current-product` 放行。若端口由其他进程占用，不要绕过预检。

## 3. 准备 v3 制造配置

随包 systemd 单元只启动 `--v3`，不会静默回退到旧 PD1000。首次安装前必须在目标板外准备三个文件；不要把它们放进源码或发布包：

- `v3-runtime.env`：从包内 `config/v3-runtime.env.example` 复制，填入已核实的局放、电流和温度 tty 绝对路径。安装器拒绝仍含 `REPLACE_` 的模板、额外环境变量和含空格/控制字符的路径。
- `v3-device-id`：由制造流程确认的单行设备标识。
- `v3-manufacturer-key`：单行厂家密钥。不得使用仓库测试值。

三个输入文件在交给安装器前应设为 `0600`。安装器将它们复制到 `/etc/uhf-gateway`，正式系统上设为 `root:uhfgateway 0640`，升级时若未传新文件则保留原内容。它还会从三个已校验的 tty 路径生成精确的 `DeviceAllow` systemd drop-in。设备标识来源、真实 tty、RS485 方向控制、厂家密钥和激活码金样本没有确认时，不执行真实安装。

## 4. 安装

预检通过后执行包内安装脚本：

```sh
ssh -i KEY -p PORT root@HOST '
set -eu
release=/tmp/uhf-deploy-VERSION/uhf-gateway-VERSION
bash "$release/install.sh" --package "$release" \
  --v3-runtime-env-file /root/provision/v3-runtime.env \
  --v3-device-id-file /root/provision/v3-device-id \
  --v3-manufacturer-key-file /root/provision/v3-manufacturer-key
'
```

安装脚本会把 release 放到 `/opt/uhf-gateway/releases/VERSION`，原子更新 `current`，记录 `previous`，停用旧的 release guard 和 legacy recovery，并重启 `uhf-privileged.service` 与 `uhf-gateway.service`。脚本会校验两个服务的实际进程都来自本次 release；若仍跑旧二进制，安装会失败。不要手动停止或重启 `frpc.service`、`4g_server`、`sysrst` 或 watchdog。

注意：`uhf-privileged.service` 启动时会根据 `/etc/uhf-gateway/network.json` 应用持久网络配置。现场网络未确认前，不要为了刷新网页而单独重启该服务。

缺少任一制造文件时，首次正式安装会在复制 release、切换 `current` 或重启服务之前明确失败。升级已配置的设备时可省略三个选项，安装器会校验并保留 `/etc/uhf-gateway` 中的现有文件。

首次启动且尚无有效绑定状态时，服务只开放激活页面，不启动三路采集、Modbus、IEC 61850、FTP 或其他 Web API。浏览器核对页面显示的设备标识，输入厂家提供的激活码；持久化并复核成功后，同一进程才启动完整服务。激活码不再作为 systemd 命令行参数或 `/etc/uhf-gateway` 制造文件保存。开发/制造自动化仍可显式使用守护进程的 `--v3-activation-code-file` 兼容入口，但正式 systemd 单元不会使用它。

## 5. 验收检查

安装后检查 release、服务、健康状态和受保护进程：

```sh
ssh -i KEY -p PORT root@HOST '
set -u
readlink -f /opt/uhf-gateway/current
cat /opt/uhf-gateway/current/RELEASE
cat /var/lib/uhf-gateway/release-state.json
systemctl is-active uhf-gateway.service uhf-privileged.service frpc.service
ps -eo pid=,user=,args= | grep -E "[/]frpc|(^|[[:space:]])4g_server([[:space:]]|$)" || true
ss -H -ltnp | grep -E "0\.0\.0\.0:(102|502|8080)[[:space:]]" || true
curl -kfsS --connect-timeout 5 https://127.0.0.1:8080/healthz
'
```

`release-state.json` 中 `pending` 保持为空；当前发布流程不再启用 release guard 自动回滚。接入采集链路的正式验收环境中，健康接口应返回 `acquisition`、`storage`、`modbus_tcp`、`modbus_rtu` 和 `iec61850` 为 `up`。

## 6. 清理临时文件

验收通过后删除目标板临时上传目录和归档：

```sh
ssh -i KEY -p PORT root@HOST '
rm -rf /tmp/uhf-deploy-VERSION /tmp/uhf-gateway-VERSION.tar.gz
'
```

本地 `build/` 和 `dist/` 都是生成物，不提交到 Git。需要保留可追溯发布材料时，记录版本号、源码提交和归档 SHA-256。
