// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include "v3/acquisition.hpp"

#include <string>

namespace uhf::web {

std::string render_v3_snapshot_json(const v3::UnifiedSnapshot& snapshot);

}  // namespace uhf::web
