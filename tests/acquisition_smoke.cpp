// SPDX-License-Identifier: GPL-3.0-only
#include "acquisition/acquisition.hpp"

#include <iostream>

int main(int argc, char* argv[]) {
    if (argc != 2) {
        std::cerr << "acquisition smoke failed: expected PTY device\n";
        return 1;
    }

    try {
        uhf::acquisition::PosixSerialPort serial_port(argv[1]);
        uhf::acquisition::SnapshotStore snapshot_store;
        uhf::acquisition::AcquisitionEngine engine(serial_port, snapshot_store);
        if (!engine.poll_once()) {
            std::cerr << "acquisition smoke failed: " << engine.last_error() << '\n';
            return 1;
        }
        const auto latest = snapshot_store.latest();
        if (!latest || latest->generation != 1U || latest->payload.measurements[0].value != -55 ||
            latest->payload.measurements[1].value != 12 ||
            latest->payload.measurements[2].value != -50 ||
            latest->payload.spectrum[0] != -60 ||
            latest->payload.payload_status != uhf::domain::PayloadStatus::good) {
            std::cerr << "acquisition smoke failed: published snapshot mismatch\n";
            return 1;
        }
        std::cout << "acquisition smoke: OK\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "acquisition smoke failed: " << error.what() << '\n';
        return 1;
    }
}
