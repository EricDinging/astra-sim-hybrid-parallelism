/******************************************************************************
This source code is licensed under the MIT license found in the
LICENSE file in the root directory of this source tree.
*******************************************************************************/

#include "common/HelperFunction.h"
#include <iostream>

using namespace NetworkAnalytical;

// Define a constant boolean based on the preprocessor flag
#ifdef VERBOSELOG
constexpr bool verbose_logging = true;
#else
constexpr bool verbose_logging = false;
#endif

void NetworkAnalytical::debug_print(const std::string& msg) noexcept {
    if (verbose_logging) {
        std::cout << msg << std::endl;
    }
}
