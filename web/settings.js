(function () {
    "use strict";

    var form = document.getElementById("settings-form");
    var error = document.getElementById("settings-error");
    var saved = document.getElementById("settings-saved");
    var version = 0;
    var csrfToken = "";
    var modbusTcpBind = "192.168.3.230";
    var numericFields = [
        "acquisition_slave_id", "acquisition_period_ms", "acquisition_response_timeout_ms",
        "acquisition_max_retries", "rtu_unit_id", "modbus_tcp_port", "modbus_tcp_unit_id",
        "iec_port", "storage_period_seconds", "storage_retention_days", "storage_min_free_bytes",
        "storage_event_threshold_dbm", "storage_event_rearm_dbm", "storage_event_delta_db",
        "storage_event_merge_seconds",
        "web_port"
    ];

    function redirectToLogin() { window.location.replace("/login"); }
    function showError(message) { error.textContent = message; saved.textContent = ""; }
    function showSaved(message) { error.textContent = ""; saved.textContent = message; }
    function responseMessage(response, fallback) {
        return response.json().then(function (payload) { return payload && payload.error ? payload.error : fallback; }).catch(function () { return fallback; });
    }
    function setValue(name, value) {
        var input = form.elements[name];
        if (input.type === "checkbox") { input.checked = Boolean(value); } else { input.value = String(value); }
    }
    function loadSession() {
        return fetch("/api/v1/session", {credentials: "same-origin"}).then(function (response) {
            if (response.status === 401) { redirectToLogin(); throw new Error("authentication required"); }
            if (!response.ok) { throw new Error("session failed"); }
            return response.json();
        }).then(function (session) { csrfToken = session.csrf_token; });
    }
    function loadConfig() {
        return fetch("/api/v1/config", {credentials: "same-origin"}).then(function (response) {
            if (response.status === 401) { redirectToLogin(); throw new Error("authentication required"); }
            if (!response.ok) { throw new Error("config failed"); }
            return response.json();
        }).then(function (config) {
            version = Number(config.version);
            modbusTcpBind = String(config.modbus_tcp_bind);
            document.getElementById("config-version").innerHTML = "<i></i>配置版本 " + version;
            numericFields.forEach(function (name) { setValue(name, config[name]); });
            setValue("iec_enabled", config.iec_enabled);
            setValue("iec_ied_name", config.iec_ied_name);
            document.getElementById("runtime-status").textContent = "配置已加载 · 版本 " + version;
        });
    }
    function load() { error.textContent = ""; return loadSession().then(loadConfig).catch(function (reason) { if (reason.message !== "authentication required") { showError("无法读取配置，请稍后重试。"); } }); }
    form.addEventListener("submit", function (event) {
        event.preventDefault();
        var payload = {};
        numericFields.forEach(function (name) { payload[name] = Number(form.elements[name].value); });
        payload.iec_enabled = form.elements.iec_enabled.checked;
        payload.iec_ied_name = form.elements.iec_ied_name.value;
        payload.acquisition_device = "/dev/ttyS1";
        payload.rtu_device = "/dev/ttyS4";
        payload.modbus_tcp_bind = modbusTcpBind;
        payload.tls_enabled = true;
        fetch("/api/v1/config", {method: "PUT", credentials: "same-origin", headers: {"Content-Type": "application/json", "X-CSRF-Token": csrfToken, "If-Match": '"' + version + '"'}, body: JSON.stringify(payload)}).then(function (response) {
            if (response.status === 401) { redirectToLogin(); return null; }
            if (!response.ok) { return responseMessage(response, "配置保存失败").then(showError); }
            return response.json().then(function (result) { version = Number(result.version); document.getElementById("config-version").innerHTML = "<i></i>配置版本 " + version; showSaved("已原子保存；采集和存储参数将由后台热加载，其余需要重启的项目将在下次服务重启时应用。"); });
        }).catch(function () { showError("无法连接服务，配置未确认保存。"); });
    });
    document.getElementById("reload-settings").addEventListener("click", load);
    document.querySelectorAll('[data-action="logout"]').forEach(function (button) { button.addEventListener("click", function () { fetch("/api/v1/session", {method: "DELETE", credentials: "same-origin", headers: {"X-CSRF-Token": csrfToken}}).then(redirectToLogin).catch(redirectToLogin); }); });
    load();
}());
