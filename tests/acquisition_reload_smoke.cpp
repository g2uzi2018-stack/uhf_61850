// SPDX-License-Identifier: GPL-3.0-only
#include "acquisition/acquisition.hpp"
#include "acquisition/loopback_pd1000.hpp"

#include <chrono>
#include <iostream>

namespace {

bool expect(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "acquisition reload smoke failed: " << message << '\n';
        return false;
    }
    return true;
}

}  // namespace

int main() {
    uhf::acquisition::LoopbackPd1000Port serial_port;
    uhf::acquisition::SnapshotStore snapshot_store;
    uhf::acquisition::AcquisitionOptions options;
    options.slave_id = 2U;
    options.inter_frame_silence = std::chrono::microseconds::zero();
    options.retry_delay = std::chrono::milliseconds::zero();
    options.quarantine_duration = std::chrono::milliseconds(1);

    uhf::acquisition::AcquisitionEngine engine(serial_port, snapshot_store, options);
    if (!expect(engine.poll_once(), "initial configured slave ID did not poll") ||
        !expect(
            engine.options().slave_id == 2U,
            "initial acquisition options were not retained") ||
        !expect(snapshot_store.latest()->generation == 1U, "initial generation")) {
        return 1;
    }

    options.slave_id = 7U;
    options.response_timeout = std::chrono::milliseconds(120);
    options.max_retries = 1U;
    engine.update_options(options);
    if (!expect(engine.poll_once(), "reloaded slave ID did not poll") ||
        !expect(engine.options().slave_id == 7U, "reloaded slave ID was not applied") ||
        !expect(
            engine.options().response_timeout == std::chrono::milliseconds(120),
            "reloaded response timeout was not applied") ||
        !expect(engine.options().max_retries == 1U, "reloaded retry count was not applied") ||
        !expect(snapshot_store.latest()->generation == 2U, "reloaded generation") ||
        !expect(
            snapshot_store.serving_view().status.availability ==
                uhf::acquisition::Availability::fresh,
            "reloaded poll was not fresh")) {
        return 1;
    }

    std::cout << "acquisition reload smoke: OK\n";
    return 0;
}
