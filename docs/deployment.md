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

## 3. 安装

预检通过后执行包内安装脚本：

```sh
ssh -i KEY -p PORT root@HOST '
set -eu
release=/tmp/uhf-deploy-VERSION/uhf-gateway-VERSION
bash "$release/install.sh" --package "$release"
'
```

安装脚本会把 release 放到 `/opt/uhf-gateway/releases/VERSION`，原子更新 `current`，记录 `previous`，停用旧的 release guard，并重启 `uhf-gateway.service`。不要手动停止或重启 `frpc.service`、`4g_server`、`sysrst` 或 watchdog。

注意：`uhf-privileged.service` 启动时会根据 `/etc/uhf-gateway/network.json` 应用持久网络配置。现场网络未确认前，不要为了刷新网页而单独重启该服务。

## 4. 验收检查

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

## 5. 清理临时文件

验收通过后删除目标板临时上传目录和归档：

```sh
ssh -i KEY -p PORT root@HOST '
rm -rf /tmp/uhf-deploy-VERSION /tmp/uhf-gateway-VERSION.tar.gz
'
```

本地 `build/` 和 `dist/` 都是生成物，不提交到 Git。需要保留可追溯发布材料时，记录版本号、源码提交和归档 SHA-256。
