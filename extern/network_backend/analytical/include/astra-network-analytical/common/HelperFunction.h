/******************************************************************************
This source code is licensed under the MIT license found in the
LICENSE file in the root directory of this source tree.
*******************************************************************************/

#pragma once

#include <string>

namespace NetworkAnalytical {

/**
 * Compile-time verbose-logging flag (ENABLE_VERBOSE_LOGS / VERBOSELOG).
 * Exposed so hot call sites can guard the *construction* of debug messages:
 * debug_print() discards its argument when logging is off, but the string
 * concatenations at the call site would still run. Wrapping a site in
 * `if (kVerboseLogging) { ... }` makes it statically dead code instead.
 */
#ifdef VERBOSELOG
inline constexpr bool kVerboseLogging = true;
#else
inline constexpr bool kVerboseLogging = false;
#endif

/**
 * Conditional debug print. Enabled/disabled at compile time.
 *
 * @param msg message to print for debugging
 */
void debug_print(const std::string& msg) noexcept;

}  // namespace NetworkAnalytical
