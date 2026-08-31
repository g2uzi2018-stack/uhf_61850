(function () {
    "use strict";

    var form = document.getElementById("login-form");
    var error = document.getElementById("login-error");
    var button = form.querySelector("button[type=submit]");

    function responseMessage(response, fallback) {
        return response.json().then(function (payload) {
            return payload && payload.error ? payload.error : fallback;
        }).catch(function () {
            return fallback;
        });
    }

    form.addEventListener("submit", function (event) {
        event.preventDefault();
        error.textContent = "";
        button.disabled = true;
        fetch("/api/v1/session", {
            method: "POST",
            credentials: "same-origin",
            headers: {"Content-Type": "application/json"},
            body: JSON.stringify({
                username: form.elements.username.value,
                password: form.elements.password.value
            })
        }).then(function (response) {
            if (response.ok) {
                window.location.replace("/");
                return null;
            }
            return responseMessage(response, response.status === 429 ? "登录尝试过于频繁，请稍后再试。" : "账号或密码错误。").then(function (message) {
                error.textContent = message;
                return null;
            });
        }).catch(function () {
            error.textContent = "无法连接服务，请确认开发机服务仍在运行。";
        }).finally(function () {
            button.disabled = false;
        });
    });
}());
