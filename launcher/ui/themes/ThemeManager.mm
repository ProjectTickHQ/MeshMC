/* SPDX-FileCopyrightText: 2026 Project Tick
 * SPDX-FileContributor: Project Tick
 * SPDX-License-Identifier: Apache-2.0
 *
 * Copyright (C) 2026 Project Tick
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "ThemeManager.h"

#include <Foundation/Foundation.h>
#include <AppKit/AppKit.h>

struct ThemeManager::MacState
{
    NSObject* windowTitlebarObserver = nil;
};

ThemeManager::ThemeManager()
	: m_macState(std::make_unique<MacState>())
{
	initialize();
}

ThemeManager::~ThemeManager()
{
	stopSettingNewWindowColorsOnMac();
}

namespace {

bool isStatusBarWindow(NSWindow* window)
{
    return window && window.level == NSStatusWindowLevel;
}

} // namespace

void ThemeManager::setTitlebarColorOnMac(WId windowId, QColor color)
{
    if (!windowId) {
        return;
    }

    auto* nativeView = reinterpret_cast<NSView*>(windowId);
    NSWindow* nativeWindow = nativeView.window;

    if (!nativeWindow || isStatusBarWindow(nativeWindow)) {
        return;
    }

    nativeWindow.titlebarAppearsTransparent = YES;
    nativeWindow.backgroundColor = [NSColor colorWithSRGBRed:color.redF()
                                                       green:color.greenF()
                                                        blue:color.blueF()
                                                       alpha:color.alphaF()];
}

void ThemeManager::setTitlebarColorOfAllWindowsOnMac(QColor color)
{
    for (NSWindow* nativeWindow in NSApp.windows) {
        if (isStatusBarWindow(nativeWindow)) {
            continue;
        }

        setTitlebarColorOnMac(
            reinterpret_cast<WId>(nativeWindow.contentView),
            color
        );
    }

    stopSettingNewWindowColorsOnMac();

    NSNotificationCenter* notificationCenter =
        NSNotificationCenter.defaultCenter;

    m_macState->windowTitlebarObserver =
        [notificationCenter addObserverForName:NSWindowDidChangeOcclusionStateNotification
                                        object:nil
                                         queue:NSOperationQueue.mainQueue
                                    usingBlock:^(NSNotification* notification) {
                                        NSWindow* nativeWindow =
                                            notification.object;

                                        if (isStatusBarWindow(nativeWindow)) {
                                            return;
                                        }

                                        setTitlebarColorOnMac(
                                            reinterpret_cast<WId>(nativeWindow.contentView),
                                            color
                                        );
                                    }];
}

void ThemeManager::stopSettingNewWindowColorsOnMac()
{
    if (!m_macState ||
        !m_macState->windowTitlebarObserver) {
        return;
    }

    [NSNotificationCenter.defaultCenter
        removeObserver:m_macState->windowTitlebarObserver];
    m_macState->windowTitlebarObserver = nil;
}
