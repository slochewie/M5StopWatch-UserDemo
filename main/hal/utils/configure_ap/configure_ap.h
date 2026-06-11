/*
 * SPDX-FileCopyrightText: 2026 M5Stack Technology CO LTD
 *
 * SPDX-License-Identifier: MIT
 */
#pragma once

#include <functional>
#include <string_view>

namespace configure_ap {

inline bool& runningFlag()
{
    static bool running = false;
    return running;
}

inline void setRunning(bool running)
{
    runningFlag() = running;
}

inline bool isRunning()
{
    return runningFlag();
}

bool run(const std::function<void(std::string_view)>& onLog);
void requestStop();

}  // namespace configure_ap
