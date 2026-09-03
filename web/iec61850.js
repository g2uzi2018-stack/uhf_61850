(function () {
    "use strict";
    var model = [];
    function redirect() { window.location.replace("/login"); }
    function api(path) { return fetch(path, {credentials: "same-origin"}).then(function (response) { if (response.status === 401) { redirect(); throw new Error("auth"); } if (!response.ok) { throw new Error("request failed"); } return response.json(); }); }
    function toast(message) { var element = document.getElementById("toast"); element.textContent = message; element.classList.add("visible"); window.setTimeout(function () { element.classList.remove("visible"); }, 3500); }
    function statusText(status) { return status === "up" ? "正常" : status === "disabled" ? "未启用" : status === "degraded" ? "降级" : "离线"; }
    function statusClass(status) { return status === "up" ? "up" : status === "disabled" ? "disabled" : status === "degraded" ? "degraded" : "down"; }
    function setText(id, value) { var element = document.getElementById(id); if (element) { element.textContent = value == null ? "--" : String(value); } }
    function valueFor(reference, snapshot) {
        if (reference.indexOf("PaDschAlm") >= 0) { return "由事件状态机更新"; }
        var name = reference.indexOf("AnIn1") >= 0 ? "average" : reference.indexOf("IntIn1") >= 0 ? "frequency" : reference.indexOf("AnIn2") >= 0 || reference.indexOf("UhfPaDsch") >= 0 ? "peak" : reference.indexOf("AnIn3") >= 0 ? "phase" : "noise";
        var measurement = (snapshot.measurements || []).filter(function (item) { return item.name === name; })[0];
        return measurement && measurement.valid ? measurement.value : "--";
    }
    function render(payload, snapshot) {
        var status = payload.status || "down";
        var dot = document.getElementById("iec-status-dot"); dot.className = "status-dot " + statusClass(status);
        setText("iec-status", "IEC · " + statusText(status));
        var badge = document.getElementById("iec-badge"); badge.className = "heading-badge " + (status === "up" ? "" : "dev-badge"); badge.innerHTML = "<i></i>" + statusText(status);
        setText("iec-enabled", payload.enabled ? "已启用" : "未启用"); setText("ied-name", payload.ied_name); setText("iec-listen", (payload.bind_address || "--") + ":" + (payload.port || "--")); setText("iec-service-status", statusText(status));
        setText("iec-active-connections", payload.active_connections);
        setText("iec-connection-rejections", payload.counters && payload.counters.connection_rejections);
        setText("iec-malformed-pdu-rejections", payload.counters && payload.counters.malformed_pdu_rejections);
        setText("iec-oversized-pdu-rejections", payload.counters && payload.counters.oversized_pdu_rejections);
        setText("iec-request-element-rejections", payload.counters && payload.counters.request_element_rejections);
        setText("iec-ber-depth-rejections", payload.counters && payload.counters.ber_depth_rejections);
        setText("iec-outstanding-rejections", payload.counters && payload.counters.max_outstanding_rejections);
        setText("iec-report-buffer-overflows", payload.counters && payload.counters.report_buffer_overflows);
        setText("dataset-reference", payload.dataset && payload.dataset.reference); setText("report-reference", payload.report && payload.report.reference); setText("integrity-period", payload.report ? payload.report.integrity_seconds + " 秒" : "--"); setText("report-kind", payload.report && payload.report.buffered ? "BRCB" : "URCB");
        var snapshotState = document.getElementById("snapshot-state"); snapshotState.textContent = snapshot && snapshot.generation ? "generation " + snapshot.generation : "尚无快照";
        var body = document.getElementById("iec-model-body"); body.textContent = ""; model = payload.model || [];
        if (!model.length) { var empty = document.createElement("tr"); var cell = document.createElement("td"); cell.colSpan = 6; cell.className = "muted-cell"; cell.textContent = "模型不可用"; empty.appendChild(cell); body.appendChild(empty); return; }
        model.forEach(function (item) { var row = document.createElement("tr"); [item.reference, item.type, item.source_register ? "寄存器 " + item.source_register : "事件状态", item.unit, valueFor(item.reference, snapshot || {}), item.reference.indexOf("PaDschAlm") >= 0 ? "事件状态" : ((snapshot || {}).payload_status || "待采集")].forEach(function (value) { var cell = document.createElement("td"); cell.textContent = value == null ? "--" : String(value); row.appendChild(cell); }); body.appendChild(row); });
    }
    function renderIcd(payload) {
        var available = payload && payload.available;
        setText("icd-source", !available ? "未找到" : payload.override_active ? "网页上传覆盖" : "系统基线");
        setText("icd-size", available ? payload.size + " / " + payload.max_bytes + " 字节" : "--");
        setText("icd-sha256", available ? payload.sha256 : "--");
        var runtimeIed = payload.runtime_model_ied_name || payload.runtime_ied_name;
        var ied = available ? (payload.ied_name || "未知") : "--";
        if (available && payload.ied_name && runtimeIed && payload.ied_name !== runtimeIed) { ied += "（尚未应用）"; }
        setText("icd-ied-name", ied); setText("icd-runtime-ied-name", runtimeIed || "--");
        if (available && payload.applied_to_runtime) { setText("icd-source", payload.override_active ? "网页上传覆盖（已应用到实时 MMS）" : "系统基线（已应用到实时 MMS）"); }
        setText("icd-previous", payload.previous_available ? "可恢复" : "无上一版本");
        var previousDownload = document.getElementById("icd-previous-download"); var restore = document.getElementById("icd-restore");
        if (previousDownload) { previousDownload.hidden = !payload.previous_available; }
        if (restore) { restore.hidden = !payload.override_active; }
    }
    function loadIcd() { api("/api/v1/iec61850/icd").then(renderIcd).catch(function (error) { if (error.message !== "auth") { setText("icd-source", "读取失败"); } }); }
    function load() { Promise.all([api("/api/v1/iec61850"), api("/api/v1/snapshot/latest").catch(function () { return {}; })]).then(function (values) { render(values[0], values[1]); }).catch(function (error) { if (error.message !== "auth") { setText("iec-status", "IEC 状态暂时不可用"); toast("IEC 状态读取失败"); } }); loadIcd(); }
    var icdForm = document.getElementById("icd-form");
    if (icdForm) { icdForm.addEventListener("submit", function (event) { event.preventDefault(); var fileInput = document.getElementById("icd-file"); var passwordInput = document.getElementById("icd-password"); var file = fileInput.files && fileInput.files[0]; if (!file) { toast("请选择 ICD 文件"); return; } if (file.size > 48 * 1024) { toast("ICD 文件超过 48 KiB 限制"); return; } Promise.all([file.text(), api("/api/v1/session")]).then(function (values) { return fetch("/api/v1/iec61850/icd", {method: "PUT", credentials: "same-origin", headers: {"Content-Type": "application/json", "X-CSRF-Token": values[1].csrf_token}, body: JSON.stringify({current_password: passwordInput.value, content: values[0]})}); }).then(function (response) { return response.json().then(function (body) { if (response.status === 401) { redirect(); throw new Error("auth"); } if (!response.ok) { throw new Error(body.error || "ICD 替换失败"); } return body; }); }).then(function (body) { passwordInput.value = ""; fileInput.value = ""; toast(body.runtime_reloaded ? "ICD 已替换，实时 MMS 模型已重建；请让客户端重连" : "ICD 已校验并原子替换"); load(); }).catch(function (error) { if (error.message !== "auth") { toast(error.message || "ICD 替换失败"); } }); }); }
    var restoreButton = document.getElementById("icd-restore");
    if (restoreButton) { restoreButton.addEventListener("click", function () { var passwordInput = document.getElementById("icd-password"); if (!passwordInput.value) { toast("请输入当前管理员密码"); return; } Promise.all([api("/api/v1/session")]).then(function (values) { return fetch("/api/v1/iec61850/icd/restore", {method: "POST", credentials: "same-origin", headers: {"Content-Type": "application/json", "X-CSRF-Token": values[0].csrf_token}, body: JSON.stringify({current_password: passwordInput.value})}); }).then(function (response) { return response.json().then(function (body) { if (response.status === 401) { redirect(); throw new Error("auth"); } if (!response.ok) { throw new Error(body.error || "恢复失败"); } return body; }); }).then(function (body) { passwordInput.value = ""; toast(body.runtime_reloaded ? "已恢复 ICD，实时 MMS 模型已重建；请让客户端重连" : "已恢复系统基线 ICD"); load(); }).catch(function (error) { if (error.message !== "auth") { toast(error.message || "恢复失败"); } }); }); }
    document.querySelectorAll('[data-action="logout"]').forEach(function (button) { button.addEventListener("click", function () { api("/api/v1/session").then(function (session) { return fetch("/api/v1/session", {method: "DELETE", credentials: "same-origin", headers: {"X-CSRF-Token": session.csrf_token}}); }).then(redirect).catch(redirect); }); });
    load(); window.setInterval(load, 6000);
}());
