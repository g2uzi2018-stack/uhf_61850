(function () {
    "use strict";
    var model = [];
    function redirect() { window.location.replace("/login"); }
    function api(path) { return fetch(path, {credentials: "same-origin"}).then(function (response) { if (response.status === 401) { redirect(); throw new Error("auth"); } if (!response.ok) { throw new Error("request failed"); } return response.json(); }); }
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
        setText("iec-connection-rejections", payload.counters && payload.counters.connection_rejections);
        setText("iec-malformed-pdu-rejections", payload.counters && payload.counters.malformed_pdu_rejections);
        setText("iec-oversized-pdu-rejections", payload.counters && payload.counters.oversized_pdu_rejections);
        setText("iec-outstanding-rejections", payload.counters && payload.counters.max_outstanding_rejections);
        setText("dataset-reference", payload.dataset && payload.dataset.reference); setText("report-reference", payload.report && payload.report.reference); setText("integrity-period", payload.report ? payload.report.integrity_seconds + " 秒" : "--");
        var snapshotState = document.getElementById("snapshot-state"); snapshotState.textContent = snapshot && snapshot.generation ? "generation " + snapshot.generation : "尚无快照";
        var body = document.getElementById("iec-model-body"); body.textContent = ""; model = payload.model || [];
        if (!model.length) { var empty = document.createElement("tr"); var cell = document.createElement("td"); cell.colSpan = 6; cell.className = "muted-cell"; cell.textContent = "模型不可用"; empty.appendChild(cell); body.appendChild(empty); return; }
        model.forEach(function (item) { var row = document.createElement("tr"); [item.reference, item.type, item.source_register ? "寄存器 " + item.source_register : "事件状态", item.unit, valueFor(item.reference, snapshot || {}), item.reference.indexOf("PaDschAlm") >= 0 ? "事件状态" : ((snapshot || {}).payload_status || "待采集")].forEach(function (value) { var cell = document.createElement("td"); cell.textContent = value == null ? "--" : String(value); row.appendChild(cell); }); body.appendChild(row); });
    }
    function load() { Promise.all([api("/api/v1/iec61850"), api("/api/v1/snapshot/latest").catch(function () { return {}; })]).then(function (values) { render(values[0], values[1]); }).catch(function (error) { if (error.message !== "auth") { setText("iec-status", "IEC 状态暂时不可用"); document.getElementById("toast").textContent = "IEC 状态读取失败"; document.getElementById("toast").classList.add("visible"); } }); }
    document.querySelectorAll('[data-action="logout"]').forEach(function (button) { button.addEventListener("click", function () { api("/api/v1/session").then(function (session) { return fetch("/api/v1/session", {method: "DELETE", credentials: "same-origin", headers: {"X-CSRF-Token": session.csrf_token}}); }).then(redirect).catch(redirect); }); });
    load(); window.setInterval(load, 6000);
}());
