/******************************************************************************
This source code is licensed under the MIT license found in the
LICENSE file in the root directory of this source tree.
*******************************************************************************/

#include "common/HelperFunction.h"
#include <iostream>

using namespace NetworkAnalytical;

void NetworkAnalytical::debug_print(const std::string& msg) noexcept {
    std::cout << msg << std::endl;
}
