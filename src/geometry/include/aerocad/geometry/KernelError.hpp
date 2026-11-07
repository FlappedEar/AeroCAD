// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <stdexcept>
#include <string>

namespace aerocad::geometry {

/// A modelling-kernel (Open CASCADE) failure translated into a standard exception, so
/// callers without OCCT headers can catch it as std::exception.
class KernelError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

/// Call only from inside a catch handler. Rethrows the exception being handled; an Open
/// CASCADE Standard_Failure (which does not derive from std::exception) is rethrown as a
/// KernelError carrying the kernel's exception type and message. Other exceptions are
/// rethrown unchanged.
[[noreturn]] void rethrowAsKernelError();

} // namespace aerocad::geometry
