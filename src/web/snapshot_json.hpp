// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include "acquisition/acquisition.hpp"

#include <optional>
#include <string>

namespace uhf::web {

std::optional<std::string> render_snapshot_json(
    const acquisition::ServingView& serving_view);

}  // namespace uhf::web
