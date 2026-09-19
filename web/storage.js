(function () {
    "use strict";

    function redirect() {
        window.location.replace("/login");
    }

    function fetchJson(path) {
        return fetch(path, {credentials: "same-origin"}).then(function (response) {
            if (response.status === 401) {
                redirect();
                throw new Error("auth");
            }
            if (!response.ok) {
                throw new Error("request failed");
            }
            return response.json();
        });
    }

    function applyRuntimeMode(mode) {
        document.querySelectorAll("[data-runtime]").forEach(function (element) {
            element.hidden = element.getAttribute("data-runtime") !== mode;
        });
    }

    function detectRuntimeMode() {
        return fetch("/api/v1/snapshot/latest", {credentials: "same-origin"})
            .then(function (response) {
                if (response.status === 503) {
                    return null;
                }
                if (!response.ok) {
                    throw new Error("runtime mode failed");
                }
                return response.json();
            }).then(function (snapshot) {
                return snapshot && Number(snapshot.schema_version) === 3 ? "v3" : "legacy";
            });
    }

    function renderEntries(target, entries, empty, mode) {
        target.textContent = "";
        if (!entries.length) {
            var emptyMessage = document.createElement("span");
            emptyMessage.className = "muted-cell";
            emptyMessage.textContent = empty;
            target.appendChild(emptyMessage);
            return;
        }
        entries.slice(0, 20).forEach(function (item) {
            var row = document.createElement("div");
            row.className = "storage-row";
            var name = document.createElement("strong");
            name.textContent = item.name || "--";
            var detail = document.createElement("span");
            if (item.generation) {
                detail.textContent = mode === "v3"
                    ? "generation " + item.generation + " · 三源时间与质量"
                    : "generation " + item.generation + " · " + (item.payload_status || "");
            } else {
                detail.textContent = "事件 #" + item.id + " · " +
                    (item.frame_count || 0) + " 帧";
            }
            row.appendChild(name);
            row.appendChild(detail);
            target.appendChild(row);
        });
    }

    function loadAll() {
        return detectRuntimeMode().then(function (mode) {
            applyRuntimeMode(mode);
            return fetchJson("/api/v1/frames").then(function (payload) {
                var historyMode = Number(payload.schema_version) === 3 ? "v3" : "legacy";
                if (historyMode !== mode) {
                    throw new Error("history mode mismatch");
                }
                renderEntries(
                    document.getElementById("frames-list"),
                    Array.isArray(payload.entries) ? payload.entries : [],
                    mode === "v3" ? "尚无 v3 历史快照" : "尚无周期帧",
                    mode);
                if (mode === "v3") {
                    return null;
                }
                return fetchJson("/api/v1/events").then(function (eventPayload) {
                    renderEntries(
                        document.getElementById("events-list"),
                        Array.isArray(eventPayload.entries) ? eventPayload.entries : [],
                        "尚无事件包",
                        mode);
                });
            });
        }).catch(function (error) {
            if (error.message !== "auth") {
                document.getElementById("toast").textContent = "存储索引暂时不可用";
                document.getElementById("toast").classList.add("visible");
            }
        });
    }

    document.querySelectorAll('[data-action="logout"]').forEach(function (button) {
        button.addEventListener("click", function () {
            fetch("/api/v1/session", {method: "GET", credentials: "same-origin"})
                .then(function (response) { return response.json(); })
                .then(function (session) {
                    return fetch("/api/v1/session", {
                        method: "DELETE",
                        credentials: "same-origin",
                        headers: {"X-CSRF-Token": session.csrf_token}
                    });
                }).then(redirect).catch(redirect);
        });
    });

    loadAll();
}());
