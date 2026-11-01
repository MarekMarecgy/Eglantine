// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#import <AppKit/AppKit.h>
#import <QuartzCore/QuartzCore.h>

#include "common/logging.h"
#include "qt_common/render/metal_present_layer.h"

@interface EglantineMetalPresentLayer : CAMetalLayer
@end

@implementation EglantineMetalPresentLayer
- (id<CAAction>)actionForKey:(NSString*)event {
    return nil;
}
@end

namespace QtCommon::Render {
namespace {

CALayer* FindQtHostLayer(NSView* view) {
    CALayer* view_layer = view.layer;
    if (view_layer == nil || [view_layer isKindOfClass:[CAMetalLayer class]]) {
        return view_layer;
    }
    for (CALayer* sublayer in view_layer.sublayers) {
        if ([sublayer isKindOfClass:[CAMetalLayer class]]) {
            return sublayer;
        }
    }
    return view_layer;
}

EglantineMetalPresentLayer* FindPresentLayer(CALayer* host_layer) {
    for (CALayer* sublayer in host_layer.sublayers) {
        if ([sublayer isKindOfClass:[EglantineMetalPresentLayer class]]) {
            return (EglantineMetalPresentLayer*)sublayer;
        }
    }
    return nil;
}

CGFloat BackingScaleFactor(NSView* view, CALayer* host_layer) {
    if (view.window != nil && view.window.backingScaleFactor > 0) {
        return view.window.backingScaleFactor;
    }
    return host_layer.contentsScale > 0 ? host_layer.contentsScale : 1.0;
}

void FitPresentLayer(NSView* view, CALayer* host_layer, EglantineMetalPresentLayer* present_layer) {
    present_layer.frame = host_layer.bounds;
    present_layer.contentsScale = BackingScaleFactor(view, host_layer);
}

} // Anonymous namespace

void* AttachMetalPresentLayer(void* native_view) {
    if (native_view == nullptr) {
        LOG_ERROR(Frontend, "Cannot create the Metal present layer: the render window has no view");
        return nullptr;
    }
    NSView* view = (__bridge NSView*)native_view;
    CALayer* host_layer = FindQtHostLayer(view);
    if (host_layer == nil) {
        LOG_ERROR(Frontend, "Cannot create the Metal present layer: the render view has no layer");
        return nullptr;
    }
    EglantineMetalPresentLayer* present_layer = FindPresentLayer(host_layer);
    if (present_layer == nil) {
        present_layer = [[EglantineMetalPresentLayer alloc] init];
        present_layer.opaque = YES;
        present_layer.autoresizingMask = kCALayerWidthSizable | kCALayerHeightSizable;
        [host_layer addSublayer:present_layer];
        [present_layer release];
    }
    FitPresentLayer(view, host_layer, present_layer);
    return (__bridge void*)present_layer;
}

void FitMetalPresentLayerToView(void* native_view) {
    if (native_view == nullptr) {
        return;
    }
    NSView* view = (__bridge NSView*)native_view;
    CALayer* host_layer = FindQtHostLayer(view);
    if (host_layer == nil) {
        return;
    }
    EglantineMetalPresentLayer* present_layer = FindPresentLayer(host_layer);
    if (present_layer != nil) {
        FitPresentLayer(view, host_layer, present_layer);
    }
}

} // namespace QtCommon::Render
