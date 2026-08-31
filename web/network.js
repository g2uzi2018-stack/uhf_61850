(function () {
    "use strict";
    var form = document.getElementById("network-form");
    var error = document.getElementById("network-error");
    var saved = document.getElementById("network-saved");
    var state = document.getElementById("network-state");
    var transactionState = document.getElementById("transaction-state");
    var countdown = document.getElementById("countdown");
    var confirmButton = document.getElementById("confirm-network");
    var csrfToken = "";
    var timer = null;
    function redirect() { window.location.replace("/login"); }
    function showError(message) { error.textContent = message; saved.textContent = ""; }
    function showSaved(message) { error.textContent = ""; saved.textContent = message; }
    function api(path, options) { options = options || {}; options.credentials = "same-origin"; return fetch(path, options).then(function (response) { if (response.status === 401) { redirect(); throw new Error("auth"); } return response.json().then(function (payload) { if (!response.ok) { throw new Error(payload.error || "request failed"); } return payload; }); }); }
    function setValue(name, value) { form.elements[name].value = value == null ? "" : String(value); }
    function setInterface(prefix, config) { ["mode", "address", "prefix", "gateway", "hostname", "dhcp_timeout_seconds"].forEach(function (suffix) { setValue(prefix + "_" + suffix, config[suffix]); }); setValue(prefix + "_dns1", config.dns && config.dns[0]); setValue(prefix + "_dns2", config.dns && config.dns[1]); toggleInterface(prefix); }
    function toggleInterface(prefix) { var dhcp = form.elements[prefix + "_mode"].value === "dhcp"; ["address", "prefix", "gateway", "dns1", "dns2"].forEach(function (suffix) { form.elements[prefix + "_" + suffix].disabled = dhcp; }); }
    function renderTransaction(transaction) { if (timer) { window.clearInterval(timer); timer = null; } if (!transaction || transaction.state !== "staged") { transactionState.innerHTML = "<i></i>无待确认事务"; countdown.innerHTML = "<i></i>未开始"; confirmButton.disabled = true; return; } transactionState.innerHTML = "<i></i>事务 #" + transaction.id + " 待确认"; confirmButton.disabled = false; var remaining = Number(transaction.remaining_seconds || 60); function tick() { countdown.innerHTML = "<i></i>剩余 " + Math.max(0, remaining) + " 秒"; if (remaining <= 0) { window.clearInterval(timer); confirmButton.disabled = true; load(); } remaining -= 1; } tick(); timer = window.setInterval(tick, 1000); }
    function actualText(actual) { if (!actual || !actual.exists) { return "内核状态：接口不存在或未读取"; } return "内核状态：" + (actual.link_up ? "链路已连接" : "链路未连接") + (actual.address ? " · " + actual.address + "/" + actual.prefix : " · 无 IPv4"); }
    function load() { showError(""); return api("/api/v1/network").then(function (payload) { state.textContent = "网络 helper 已连接"; setInterface("eth0", payload.config.eth0); setInterface("eth1", payload.config.eth1); document.getElementById("eth0-actual").textContent = actualText(payload.actual && payload.actual.eth0) + " · 配置 " + payload.config.eth0.mode; document.getElementById("eth1-actual").textContent = actualText(payload.actual && payload.actual.eth1) + " · 配置 " + payload.config.eth1.mode; renderTransaction(payload.transaction); }).catch(function (reason) { if (reason.message !== "auth") { state.textContent = "网络 helper 不可用"; showError("无法读取网络状态：" + reason.message); } }); }
    function candidate() { var result = {}; ["eth0", "eth1"].forEach(function (prefix) { ["mode", "address", "gateway", "hostname"].forEach(function (suffix) { result[prefix + "_" + suffix] = form.elements[prefix + "_" + suffix].value; }); result[prefix + "_prefix"] = Number(form.elements[prefix + "_prefix"].value); result[prefix + "_dns1"] = form.elements[prefix + "_dns1"].value; result[prefix + "_dns2"] = form.elements[prefix + "_dns2"].value; result[prefix + "_dhcp_timeout_seconds"] = 15; }); return result; }
    function post(path, body) { return api(path, {method: "POST", headers: {"Content-Type": "application/json", "X-CSRF-Token": csrfToken}, body: JSON.stringify(body || {})}); }
    form.addEventListener("submit", function (event) { event.preventDefault(); var password = document.getElementById("current-password").value; if (!password) { showError("请输入当前密码进行再认证"); return; } post("/api/v1/network/stage", {current_password: password, candidate: candidate()}).then(function () { document.getElementById("current-password").value = ""; showSaved("候选地址已试应用，请确认新地址仍可访问。"); return load(); }).catch(function (reason) { if (reason.message !== "auth") { showError("试应用失败：" + reason.message); } }); });
    confirmButton.addEventListener("click", function () { post("/api/v1/network/confirm").then(function () { showSaved("网络候选已确认并写入持久配置。"); return load(); }).catch(function (reason) { if (reason.message !== "auth") { showError("确认失败：" + reason.message); } }); });
    document.getElementById("rollback-network").addEventListener("click", function () { var password = document.getElementById("current-password").value; if (!password) { showError("回滚也需要当前密码再认证"); return; } post("/api/v1/network/rollback", {current_password: password}).then(function () { showSaved("候选网络已回滚。"); document.getElementById("current-password").value = ""; return load(); }).catch(function (reason) { if (reason.message !== "auth") { showError("回滚失败：" + reason.message); } }); });
    document.getElementById("reload-network").addEventListener("click", load); ["eth0", "eth1"].forEach(function (prefix) { form.elements[prefix + "_mode"].addEventListener("change", function () { toggleInterface(prefix); }); });
    api("/api/v1/session").then(function (session) { csrfToken = session.csrf_token; return load(); }).catch(function () {});
}());
