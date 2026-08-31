(function () {
    "use strict";

    var modal = document.getElementById("password-modal");
    var form = document.getElementById("password-form");
    var error = document.getElementById("form-error");
    var toast = document.getElementById("toast");
    var toastTimer;

    function showToast(message) {
        toast.textContent = message;
        toast.classList.add("visible");
        window.clearTimeout(toastTimer);
        toastTimer = window.setTimeout(function () {
            toast.classList.remove("visible");
        }, 2800);
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
        closeModal();
        showToast("演示操作完成：密码表单已通过校验");
    });
    document.querySelectorAll('[data-action="revoke-other-sessions"]').forEach(function (button) {
        button.addEventListener("click", function () {
            showToast("演示操作完成：其他会话已撤销");
        });
    });
    document.querySelectorAll('[data-action="remove-session"]').forEach(function (button) {
        button.addEventListener("click", function () {
            var row = button.closest("tr");
            if (row) {
                row.remove();
            }
            showToast("演示操作完成：会话已从列表移除");
        });
    });
}());
