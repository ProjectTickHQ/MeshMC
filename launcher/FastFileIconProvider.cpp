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

#include "FastFileIconProvider.h"

#include <QApplication>
#include <QStyle>

QIcon FastFileIconProvider::icon(const QFileInfo& info) const
{
	/* isAlias() covers macOS aliases, which are not symlinks and would
	 * otherwise be drawn as plain files. It is Qt 6.4+ only, so on a Qt 5
	 * build a macOS alias gets the plain-file icon instead of the link
	 * one -- cosmetic, and only on macOS. */
#if QT_VERSION >= QT_VERSION_CHECK(6, 4, 0)
	const bool link =
		info.isSymbolicLink() || info.isAlias() || info.isShortcut();
#else
	const bool link = info.isSymbolicLink() || info.isShortcut();
#endif

	QStyle::StandardPixmap pixmap;
	if (info.isDir()) {
		pixmap = link ? QStyle::SP_DirLinkIcon : QStyle::SP_DirIcon;
	} else {
		pixmap = link ? QStyle::SP_FileLinkIcon : QStyle::SP_FileIcon;
	}

	return QApplication::style()->standardIcon(pixmap);
}
