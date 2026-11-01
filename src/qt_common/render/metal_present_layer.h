// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

namespace QtCommon::Render {

void* AttachMetalPresentLayer(void* native_view);

void FitMetalPresentLayerToView(void* native_view);

} // namespace QtCommon::Render
