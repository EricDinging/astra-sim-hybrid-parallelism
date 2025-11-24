/******************************************************************************
This source code is licensed under the MIT license found in the
LICENSE file in the root directory of this source tree.
*******************************************************************************/

#pragma once

#include <string>

namespace NetworkAnalytical {

/**
 * Conditional debug print. Enabled/disabled at compile time.
 *
 * @param msg message to print for debugging
 */
void debug_print(const std::string& msg) noexcept;

}  // namespace NetworkAnalytical
