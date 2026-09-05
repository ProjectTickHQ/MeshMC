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

#pragma once

#include "CatPack.h"
#include "ITheme.h"

#include <QString>
#include <QList>
#include <QDir>
#include <QIcon>
#include <QPalette>
#include <memory>
#include <map>
#include <vector>

class ITheme;

struct IconThemeEntry {
	QString id;
	QString name;
	QString family;
	QString variant;
};

class ThemeManager
{
  public:
	ThemeManager();
	~ThemeManager();

	void addTheme(std::unique_ptr<ITheme> theme);

	ITheme* getTheme(const QString& id);

	void setApplicationTheme(const QString& id, bool initial);

	void setIconTheme(const QString& name);

	void applyCurrentlySelectedTheme(bool initial = false);

	std::vector<ITheme*> allThemes();

	QStringList families();

	std::vector<ITheme*> themesInFamily(const QString& family);

	QList<IconThemeEntry> iconThemes() const;

	QStringList iconThemeFamilies() const;

	QList<IconThemeEntry> iconThemesInFamily(const QString& family) const;

	QString resolveIconTheme(const QString& family) const;

	QString bestIconThemeForPalette(const QString& currentIconId) const;

	/// Folder holding the user's custom widget themes.
	QDir getApplicationThemesFolder();

	/// Folder holding the user's custom icon themes.
	QDir getIconThemesFolder();

	/*!
	 * Drops every registered theme, icon theme and cat pack and scans them
	 * from disk again, so newly added themes show up without a restart.
	 * NOTE: invalidates every ITheme* previously handed out by getTheme().
	 */
	void refresh();

	// CatPack API
	QString getCatPack(const QString& catName = QString());
	QList<CatPack*> getValidCatPacks();
	QDir getCatPacksFolder();

  private:
	std::map<QString, std::unique_ptr<ITheme>> m_themes;
	QList<IconThemeEntry> m_iconThemes;
	std::map<QString, std::unique_ptr<CatPack>> m_catPacks;
	QDir m_catPacksFolder;
	QDir m_applicationThemeFolder;
	QDir m_iconThemeFolder;
	QString m_defaultStyle;
	QPalette m_defaultPalette;

	void initializeThemes();
	void initializeCustomThemes(ITheme* baseTheme);
	void initIconThemes();
	void initCustomIconThemes();
	void initializeCatPacks();
	void addCatPack(std::unique_ptr<CatPack> catPack);
#ifdef Q_OS_MACOS
	struct MacState;
	std::unique_ptr<MacState> m_macState;
#endif
	void initialize();
	void setTitlebarColorOfAllWindowsOnMac(QColor color);
	void setTitlebarColorOnMac(WId windowId, QColor color);
	void stopSettingNewWindowColorsOnMac();
};
