// SPDX-License-Identifier: GPL-3.0-only
#include "acquisition/acquisition.hpp"
#include "acquisition/loopback_pd1000.hpp"

#include <cstdint>
#include <iostream>

int main() {
    uhf::acquisition::LoopbackPd1000Port serial_port;
    uhf::acquisition::SnapshotStore snapshot_store;
    uhf::acquisition::AcquisitionEngine engine(serial_port, snapshot_store);
    if (!engine.poll_once()) {
        std::cerr << "loopback acquisition smoke failed: " << engine.last_error() << '\n';
        return 1;
    }
    const auto latest = snapshot_store.latest();
    if (!latest || latest->generation != 1U ||
        latest->payload.measurements[0U].value != -55 ||
        latest->payload.measurements[1U].value != 12 ||
        latest->payload.measurements[2U].value != -50 ||
        latest->payload.spectrum[0U] != -60 ||
        latest->payload.spectrum.size() != 3600U ||
        latest->payload.payload_status != uhf::domain::PayloadStatus::good) {
        std::cerr << "loopback acquisition smoke failed: snapshot mismatch\n";
        return 1;
    }
    std::cout << "loopback acquisition smoke: OK\n";
    return 0;
}
