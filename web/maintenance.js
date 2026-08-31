(function () {
    "use strict";
    var csrf = "";
    var error = document.getElementById("maintenance-error");
    var saved = document.getElementById("maintenance-saved");
    var replacementPanel = document.createElement("section");
    replacementPanel.className = "panel settings-panel";
    replacementPanel.id = "certificate-replacement";
    replacementPanel.innerHTML = '<div class="panel-header"><div><h2>替换设备证书</h2><p>上传 PEM 证书链和匹配的私钥；服务端校验有效期、用途、SAN 和密钥匹配后才会原子替换。</p></div><span class="count-badge">需再认证</span></div><div class="settings-grid"><label>当前密码<input id="tls-password" type="password" autocomplete="current-password"><small>替换成功后当前会话立即失效</small></label><label class="wide">服务端证书链（PEM）<textarea id="tls-certificate" rows="7" autocomplete="off" spellcheck="false" placeholder="-----BEGIN CERTIFICATE-----"></textarea><small>单个请求最多 32 KiB；请包含完整证书链</small></label><label class="wide">私钥（PEM）<textarea id="tls-private-key" rows="7" autocomplete="off" spellcheck="false" placeholder="-----BEGIN PRIVATE KEY-----"></textarea><small>仅发送到当前设备，不会回显或写入日志</small></label></div><div class="settings-actions"><span class="form-error" id="tls-error" role="alert"></span><span class="settings-saved" id="tls-saved"></span><button class="primary-button" id="replace-tls" type="button">替换并热加载</button></div>';
    document.querySelector(".settings-content").insertBefore(replacementPanel, error.parentNode);
    var tlsError = document.getElementById("tls-error");
    var tlsSaved = document.getElementById("tls-saved");
    var tlsStatus = document.createElement("span");
    tlsStatus.className = "heading-badge";
    tlsStatus.id = "tls-status";
    tlsStatus.innerHTML = "<i></i>读取中";
    var tlsCertificateStatus = document.createElement("dd");
    tlsCertificateStatus.id = "tls-certificate-status";
    tlsCertificateStatus.textContent = "读取中…";
    var securityPanel = document.querySelector(".settings-content > section.settings-panel");
    securityPanel.querySelector(".panel-header").appendChild(tlsStatus);
    var details = securityPanel.querySelector(".snapshot-details");
    if (details) {
        var statusDetail = document.createElement("div");
        statusDetail.innerHTML = "<dt>证书状态</dt>";
        statusDetail.appendChild(tlsCertificateStatus);
        details.appendChild(statusDetail);
    }
    function redirect() { window.location.replace("/login"); }
    function message(text, ok) { (ok ? saved : error).textContent = text; (ok ? error : saved).textContent = ""; }
    function post(path, password) { return fetch(path, {method: "POST", credentials: "same-origin", headers: {"Content-Type": "application/json", "X-CSRF-Token": csrf}, body: JSON.stringify({current_password: password})}).then(function (response) { if (response.status === 401) { redirect(); throw new Error("auth"); } return response.json().then(function (payload) { if (!response.ok) { throw new Error(payload.error || "维护动作失败"); } return payload; }); }); }
    function tlsMessage(text, ok) { (ok ? tlsSaved : tlsError).textContent = text; (ok ? tlsError : tlsSaved).textContent = ""; }
    function loadTlsStatus() { fetch("/api/v1/tls", {credentials: "same-origin"}).then(function (response) { if (response.status === 401) { redirect(); throw new Error("auth"); } return response.json().then(function (payload) { if (!response.ok) { throw new Error(payload.error || "证书状态读取失败"); } return payload; }); }).then(function (payload) { var ready = payload.certificate_ready === true; tlsStatus.innerHTML = "<i></i>" + (ready ? "证书已就绪" : "TLS 未启用"); tlsCertificateStatus.textContent = ready ? "服务端证书已加载" : "恢复模式未加载证书"; }).catch(function (reason) { if (reason.message !== "auth") { tlsStatus.innerHTML = "<i></i>读取失败"; tlsCertificateStatus.textContent = "无法读取"; } }); }
    document.getElementById("replace-tls").addEventListener("click", function () { var button = this; var password = document.getElementById("tls-password").value; var certificate = document.getElementById("tls-certificate").value; var privateKey = document.getElementById("tls-private-key").value; if (!password || !certificate || !privateKey) { tlsMessage("请填写当前密码、证书链和私钥", false); return; } if (!window.confirm("确认替换设备证书？成功后当前会话会失效。")) { return; } button.disabled = true; tlsMessage("", true); fetch("/api/v1/tls", {method: "PUT", credentials: "same-origin", headers: {"Content-Type": "application/json", "X-CSRF-Token": csrf}, body: JSON.stringify({current_password: password, certificate_pem: certificate, private_key_pem: privateKey})}).then(function (response) { if (response.status === 401) { redirect(); throw new Error("auth"); } return response.json().then(function (payload) { if (!response.ok) { throw new Error(payload.error || "证书替换失败"); } return payload; }); }).then(function () { tlsMessage("证书已替换并热加载，请重新登录。", true); document.getElementById("tls-password").value = ""; document.getElementById("tls-certificate").value = ""; document.getElementById("tls-private-key").value = ""; }).catch(function (reason) { if (reason.message !== "auth") { tlsMessage(reason.message, false); button.disabled = false; } }); });
    function action(button, path, input, confirmation) { button.addEventListener("click", function () { var password = document.getElementById(input).value; if (!password) { message("请输入当前密码", false); return; } if (!window.confirm(confirmation)) { return; } button.disabled = true; post(path, password).then(function () { message("动作已提交，服务可能会断开当前页面。", true); }).catch(function (reason) { if (reason.message !== "auth") { message(reason.message, false); } }).then(function () { button.disabled = false; }); }); }
    document.getElementById("restart-service").addEventListener("click", function () { var button = this; var password = document.getElementById("service-password").value; if (!password) { message("请输入当前密码", false); return; } if (!window.confirm("确认重启 uhf-gateway 服务？")) { return; } button.disabled = true; post("/api/v1/maintenance/restart-service", password).then(function () { message("服务重启已提交。", true); }).catch(function (reason) { if (reason.message !== "auth") { message(reason.message, false); } }).then(function () { button.disabled = false; }); });
    document.getElementById("reboot-device").addEventListener("click", function () { var button = this; var password = document.getElementById("reboot-password").value; if (!password) { message("请输入当前密码", false); return; } if (!window.confirm("确认重启整台设备？")) { return; } button.disabled = true; post("/api/v1/maintenance/reboot", password).then(function () { message("设备重启已提交。", true); }).catch(function (reason) { if (reason.message !== "auth") { message(reason.message, false); } }).then(function () { button.disabled = false; }); });
    document.querySelectorAll('[data-action="logout"]').forEach(function (button) { button.addEventListener("click", function () { fetch("/api/v1/session", {method: "DELETE", credentials: "same-origin", headers: {"X-CSRF-Token": csrf}}).then(redirect).catch(redirect); }); });
    fetch("/api/v1/session", {credentials: "same-origin"}).then(function (response) { if (response.status === 401) { redirect(); throw new Error("auth"); } return response.json(); }).then(function (session) { csrf = session.csrf_token; loadTlsStatus(); }).catch(function () {});
}());
