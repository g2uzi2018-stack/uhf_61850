// SPDX-License-Identifier: GPL-3.0-only
#include "domain/snapshot.hpp"
#include "web/snapshot_json.hpp"

#include <chrono>
#include <cstdint>
#include <iostream>
#include <string>

namespace {

bool expect(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "web snapshot smoke failed: " << message << '\n';
        return false;
    }
    return true;
}

uhf::acquisition::ServingView make_view(uhf::acquisition::Availability availability) {
    uhf::domain::ParsedSnapshot payload;
    payload.payload_status = uhf::domain::PayloadStatus::good;
    payload.measurements[0] = uhf::domain::Measurement{uhf::domain::kInvalidDataMarker, -70, false};
    payload.measurements[1] = uhf::domain::Measurement{12U, 12, true};
    payload.spectrum[0] = -60;
    payload.spectrum_valid.set(0U, true);

    uhf::acquisition::PublishedSnapshot snapshot;
    snapshot.generation = 9U;
    snapshot.started_at = std::chrono::steady_clock::now();
    snapshot.completed_at = snapshot.started_at;
    snapshot.payload = payload;

    uhf::acquisition::ServingView view;
    view.snapshot = snapshot;
    view.status.availability = availability;
    return view;
}

}  // namespace

int main() {
    const auto stale_body = uhf::web::render_snapshot_json(
        make_view(uhf::acquisition::Availability::stale));
    if (!expect(stale_body.has_value(), "stale snapshot was not rendered") ||
        !expect(stale_body->find("\"availability\":\"stale\"") != std::string::npos,
            "stale availability missing") ||
        !expect(stale_body->find("\"value\":null") != std::string::npos,
            "invalid measurement was rendered as a number") ||
        !expect(stale_body->find("\"value\":12") != std::string::npos,
            "valid stale measurement was discarded") ||
        !expect(stale_body->find("\"spectrum\":[-60") != std::string::npos,
            "valid stale spectrum was discarded")) {
        return 1;
    }

    const auto invalid_body = uhf::web::render_snapshot_json(
        make_view(uhf::acquisition::Availability::invalid));
    if (!expect(invalid_body.has_value(), "invalid view with retained data was not rendered") ||
        !expect(invalid_body->find("\"availability\":\"invalid\"") != std::string::npos,
            "invalid availability missing") ||
        !expect(invalid_body->find("\"spectrum\":[null") != std::string::npos,
            "invalid spectrum was rendered as usable data") ||
        !expect(invalid_body->find("\"spectrum_valid\":[false") != std::string::npos,
            "invalid spectrum validity was not cleared") ||
        !expect(invalid_body->find("\"value\":12") == std::string::npos,
            "invalid view rendered a normal measurement value")) {
        return 1;
    }

    uhf::acquisition::ServingView empty_view;
    if (!expect(!uhf::web::render_snapshot_json(empty_view).has_value(),
            "empty view unexpectedly produced a snapshot")) {
        return 1;
    }

    std::cout << "web snapshot smoke: OK\n";
    return 0;
}
