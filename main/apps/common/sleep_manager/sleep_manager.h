/*
 * SPDX-FileCopyrightText: 2026 M5Stack Technology CO LTD
 *
 * SPDX-License-Identifier: MIT
 */
#pragma once
#include <cstdint>

namespace sleep_manager {

void begin();
void update();

void setInhibit(bool inhibit);
bool isInhibited();
bool isSleeping();

void markActivity();
void wake();

}  // namespace sleep_manager
