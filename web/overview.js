(function () {
    "use strict";

    var pollTimer;
    var reconnectTimer;
    var websocket;
    var websocketConnected = false;
    var phaseStartDegree = 0;
    var csrfToken = "";
    var colors = ["#e8eef8", "#9cc8ff", "#5a9df2", "#786ed8", "#c55cb8", "#e66b66", "#e8a03e"];

    function redirectToLogin() {
        window.location.replace("/login");
    }

    function showToast(message) {
        var toast = document.getElementById("toast");
        toast.textContent = message;
        toast.classList.add("visible");
        window.clearTimeout(toastTimer);
        toastTimer = window.setTimeout(function () { toast.classList.remove("visible"); }, 2800);
    }

    var toastTimer;

    function valueOrDash(measurement) {
        return measurement && measurement.valid ? String(measurement.value) : "--";
    }

    function statusText(status) {
        return status === "up" ? "正常" : status === "degraded" ? "降级" : status === "disabled" ? "未启用" : "离线";
    }

    function statusClass(status) {
        return status === "up" ? "health-up" : status === "degraded" ? "health-degraded" : status === "disabled" ? "health-disabled" : "health-down";
    }

    function availabilityText(availability) {
        return availability === "fresh" ? "快照新鲜" : availability === "stale" ? "快照已过期" : "快照无效";
    }

    function updateHealth(payload) {
        var overall = document.getElementById("health-overall");
        overall.textContent = statusText(payload.status);
        overall.className = "count-badge " + statusClass(payload.status);
        ["acquisition", "modbus_tcp", "modbus_rtu", "storage", "iec61850"].forEach(function (name) {
            var item = payload[name] || {};
            var target = document.querySelector('[data-health="' + name + '"]');
            if (target) {
                target.textContent = statusText(item.status);
                target.className = statusClass(item.status);
            }
        });
        var acquisition = payload.acquisition || {};
        var runtimeStatus = document.getElementById("runtime-status");
        var dot = document.getElementById("runtime-status-dot");
        runtimeStatus.textContent = "采集 · " + statusText(acquisition.status);
        dot.className = "status-dot " + (acquisition.status || "down");
        var deviceStatus = document.getElementById("device-status");
        if (deviceStatus) {
            var deviceDot = deviceStatus.querySelector(".status-dot");
            var deviceLabel = deviceStatus.querySelector("span:not(.status-dot)");
            var overallStatus = payload.status || "down";
            deviceDot.className = "status-dot " + overallStatus;
            deviceLabel.textContent = "设备状态 · " + statusText(overallStatus);
        }
        var lastSuccess = document.getElementById("last-success");
        if (acquisition.age_ms === null || typeof acquisition.age_ms === "undefined") {
            lastSuccess.textContent = "尚无成功轮次";
        } else {
            lastSuccess.textContent = acquisition.age_ms < 1000 ? "刚刚" : Math.round(acquisition.age_ms / 1000) + " 秒前";
        }
    }

    function setCanvasSize(canvas) {
        var rect = canvas.getBoundingClientRect();
        var ratio = window.devicePixelRatio || 1;
        var width = Math.max(1, Math.round(rect.width * ratio));
        var height = Math.max(1, Math.round(rect.height * ratio));
        if (canvas.width !== width || canvas.height !== height) {
            canvas.width = width;
            canvas.height = height;
        }
        var context = canvas.getContext("2d");
        context.setTransform(ratio, 0, 0, ratio, 0, 0);
        return {context: context, width: rect.width, height: rect.height};
    }

    function colorFor(value) {
        var normalized = Math.max(0, Math.min(1, (value + 70) / 85));
        return colors[Math.min(colors.length - 1, Math.floor(normalized * colors.length))];
    }

    function phaseStartBin() {
        return Math.round(phaseStartDegree / 5) % 72;
    }

    function phaseLabel(degree) {
        var normalized = degree % 360;
        if (normalized < 0) { normalized += 360; }
        return normalized + "°";
    }

    function updatePhaseAxis() {
        document.getElementById("prpd-axis-start").textContent = phaseLabel(phaseStartDegree);
        document.getElementById("prpd-axis-middle").textContent = phaseLabel(phaseStartDegree + 180);
        document.getElementById("prpd-axis-end").textContent = phaseLabel(phaseStartDegree + 360);
    }

    function updateOverview(payload) {
        var title = String(payload.overview_title || "局部放电在线监测系统");
        var device = String(payload.overview_device || "1号主变");
        phaseStartDegree = Number(payload.phase_start_degree || 0);
        document.getElementById("overview-title").textContent = title;
        document.getElementById("overview-device").textContent = device;
        document.getElementById("overview-title-input").value = title;
        document.getElementById("overview-device-input").value = device;
        document.getElementById("phase-start-degree").value = phaseStartDegree;
        updatePhaseAxis();
    }

    function drawPrpd(values, valid) {
        var canvas = document.getElementById("prpd-canvas");
        var setup = setCanvasSize(canvas);
        var context = setup.context;
        context.clearRect(0, 0, setup.width, setup.height);
        context.fillStyle = "#f7faff";
        context.fillRect(0, 0, setup.width, setup.height);
        var cellWidth = setup.width / 72;
        var cellHeight = setup.height / 50;
        var startBin = phaseStartBin();
        for (var cycle = 0; cycle < 50; cycle += 1) {
            for (var phase = 0; phase < 72; phase += 1) {
                var index = cycle * 72 + phase;
                if (valid[index] && values[index] !== null) {
                    context.fillStyle = colorFor(values[index]);
                    var displayPhase = (phase - startBin + 72) % 72;
                    context.fillRect(displayPhase * cellWidth, cycle * cellHeight, cellWidth + 0.5, cellHeight + 0.5);
                }
            }
        }
        context.strokeStyle = "#dbe5f2";
        context.strokeRect(0.5, 0.5, setup.width - 1, setup.height - 1);
    }

    function drawPrps(values, valid) {
        var canvas = document.getElementById("prps-canvas");
        var setup = setCanvasSize(canvas);
        var context = setup.context;
        context.clearRect(0, 0, setup.width, setup.height);
        context.fillStyle = "#f7faff";
        context.fillRect(0, 0, setup.width, setup.height);
        var startBin = phaseStartBin();
        for (var cycle = 0; cycle < 50; cycle += 1) {
            for (var phase = 0; phase < 72; phase += 1) {
                var index = cycle * 72 + phase;
                if (!valid[index] || values[index] === null) {
                    continue;
                }
                context.fillStyle = colorFor(values[index]);
                context.beginPath();
                var displayPhase = (phase - startBin + 72) % 72;
                context.arc((displayPhase + 0.5) * setup.width / 72, (cycle + 0.5) * setup.height / 50, 2, 0, Math.PI * 2);
                context.fill();
            }
        }
        context.strokeStyle = "#dbe5f2";
        context.strokeRect(0.5, 0.5, setup.width - 1, setup.height - 1);
    }

    function updateSnapshot(payload) {
        window.latestSnapshot = payload;
        var measurements = {};
        (payload.measurements || []).forEach(function (measurement) { measurements[measurement.name] = measurement; });
        Object.keys(measurements).forEach(function (name) {
            var target = document.querySelector('[data-measurement="' + name + '"]');
            if (target) { target.textContent = valueOrDash(measurements[name]); }
        });
        document.getElementById("snapshot-generation").textContent = "generation " + payload.generation;
        var availability = payload.availability || "invalid";
        document.getElementById("payload-status").textContent = (payload.payload_status || "--") + " · " + availability;
        document.getElementById("poll-duration").textContent = payload.poll_duration_ms + " ms";
        document.getElementById("spectrum-count").textContent = (payload.spectrum_valid || []).filter(Boolean).length + " / 3600 有效";
        var snapshotBadge = document.getElementById("snapshot-badge");
        snapshotBadge.className = "heading-badge" + (availability === "fresh" ? "" : " dev-badge");
        snapshotBadge.textContent = availabilityText(availability);
        var marker = document.createElement("i");
        snapshotBadge.insertBefore(marker, snapshotBadge.firstChild);
        document.getElementById("snapshot-time").textContent = new Date().toLocaleTimeString();
        drawPrpd(payload.spectrum || [], payload.spectrum_valid || []);
        drawPrps(payload.spectrum || [], payload.spectrum_valid || []);
    }

    function fetchJson(path) {
        return fetch(path, {credentials: "same-origin"}).then(function (response) {
            if (response.status === 401) {
                redirectToLogin();
                throw new Error("authentication required");
            }
            if (!response.ok) {
                throw new Error(path + " returned " + response.status);
            }
            return response.json();
        });
    }

    function loadOverview() {
        return fetch("/api/v1/overview", {credentials: "same-origin"}).then(function (response) {
            if (!response.ok) { throw new Error("overview returned " + response.status); }
            return response.json();
        }).then(updateOverview);
    }

    function responseMessage(response, fallback) {
        return response.json().then(function (payload) {
            return payload && payload.error ? payload.error : fallback;
        }).catch(function () { return fallback; });
    }

    function saveOverview(event) {
        event.preventDefault();
        var saveError = document.getElementById("overview-settings-error");
        var saved = document.getElementById("overview-settings-saved");
        saveError.textContent = "";
        saved.textContent = "";
        fetch("/api/v1/session", {credentials: "same-origin"}).then(function (sessionResponse) {
            if (sessionResponse.status === 401) {
                redirectToLogin();
                throw new Error("authentication required");
            }
            if (!sessionResponse.ok) { throw new Error("session failed"); }
            return sessionResponse.json();
        }).then(function (session) {
            csrfToken = session.csrf_token;
            return fetch("/api/v1/config", {credentials: "same-origin"});
        }).then(function (configResponse) {
            if (configResponse.status === 401) {
                redirectToLogin();
                throw new Error("authentication required");
            }
            if (!configResponse.ok) { throw new Error("config failed"); }
            return configResponse.json();
        }).then(function (config) {
            var payload = Object.assign({}, config);
            delete payload.version;
            payload.overview_title = document.getElementById("overview-title-input").value;
            payload.overview_device = document.getElementById("overview-device-input").value;
            payload.phase_start_degree = Number(document.getElementById("phase-start-degree").value);
            return fetch("/api/v1/config", {
                method: "PUT",
                credentials: "same-origin",
                headers: {
                    "Content-Type": "application/json",
                    "X-CSRF-Token": csrfToken,
                    "If-Match": '"' + config.version + '"'
                },
                body: JSON.stringify(payload)
            });
        }).then(function (response) {
            if (response.status === 401) {
                redirectToLogin();
                throw new Error("authentication required");
            }
            if (!response.ok) {
                return responseMessage(response, "总览设置保存失败").then(function (message) { throw new Error(message); });
            }
            return response.json();
        }).then(function () {
            saved.textContent = "总览设置已保存";
            return loadOverview();
        }).catch(function (error) {
            if (error.message !== "authentication required") { saveError.textContent = error.message; }
        });
    }

    function refresh() {
        return Promise.all([fetchJson("/api/v1/health"), fetchJson("/api/v1/snapshot/latest")]).then(function (responses) {
            updateHealth(responses[0]);
            updateSnapshot(responses[1]);
        }).catch(function (error) {
            if (error.message !== "authentication required") {
                showToast("实时数据暂时不可用，正在重试");
            }
        }).then(function () {
            if (!websocketConnected) {
                pollTimer = window.setTimeout(function () {
                    pollTimer = null;
                    refresh();
                }, 6000);
            }
        });
    }

    function setTransport(text, className) {
        var badge = document.getElementById("transport-badge");
        badge.textContent = text;
        badge.className = "heading-badge " + (className || "dev-badge");
        var marker = document.createElement("i");
        badge.insertBefore(marker, badge.firstChild);
    }

    function connectWebSocket() {
        if (typeof window.WebSocket !== "function") {
            setTransport("HTTP 降级拉取", "dev-badge");
            return;
        }
        var protocol = window.location.protocol === "https:" ? "wss" : "ws";
        try {
            websocket = new window.WebSocket(protocol + "://" + window.location.host + "/ws/v1/telemetry");
        } catch (error) {
            setTransport("HTTP 降级拉取", "dev-badge");
            reconnectTimer = window.setTimeout(connectWebSocket, 2000);
            return;
        }
        websocket.onopen = function () {
            websocketConnected = true;
            if (pollTimer) {
                window.clearTimeout(pollTimer);
                pollTimer = null;
            }
            setTransport("WebSocket 已连接", "");
        };
        websocket.onmessage = function (event) {
            try {
                var message = JSON.parse(event.data);
                if (message.type === "telemetry") {
                    updateHealth(message.health || {});
                    updateSnapshot(message.snapshot || {});
                } else if (message.type === "health") {
                    updateHealth(message.data || {});
                }
            } catch (error) {
                showToast("实时消息格式无效");
            }
        };
        websocket.onclose = function () {
            websocketConnected = false;
            setTransport("HTTP 降级拉取", "dev-badge");
            if (!pollTimer) {
                refresh();
            }
            reconnectTimer = window.setTimeout(connectWebSocket, 2000);
        };
        websocket.onerror = function () {
            setTransport("正在重连 WebSocket", "dev-badge");
        };
    }

    document.querySelectorAll('[data-action="logout"]').forEach(function (button) {
        button.addEventListener("click", function () {
            fetch("/api/v1/session", {method: "GET", credentials: "same-origin"}).then(function (response) {
                if (!response.ok) { redirectToLogin(); return null; }
                return response.json();
            }).then(function (session) {
                if (!session) { return; }
                return fetch("/api/v1/session", {method: "DELETE", credentials: "same-origin", headers: {"X-CSRF-Token": session.csrf_token}});
            }).then(redirectToLogin).catch(function () { showToast("退出登录失败"); });
        });
    });
    window.addEventListener("resize", function () {
        var snapshot = window.latestSnapshot;
        if (snapshot) {
            drawPrpd(snapshot.spectrum || [], snapshot.spectrum_valid || []);
            drawPrps(snapshot.spectrum || [], snapshot.spectrum_valid || []);
        }
    });
    document.getElementById("overview-settings-form").addEventListener("submit", saveOverview);
    loadOverview().catch(function () { showToast("总览显示设置暂时不可用"); }).then(refresh);
    connectWebSocket();
}());
