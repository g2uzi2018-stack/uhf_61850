// SPDX-License-Identifier: GPL-3.0-only
#include "web/snapshot_json.hpp"

#include "domain/snapshot.hpp"

#include <array>
#include <cstddef>
#include <string>
#include <string_view>

namespace {

std::string_view payload_status_name(uhf::domain::PayloadStatus status) {
    switch (status) {
    case uhf::domain::PayloadStatus::good:
        return "good";
    case uhf::domain::PayloadStatus::degraded:
        return "degraded";
    case uhf::domain::PayloadStatus::not_refreshed:
        return "not_refreshed";
    }
    return "degraded";
}

}  // namespace

namespace uhf::web {

std::optional<std::string> render_snapshot_json(
    const acquisition::ServingView& serving_view) {
    if (!serving_view.snapshot) {
        return std::nullopt;
    }

    const acquisition::PublishedSnapshot& latest = *serving_view.snapshot;
    const domain::ParsedSnapshot& payload = latest.payload;
    constexpr std::array<std::string_view, 5U> measurement_names = {
        "average", "frequency", "peak", "phase", "noise"};
    const bool snapshot_usable = serving_view.status.availability != acquisition::Availability::invalid;
    std::string body = "{\"schema_version\":1,\"generation\":" +
        std::to_string(latest.generation) + ",\"availability\":\"" +
        std::string(acquisition::availability_name(serving_view.status.availability)) +
        "\",\"payload_status\":\"" +
        std::string(payload_status_name(payload.payload_status)) +
        "\",\"poll_duration_ms\":" + std::to_string(latest.poll_duration.count()) +
        ",\"measurements\":[";
    for (std::size_t index = 0; index < payload.measurements.size(); ++index) {
        if (index > 0U) {
            body.append(",");
        }
        const domain::Measurement& measurement = payload.measurements[index];
        body.append("{\"name\":\"");
        body.append(measurement_names[index]);
        body.append("\",\"raw\":");
        body.append(std::to_string(measurement.raw));
        body.append(",\"value\":");
        if (snapshot_usable && measurement.valid) {
            body.append(std::to_string(measurement.value));
        } else {
            body.append("null");
        }
        body.append(",\"valid\":");
        body.append(measurement.valid ? "true" : "false");
        body.append("}");
    }
    body.append("],\"spectrum\":[");
    for (std::size_t index = 0; index < payload.spectrum.size(); ++index) {
        if (index > 0U) {
            body.append(",");
        }
        if (snapshot_usable && payload.spectrum_valid.test(index)) {
            body.append(std::to_string(payload.spectrum[index]));
        } else {
            body.append("null");
        }
    }
    body.append("],\"spectrum_valid\":[");
    for (std::size_t index = 0; index < payload.spectrum_valid.size(); ++index) {
        if (index > 0U) {
            body.append(",");
        }
        body.append(snapshot_usable && payload.spectrum_valid.test(index) ? "true" : "false");
    }
    body.append("]}\n");
    return body;
}

}  // namespace uhf::web
