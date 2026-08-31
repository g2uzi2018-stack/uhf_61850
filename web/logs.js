(function () {
    "use strict";
    var entries = [];
    var logsBody = document.getElementById("logs-body");
    var levelFilter = document.getElementById("level-filter");
    var componentFilter = document.getElementById("component-filter");
    function redirect() { window.location.replace("/login"); }
    function api(path) { return fetch(path, {credentials: "same-origin"}).then(function (response) { if (response.status === 401) { redirect(); throw new Error("auth"); } if (!response.ok) { throw new Error("request failed"); } return response.json(); }); }
    function formatTime(milliseconds) { var date = new Date(milliseconds); return isNaN(date.getTime()) ? "--" : date.toLocaleString(); }
    function renderLogs() {
        logsBody.textContent = "";
        var filtered = entries.filter(function (entry) { return (!levelFilter.value || entry.level === levelFilter.value) && (!componentFilter.value || entry.component === componentFilter.value); });
        if (!filtered.length) { var empty = document.createElement("tr"); var cell = document.createElement("td"); cell.colSpan = 5; cell.className = "muted-cell"; cell.textContent = "没有匹配日志"; empty.appendChild(cell); logsBody.appendChild(empty); return; }
        filtered.slice().reverse().forEach(function (entry) {
            var row = document.createElement("tr"); [formatTime(entry.timestamp_ms), entry.level || "--", entry.component || "--", entry.event || "--", entry.message || "--"].forEach(function (value, index) { var cell = document.createElement("td"); cell.textContent = value; if (index === 1) { cell.className = "log-level " + String(value).toLowerCase(); } row.appendChild(cell); }); logsBody.appendChild(row);
        });
    }
    function loadLogs() { return api("/api/v1/logs").then(function (payload) { entries = Array.isArray(payload.entries) ? payload.entries : []; document.getElementById("log-state").innerHTML = "<i></i>" + entries.length + " 条日志"; renderLogs(); }); }
    function loadStorage(path, target, emptyText) { return api(path).then(function (payload) { target.textContent = ""; var list = Array.isArray(payload.entries) ? payload.entries : []; if (!list.length) { var empty = document.createElement("span"); empty.className = "muted-cell"; empty.textContent = emptyText; target.appendChild(empty); return; } list.slice(0, 8).forEach(function (item) { var row = document.createElement("div"); row.className = "storage-row"; var strong = document.createElement("strong"); strong.textContent = item.name || ("generation " + item.generation); var detail = document.createElement("span"); detail.textContent = item.generation ? "generation " + item.generation + " · " + (item.payload_status || "") : "事件 #" + item.id + " · " + (item.frame_count || 0) + " 帧"; row.appendChild(strong); row.appendChild(detail); target.appendChild(row); }); }); }
    function load() { return Promise.all([loadLogs(), loadStorage("/api/v1/frames", document.getElementById("frames-list"), "尚无周期帧"), loadStorage("/api/v1/events", document.getElementById("events-list"), "尚无事件包")]).catch(function (error) { if (error.message !== "auth") { document.getElementById("log-state").textContent = "读取失败"; } }); }
    function csvEscape(value) { var text = String(value == null ? "" : value); return /[",\n]/.test(text) ? '"' + text.replace(/"/g, '""') + '"' : text; }
    document.getElementById("download-logs").addEventListener("click", function () { var rows = [["timestamp_ms", "level", "component", "event", "message"]].concat(entries.map(function (entry) { return [entry.timestamp_ms, entry.level, entry.component, entry.event, entry.message]; })); var csv = rows.map(function (row) { return row.map(csvEscape).join(","); }).join("\n") + "\n"; var link = document.createElement("a"); link.href = URL.createObjectURL(new Blob([csv], {type: "text/csv"})); link.download = "uhf-gateway-logs.csv"; link.click(); URL.revokeObjectURL(link.href); });
    [levelFilter, componentFilter].forEach(function (element) { element.addEventListener("change", renderLogs); }); document.getElementById("refresh-logs").addEventListener("click", load); document.querySelectorAll('[data-action="logout"]').forEach(function (button) { button.addEventListener("click", function () { fetch("/api/v1/session", {method: "GET", credentials: "same-origin"}).then(function (response) { return response.json(); }).then(function (session) { return fetch("/api/v1/session", {method: "DELETE", credentials: "same-origin", headers: {"X-CSRF-Token": session.csrf_token}}); }).then(redirect).catch(redirect); }); }); load();
}());
