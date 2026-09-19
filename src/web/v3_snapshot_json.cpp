// SPDX-License-Identifier: GPL-3.0-only
#include "web/v3_snapshot_json.hpp"

#include <array>
#include <chrono>
#include <cstddef>
#include <string_view>

namespace uhf::web {
namespace {

std::string escape(std::string_view text) {
    std::string out;
    out.reserve(text.size());
    for (const char character : text) {
        if (character == '\\' || character == '"') {
            out.push_back('\\');
        }
        out.push_back(character);
    }
    return out;
}

void append_status(std::string& body, const v3::SourceStatus& status) {
    body.append("{\"has_sample\":");
    body.append(status.has_sample ? "true" : "false");
    body.append(",\"online\":");
    body.append(status.online ? "true" : "false");
    body.append(",\"communication_alarm\":");
    body.append(status.communication_alarm ? "true" : "false");
    body.append(",\"stale\":");
    body.append(status.stale ? "true" : "false");
    body.append(",\"consecutive_failures\":");
    body.append(std::to_string(status.consecutive_failures));
    body.append(",\"freshness_limit_ms\":");
    body.append(std::to_string(status.freshness_limit_ms));
    body.append(",\"acquisition\":{\"unit_id\":");
    body.append(std::to_string(status.acquisition.unit_id));
    body.append(",\"poll_interval_ms\":");
    body.append(std::to_string(status.acquisition.poll_interval_ms));
    body.append(",\"response_timeout_ms\":");
    body.append(std::to_string(status.acquisition.response_timeout_ms));
    body.append(",\"retry_delay_ms\":");
    body.append(std::to_string(status.acquisition.retry_delay_ms));
    body.append(",\"late_frame_quarantine_ms\":");
    body.append(std::to_string(status.acquisition.late_frame_quarantine_ms));
    body.append(",\"max_retries\":");
    body.append(std::to_string(status.acquisition.max_retries));
    body.push_back('}');
    const auto attempt_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        status.last_attempt_utc.time_since_epoch()).count();
    const auto success_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        status.last_success_utc.time_since_epoch()).count();
    body.append(",\"last_attempt_ms\":");
    body.append(attempt_ms > 0 ? std::to_string(attempt_ms) : "null");
    body.append(",\"last_success_ms\":");
    body.append(success_ms > 0 ? std::to_string(success_ms) : "null");
    body.append(",\"last_error\":\"");
    body.append(escape(status.last_error));
    body.append("\"}");
}

void append_value(std::string& body, const v3::Value& value) {
    body.append("{\"valid\":");
    body.append(value.valid() ? "true" : "false");
    body.append(",\"quality\":");
    body.append(std::to_string(static_cast<int>(value.quality)));
    body.append(",\"value\":");
    if (value.valid()) {
        body.append(std::to_string(value.value));
    } else {
        body.append("null");
    }
    body.push_back('}');
}

}  // namespace

std::string render_v3_snapshot_json(const v3::UnifiedSnapshot& snapshot) {
    std::string body = "{\"schema_version\":3,\"generation\":" +
        std::to_string(snapshot.generation) + ",\"sources\":{\"pd\":";
    append_status(body, snapshot.pd_status);
    body.append(",\"current\":");
    append_status(body, snapshot.current_status);
    body.append(",\"temperature\":");
    append_status(body, snapshot.temperature_status);
    body.append("},\"measurements\":[");
    for (std::size_t index = 0U; index < snapshot.measurements.size(); ++index) {
        if (index != 0U) {
            body.push_back(',');
        }
        body.append("{\"name\":\"");
        body.append(v3::kValueNames[index]);
        body.append("\",\"value\":");
        append_value(body, snapshot.measurements[index]);
        body.push_back('}');
    }
    body.append("],\"alarms\":[");
    for (std::size_t alarm = 0U; alarm < v3::kAlarmCount; ++alarm) {
        if (alarm != 0U) {
            body.push_back(',');
        }
        body.append("{\"index\":");
        body.append(std::to_string(alarm));
        body.append(",\"valid\":");
        body.append(snapshot.alarm_valid[alarm] ? "true" : "false");
        body.append(",\"active\":");
        body.append(snapshot.alarm_active[alarm] ? "true" : "false");
        body.push_back('}');
    }
    body.append("],\"pd\":[");
    for (std::size_t channel = 0U; channel < v3::kChannelCount; ++channel) {
        if (channel != 0U) {
            body.push_back(',');
        }
        body.append("{\"channel\":");
        body.append(std::to_string(channel + 1U));
        body.append(",\"valid\":");
        body.append(snapshot.pd_valid[channel] ? "true" : "false");
        body.append(",\"features\":[");
        for (std::size_t feature = 0U; feature < v3::kFeatureCount; ++feature) {
            if (feature != 0U) {
                body.push_back(',');
            }
            const v3::PdFeature& value = snapshot.pd[channel].features[feature];
            const bool valid = snapshot.pd_valid[channel] && value.valid;
            body.append("{\"raw\":");
            body.append(std::to_string(value.raw));
            body.append(",\"valid\":");
            body.append(valid ? "true" : "false");
            body.append(",\"value\":");
            if (valid) {
                body.append(std::to_string(value.value));
            } else {
                body.append("null");
            }
            body.push_back('}');
        }
        body.append("],\"spectrum\":[");
        for (std::size_t point = 0U; point < v3::kSpectrumPoints; ++point) {
            if (point != 0U) {
                body.push_back(',');
            }
            if (snapshot.pd_valid[channel] &&
                snapshot.pd[channel].spectrum_received[point]) {
                body.append(std::to_string(snapshot.pd[channel].spectrum_raw[point]));
            } else {
                body.append("null");
            }
        }
        body.append("]}");
    }
    body.append("]}\n");
    return body;
}

}  // namespace uhf::web
