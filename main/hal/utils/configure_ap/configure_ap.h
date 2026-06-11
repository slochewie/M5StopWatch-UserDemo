/*
 * SPDX-FileCopyrightText: 2026 M5Stack Technology CO LTD
 *
 * SPDX-License-Identifier: MIT
 */
#pragma once

#include <functional>
#include <string_view>

namespace configure_ap {

bool run(const std::function<void(std::string_view)>& onLog);
void requestStop();
void setRunning(bool running);
bool isRunning();

}  // namespace configure_ap
