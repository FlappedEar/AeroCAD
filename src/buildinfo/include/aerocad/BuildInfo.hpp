// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

namespace aerocad {

/// Identifies this build instead of a version number: local date and time of the build
/// and the short git hash, e.g. "2026.11.07-0747 (1a2b3c4)". The hash is left out when
/// the sources are not a git checkout.
[[nodiscard]] const char* buildStamp();

} // namespace aerocad
