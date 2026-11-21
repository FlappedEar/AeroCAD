// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <functional>
#include <stdexcept>

namespace aerocad::core {

/// Thrown by a long computation that was asked to stop (CAD-30). Nothing it produced so far
/// is used.
class Cancelled : public std::runtime_error {
public:
    Cancelled() : std::runtime_error("cancelled") {}
};

/// Polled by long computations: returns true when the user cancelled. May be empty.
using CancelCheck = std::function<bool()>;

/// Throws Cancelled when `cancelled` is set and returns true.
inline void throwIfCancelled(const CancelCheck& cancelled)
{
    if (cancelled && cancelled())
        throw Cancelled();
}

} // namespace aerocad::core
