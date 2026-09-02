(function () {
    "use strict";

    var form = document.getElementById("network-form");
    var error = document.getElementById("network-error");
    var saved = document.getElementById("network-saved");
    var state = document.getElementById("network-state");
    var transactionState = document.getElementById("transaction-state");
    var countdown = document.getElementById("countdown");
    var confirmButton = document.getElementById("confirm-network");
    var timeError = document.getElementById("time-error");
    var timeSaved = document.getElementById("time-saved");
    var timeState = document.getElementById("time-state");
    var csrfToken = "";
    var timer = null;
    var configuration = null;
    var configurationVersion = 0;

    function redirect() { window.location.replace("/login"); }
    function showError(message) { error.textContent = message; saved.textContent = ""; }
    function showSaved(message) { error.textContent = ""; saved.textContent = message; }
    function showTimeError(message) { timeError.textContent = message; timeSaved.textContent = ""; }
    function showTimeSaved(message) { timeError.textContent = ""; timeSaved.textContent = message; }
    function api(path, options) {
        options = options || {};
        options.credentials = "same-origin";
        return fetch(path, options).then(function (response) {
            if (response.status === 401) { redirect(); throw new Error("auth"); }
            return response.json().then(function (payload) {
                if (!response.ok) { throw new Error(payload.error || "request failed"); }
                return payload;
            });
        });
    }
    function setValue(name, value) {
        var input = form.elements[name];
        if (!input) { return; }
        if (input.type === "checkbox") { input.checked = Boolean(value); } else { input.value = value == null ? "" : String(value); }
    }
    function setInterface(prefix, config) {
        ["mode", "address", "netmask", "gateway", "hostname", "dhcp_timeout_seconds"].forEach(function (suffix) {
            setValue(prefix + "_" + suffix, suffix === "netmask" ? config.netmask : config[suffix]);
        });
        setValue(prefix + "_dns1", config.dns && config.dns[0]);
        setValue(prefix + "_dns2", config.dns && config.dns[1]);
        toggleInterface(prefix);
    }
    function toggleInterface(prefix) {
        var dhcp = form.elements[prefix + "_mode"].value === "dhcp";
        ["address", "netmask", "gateway", "dns1", "dns2"].forEach(function (suffix) {
            form.elements[prefix + "_" + suffix].disabled = dhcp;
        });
    }
    function renderTransaction(transaction) {
        if (timer) { window.clearInterval(timer); timer = null; }
        if (!transaction || transaction.state !== "staged") {
            transactionState.innerHTML = "<i></i>无待确认事务";
            countdown.innerHTML = "<i></i>未开始";
            confirmButton.disabled = true;
            return;
        }
        transactionState.innerHTML = "<i></i>事务 #" + transaction.id + " 待确认";
        confirmButton.disabled = false;
        var remaining = Number(transaction.remaining_seconds || 60);
        function tick() {
            countdown.innerHTML = "<i></i>剩余 " + Math.max(0, remaining) + " 秒";
            if (remaining <= 0) {
                window.clearInterval(timer);
                confirmButton.disabled = true;
                loadNetwork();
            }
            remaining -= 1;
        }
        tick();
        timer = window.setInterval(tick, 1000);
    }
    function actualText(actual) {
        if (!actual || !actual.exists) { return "内核状态：接口不存在或未读取"; }
        return "内核状态：" + (actual.link_up ? "链路已连接" : "链路未连接") + (actual.address ? " · " + actual.address + "/" + actual.prefix : " · 无 IPv4");
    }
    function loadNetwork() {
        return api("/api/v1/network").then(function (payload) {
            state.textContent = "网络 helper 已连接";
            setInterface("eth0", payload.config.eth0);
            setInterface("eth1", payload.config.eth1);
            document.getElementById("eth0-actual").textContent = actualText(payload.actual && payload.actual.eth0) + " · 配置 " + payload.config.eth0.mode;
            document.getElementById("eth1-actual").textContent = actualText(payload.actual && payload.actual.eth1) + " · 配置 " + payload.config.eth1.mode;
            renderTransaction(payload.transaction);
        }).catch(function (reason) {
            if (reason.message !== "auth") { state.textContent = "网络 helper 不可用"; showError("无法读取网络状态：" + reason.message); }
        });
    }
    function loadConfiguration() {
        return api("/api/v1/config").then(function (config) {
            configuration = config;
            configurationVersion = Number(config.version);
            setValue("iec_ied_name", config.iec_ied_name);
            setValue("time_sync_enabled", config.time_sync_enabled);
            setValue("sntp_server", config.sntp_server);
            timeState.textContent = config.time_sync_enabled ? "自动同步" : "手动模式";
        });
    }
    function load() {
        showError("");
        return Promise.all([loadNetwork(), loadConfiguration()]);
    }
    function candidate() {
        var result = {};
        ["eth0", "eth1"].forEach(function (prefix) {
            ["mode", "address", "netmask", "gateway", "hostname"].forEach(function (suffix) {
                result[prefix + "_" + suffix] = form.elements[prefix + "_" + suffix].value;
            });
            result[prefix + "_dns1"] = form.elements[prefix + "_dns1"].value;
            result[prefix + "_dns2"] = form.elements[prefix + "_dns2"].value;
            result[prefix + "_dhcp_timeout_seconds"] = 15;
        });
        return result;
    }
    function post(path, body) {
        return api(path, {method: "POST", headers: {"Content-Type": "application/json", "X-CSRF-Token": csrfToken}, body: JSON.stringify(body || {})});
    }
    function password() { return document.getElementById("current-password").value; }
    function saveConfiguration() {
        if (!configuration) { return Promise.reject(new Error("配置尚未读取")); }
        var payload = Object.assign({}, configuration);
        delete payload.version;
        payload.iec_ied_name = document.getElementById("iec-ied-name").value;
        payload.time_sync_enabled = document.getElementById("time-sync-enabled").checked;
        payload.sntp_server = document.getElementById("sntp-server").value;
        return api("/api/v1/config", {
            method: "PUT",
            headers: {"Content-Type": "application/json", "X-CSRF-Token": csrfToken, "If-Match": '"' + configurationVersion + '"'},
            body: JSON.stringify(payload)
        }).then(function (result) {
            configurationVersion = Number(result.version);
            payload.version = configurationVersion;
            configuration = payload;
            timeState.textContent = payload.time_sync_enabled ? "自动同步" : "手动模式";
            return result;
        });
    }
    function applyTimeAction(action) {
        var currentPassword = password();
        if (!currentPassword) { showTimeError("请输入当前密码进行再认证"); return Promise.reject(new Error("password")); }
        return post("/api/v1/time", {action: action, current_password: currentPassword});
    }
    form.addEventListener("submit", function (event) {
        event.preventDefault();
        var currentPassword = password();
        if (!currentPassword) { showError("请输入当前密码进行再认证"); return; }
        post("/api/v1/network/stage", {current_password: currentPassword, candidate: candidate()}).then(function () {
            showSaved("候选地址已试应用，请确认新地址仍可访问。");
            return loadNetwork();
        }).catch(function (reason) { if (reason.message !== "auth") { showError("试应用失败：" + reason.message); } });
    });
    confirmButton.addEventListener("click", function () {
        var currentPassword = password();
        if (!currentPassword) { showError("确认网络也需要当前密码再认证"); return; }
        post("/api/v1/network/confirm", {current_password: currentPassword}).then(function () {
            document.getElementById("current-password").value = "";
            showSaved("网络候选已确认并写入持久配置。");
            return loadNetwork();
        }).catch(function (reason) { if (reason.message !== "auth") { showError("确认失败：" + reason.message); } });
    });
    document.getElementById("rollback-network").addEventListener("click", function () {
        var currentPassword = password();
        if (!currentPassword) { showError("回滚也需要当前密码再认证"); return; }
        post("/api/v1/network/rollback", {current_password: currentPassword}).then(function () {
            showSaved("候选网络已回滚。");
            document.getElementById("current-password").value = "";
            return loadNetwork();
        }).catch(function (reason) { if (reason.message !== "auth") { showError("回滚失败：" + reason.message); } });
    });
    document.getElementById("save-device-options").addEventListener("click", function () {
        var button = this;
        var currentPassword = password();
        if (!currentPassword) { showTimeError("请输入当前密码进行再认证"); return; }
        button.disabled = true;
        saveConfiguration().then(function () {
            return applyTimeAction(document.getElementById("time-sync-enabled").checked ? "sync" : "disable");
        }).then(function () {
            showTimeSaved("设备标识与时间设置已保存并应用。");
        }).catch(function (reason) { if (reason.message !== "auth" && reason.message !== "password") { showTimeError("保存时间设置失败：" + reason.message); } }).then(function () { button.disabled = false; });
    });
    document.getElementById("sync-time").addEventListener("click", function () {
        var button = this;
        var currentPassword = password();
        if (!currentPassword) { showTimeError("请输入当前密码进行再认证"); return; }
        document.getElementById("time-sync-enabled").checked = true;
        button.disabled = true;
        saveConfiguration().then(function () { return applyTimeAction("sync"); }).then(function () {
            showTimeSaved("已请求 SNTP 同步。");
        }).catch(function (reason) { if (reason.message !== "auth" && reason.message !== "password") { showTimeError("SNTP 同步失败：" + reason.message); } }).then(function () { button.disabled = false; });
    });
    document.getElementById("set-time").addEventListener("click", function () {
        var button = this;
        var currentPassword = password();
        var localTime = document.getElementById("manual-time").value;
        if (!currentPassword) { showTimeError("请输入当前密码进行再认证"); return; }
        if (!localTime) { showTimeError("请选择设备时间"); return; }
        document.getElementById("time-sync-enabled").checked = false;
        button.disabled = true;
        saveConfiguration().then(function () { return post("/api/v1/time", {action: "set", current_password: currentPassword, local_time: localTime}); }).then(function () {
            showTimeSaved("设备时间设置请求已提交，自动同步已停用。");
        }).catch(function (reason) { if (reason.message !== "auth") { showTimeError("手动校时失败：" + reason.message); } }).then(function () { button.disabled = false; });
    });
    document.getElementById("reload-network").addEventListener("click", load);
    ["eth0", "eth1"].forEach(function (prefix) { form.elements[prefix + "_mode"].addEventListener("change", function () { toggleInterface(prefix); }); });
    document.querySelectorAll('[data-action="logout"]').forEach(function (button) {
        button.addEventListener("click", function () { fetch("/api/v1/session", {method: "DELETE", credentials: "same-origin", headers: {"X-CSRF-Token": csrfToken}}).then(redirect).catch(redirect); });
    });
    api("/api/v1/session").then(function (session) { csrfToken = session.csrf_token; return load(); }).catch(function () {});
}());
