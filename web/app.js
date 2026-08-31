(function () {
    "use strict";

    var modal = document.getElementById("password-modal");
    var form = document.getElementById("password-form");
    var error = document.getElementById("form-error");
    var toast = document.getElementById("toast");
    var csrfToken = "";
    var toastTimer;

    function showToast(message) {
        toast.textContent = message;
        toast.classList.add("visible");
        window.clearTimeout(toastTimer);
        toastTimer = window.setTimeout(function () {
            toast.classList.remove("visible");
        }, 2800);
    }

    function responseMessage(response, fallback) {
        return response.json().then(function (payload) {
            return payload && payload.error ? payload.error : fallback;
        }).catch(function () {
            return fallback;
        });
    }

    function openModal() {
        error.textContent = "";
        form.reset();
        modal.hidden = false;
        document.body.classList.add("modal-open");
        form.elements["current-password"].focus();
    }

    function closeModal() {
        modal.hidden = true;
        document.body.classList.remove("modal-open");
    }

    function redirectToLogin() {
        window.location.replace("/login");
    }

    function updatePasswordChecklist(mustChange) {
        var icon = document.getElementById("password-check-icon");
        var status = document.getElementById("password-check-status");
        if (!icon || !status) {
            return;
        }
        if (mustChange) {
            icon.textContent = "";
            icon.classList.remove("completed");
            status.textContent = "去设置";
            status.disabled = false;
        } else {
            icon.textContent = "✓";
            icon.classList.add("completed");
            status.textContent = "已完成";
            status.disabled = true;
        }
    }

    function loadSession() {
        return fetch("/api/v1/session", {credentials: "same-origin"}).then(function (response) {
            if (!response.ok) {
                redirectToLogin();
                return null;
            }
            return response.json();
        }).then(function (session) {
            if (!session) {
                return;
            }
            csrfToken = session.csrf_token;
            updatePasswordChecklist(Boolean(session.must_change));
            if (session.must_change) {
                window.setTimeout(openModal, 120);
                showToast("首次登录必须先修改管理员密码");
            }
        }).catch(function () {
            showToast("无法读取登录会话");
        });
    }

    function loadRuntimeStatus() {
        fetch("/api/v1/health", {credentials: "same-origin"}).then(function (response) {
            if (response.status === 401) {
                redirectToLogin();
                return null;
            }
            if (!response.ok) {
                throw new Error("health request failed");
            }
            return response.json();
        }).then(function (health) {
            if (!health) {
                return;
            }
            var acquisition = health.acquisition || {};
            var status = acquisition.status === "up" ? "采集正常" : acquisition.status === "degraded" ? "采集降级" : "采集离线";
            document.getElementById("runtime-status").textContent = "开发模式 · " + status;
            document.getElementById("runtime-status-dot").className = "status-dot " + (acquisition.status || "down");
        }).catch(function () {
            document.getElementById("runtime-status").textContent = "开发模式 · 状态不可用";
        });
    }

    document.querySelectorAll('[data-action="change-password"]').forEach(function (button) {
        button.addEventListener("click", openModal);
    });
    document.querySelectorAll('[data-action="close-modal"]').forEach(function (button) {
        button.addEventListener("click", closeModal);
    });
    modal.addEventListener("click", function (event) {
        if (event.target === modal) {
            closeModal();
        }
    });
    document.addEventListener("keydown", function (event) {
        if (event.key === "Escape" && !modal.hidden) {
            closeModal();
        }
    });
    form.addEventListener("submit", function (event) {
        event.preventDefault();
        var currentPassword = form.elements["current-password"].value;
        var newPassword = form.elements["new-password"].value;
        var confirmedPassword = form.elements["confirm-password"].value;
        if (newPassword.length < 12) {
            error.textContent = "新密码至少需要 12 位。";
            return;
        }
        if (newPassword !== confirmedPassword) {
            error.textContent = "两次输入的新密码不一致。";
            return;
        }
        fetch("/api/v1/password", {
            method: "PUT",
            credentials: "same-origin",
            headers: {
                "Content-Type": "application/json",
                "X-CSRF-Token": csrfToken
            },
            body: JSON.stringify({
                current_password: currentPassword,
                new_password: newPassword
            })
        }).then(function (response) {
            if (response.ok) {
                showToast("密码已保存，请重新登录");
                window.setTimeout(redirectToLogin, 500);
                return null;
            }
            if (response.status === 401) {
                redirectToLogin();
                return null;
            }
            return responseMessage(response, "密码保存失败").then(function (message) {
                error.textContent = message;
                return null;
            });
        }).catch(function () {
            error.textContent = "无法连接服务，请稍后再试。";
        });
    });
    document.querySelectorAll('[data-action="revoke-other-sessions"]').forEach(function (button) {
        button.addEventListener("click", function () {
            fetch("/api/v1/session/revoke-others", {
                method: "POST",
                credentials: "same-origin",
                headers: {"X-CSRF-Token": csrfToken}
            }).then(function (response) {
                if (response.ok) {
                    showToast("其他会话已撤销");
                } else if (response.status === 401) {
                    redirectToLogin();
                } else {
                    showToast("撤销其他会话失败");
                }
            }).catch(function () {
                showToast("无法连接服务");
            });
        });
    });
    document.querySelectorAll('[data-action="logout"]').forEach(function (button) {
        button.addEventListener("click", function () {
            fetch("/api/v1/session", {
                method: "DELETE",
                credentials: "same-origin",
                headers: {"X-CSRF-Token": csrfToken}
            }).then(function (response) {
                if (response.ok || response.status === 401) {
                    redirectToLogin();
                } else {
                    showToast("退出登录失败");
                }
            }).catch(function () {
                showToast("无法连接服务");
            });
        });
    });

    loadSession();
    loadRuntimeStatus();
    window.setInterval(loadRuntimeStatus, 6000);
}());
