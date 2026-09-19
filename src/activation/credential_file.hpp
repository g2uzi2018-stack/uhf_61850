// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include <filesystem>
#include <optional>
#include <string>

namespace uhf::activation {

/** Read one protected, single-line provisioning value without following links. */
std::optional<std::string> read_credential_file(
    const std::filesystem::path& path, std::string& error);

}  // namespace uhf::activation
