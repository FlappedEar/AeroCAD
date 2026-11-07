// SPDX-License-Identifier: GPL-3.0-or-later
#include "aerocad/geometry/KernelError.hpp"

#include <Standard_Failure.hxx>

#include <string>

namespace aerocad::geometry {

void rethrowAsKernelError()
{
    try {
        throw;
    } catch (const Standard_Failure& e) {
        std::string message = "modelling kernel error";
        if (const char* type = e.DynamicType()->Name(); type != nullptr && *type != '\0')
            message += std::string(" (") + type + ")";
        if (const char* text = e.GetMessageString(); text != nullptr && *text != '\0')
            message += std::string(": ") + text;
        throw KernelError(message);
    }
}

} // namespace aerocad::geometry
