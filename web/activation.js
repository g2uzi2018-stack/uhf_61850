(function () {
    "use strict";

    var form = document.getElementById("activation-form");
    var error = document.getElementById("activation-error");
    var success = document.getElementById("activation-success");
    var deviceId = document.getElementById("device-id");
    var button = form.querySelector("button[type=submit]");
    var transportFootnote = document.getElementById("transport-footnote");

    if (window.location.protocol === "https:") {
        transportFootnote.textContent = "当前使用 HTTPS。首次接入请先核对设备证书，再输入厂家提供的激活码。";
    }

    function responseMessage(response, fallback) {
        return response.json().then(function (payload) {
            return payload && payload.error ? payload.error : fallback;
        }).catch(function () {
            return fallback;
        });
    }

    function waitForRuntime() {
        window.setTimeout(function probe() {
            fetch("/api/v1/overview", {credentials: "same-origin", cache: "no-store"})
                .then(function (response) {
                    if (response.ok) {
                        window.location.replace("/overview");
                        return;
                    }
                    window.setTimeout(probe, 500);
                }).catch(function () {
                    window.setTimeout(probe, 500);
                });
        }, 500);
    }

    fetch("/api/v1/activation", {credentials: "same-origin", cache: "no-store"})
        .then(function (response) {
            if (!response.ok) {
                throw new Error("activation status unavailable");
            }
            return response.json();
        }).then(function (payload) {
            deviceId.textContent = payload.device_id || "未配置";
        }).catch(function () {
            deviceId.textContent = "读取失败";
            error.textContent = "无法读取设备标识，请检查服务配置。";
            button.disabled = true;
        });

    form.addEventListener("submit", function (event) {
        event.preventDefault();
        error.textContent = "";
        success.textContent = "";
        button.disabled = true;
        fetch("/api/v1/activation", {
            method: "POST",
            credentials: "same-origin",
            headers: {"Content-Type": "application/json"},
            body: JSON.stringify({code: form.elements.code.value})
        }).then(function (response) {
            if (response.ok) {
                success.textContent = "激活成功，正在启动采集与协议服务…";
                form.elements.code.value = "";
                waitForRuntime();
                return null;
            }
            return responseMessage(
                response,
                response.status === 429 ? "尝试过于频繁，请稍后再试。" : "激活码无效或激活状态无法保存。"
            ).then(function (message) {
                error.textContent = message;
                button.disabled = false;
                return null;
            });
        }).catch(function () {
            error.textContent = "激活请求失败，请确认设备服务仍在运行。";
            button.disabled = false;
        });
    });
}());
