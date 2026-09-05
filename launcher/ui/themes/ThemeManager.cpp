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

#include "SystemTheme.h"
#include "DarkTheme.h"
#include "BrightTheme.h"
#include "GreenDarkTheme.h"
#include "GreenLightTheme.h"
#include "CustomTheme.h"

#include "Application.h"
#include "Exception.h"
#include <QApplication>
#include <QDebug>
#include <QDirIterator>
#include <QFileInfo>
#include <QImageReader>
#include <QSet>
#include <QSettings>
#include <QStyle>
#include <QStyleFactory>
#include <QSysInfo>
#include <xdgicon.h>

#ifndef Q_OS_MACOS

ThemeManager::ThemeManager()
{
	initialize();
}

ThemeManager::~ThemeManager()
{
	stopSettingNewWindowColorsOnMac();
}

#endif

void ThemeManager::initialize()
{
	// NOTE: captured once, before any theme is applied, so that the "System"
	// theme keeps reporting the real system look even after a refresh().
	const auto& style = QApplication::style();
	m_defaultStyle = style->objectName();
	m_defaultPalette = QApplication::palette();

	initializeThemes();
	initIconThemes();
	initializeCatPacks();
}

void ThemeManager::initializeThemes()
{
	// Default "System" theme
	addTheme(
		std::make_unique<SystemTheme>(m_defaultStyle, m_defaultPalette, true));

	// Built-in Fusion themes
	auto darkTheme = new DarkTheme();
	addTheme(std::unique_ptr<ITheme>(darkTheme));
	addTheme(std::make_unique<BrightTheme>());
	addTheme(std::make_unique<GreenDarkTheme>());
	addTheme(std::make_unique<GreenLightTheme>());

	// System widget themes from QStyleFactory
	QStringList styles = QStyleFactory::keys();
	for (auto& st : styles) {
#ifdef Q_OS_WINDOWS
		if (QSysInfo::productVersion() != "11" && st == "windows11") {
			continue;
		}
#endif
		addTheme(std::make_unique<SystemTheme>(st, m_defaultPalette, false));
	}

	// User themes from the themes/ folder. DarkTheme is the base every custom
	// theme inherits unspecified values from.
	initializeCustomThemes(darkTheme);
}

#ifndef Q_OS_MACOS
void ThemeManager::setTitlebarColorOnMac(WId windowId, QColor color) {}
void ThemeManager::setTitlebarColorOfAllWindowsOnMac(QColor color) {}
void ThemeManager::stopSettingNewWindowColorsOnMac() {}
#endif

void ThemeManager::initializeCustomThemes(ITheme* baseTheme)
{
	m_applicationThemeFolder = QDir("themes");
	if (!m_applicationThemeFolder.mkpath(".")) {
		qWarning() << "Couldn't create themes folder";
		return;
	}

	// First run, or the user wiped the folder: leave a starter theme behind so
	// there is something to copy and edit instead of a blank page.
	if (m_applicationThemeFolder.isEmpty(QDir::AllEntries |
										 QDir::NoDotAndDotDot)) {
		CustomTheme::writeSkeleton(baseTheme, QStringLiteral("custom"));
	}

	QDirIterator directoryIterator(m_applicationThemeFolder.path(),
								   QDir::Dirs | QDir::NoDotAndDotDot);
	while (directoryIterator.hasNext()) {
		QDir dir(directoryIterator.next());
		QFileInfo manifest(
			dir.absoluteFilePath(CustomTheme::manifestFileName));

		if (manifest.isFile()) {
			// theme.json based theme
			addTheme(CustomTheme::fromManifest(baseTheme, manifest));
		} else {
			// Plain stylesheet themes: every .qss/.css in the folder becomes
			// a theme of its own.
			QDirIterator styleSheetIterator(
				dir.absolutePath(), {"*.qss", "*.css"}, QDir::Files);
			while (styleSheetIterator.hasNext()) {
				QFileInfo info(styleSheetIterator.next());
				addTheme(CustomTheme::fromStyleSheet(baseTheme, info));
			}
		}
	}
}

void ThemeManager::refresh()
{
	m_themes.clear();
	m_catPacks.clear();

	initializeThemes();
	initIconThemes();
	initializeCatPacks();
}

void ThemeManager::addTheme(std::unique_ptr<ITheme> theme)
{
	QString id = theme->id();
	if (m_themes.find(id) != m_themes.end()) {
		// std::map::insert would silently drop it, which is very confusing
		// when it happens to a user's custom theme.
		qWarning() << "Theme" << id
				   << "not added to prevent id duplication";
		return;
	}
	m_themes.insert(std::make_pair(id, std::move(theme)));
}

ITheme* ThemeManager::getTheme(const QString& id)
{
	auto it = m_themes.find(id);
	if (it != m_themes.end()) {
		return it->second.get();
	}
	return nullptr;
}

void ThemeManager::setApplicationTheme(const QString& id, bool initial)
{
	auto theme = getTheme(id);
	if (theme) {
		theme->apply(initial);
		setTitlebarColorOfAllWindowsOnMac(qApp->palette().window().color());
	} else {
		qWarning() << "Tried to set invalid theme:" << id;
	}
}

void ThemeManager::setIconTheme(const QString& name)
{
	XdgIcon::setThemeName(name);
	QIcon::setFallbackThemeName(name);
}

void ThemeManager::applyCurrentlySelectedTheme(bool initial)
{
	auto settings = APPLICATION->settings();

	// Apply widget theme first (sets palette)
	auto applicationTheme = settings->get("ApplicationTheme").toString();
	if (applicationTheme.isEmpty()) {
		applicationTheme = "system";
	}
	setApplicationTheme(applicationTheme, initial);

	// Auto-resolve icon variant based on the now-active palette brightness
	auto iconTheme = settings->get("IconTheme").toString();
	if (!iconTheme.isEmpty()) {
		auto resolved = bestIconThemeForPalette(iconTheme);
		if (resolved != iconTheme) {
			settings->set("IconTheme", resolved);
		}
		setIconTheme(resolved);
	}
}

std::vector<ITheme*> ThemeManager::allThemes()
{
	std::vector<ITheme*> ret;
	for (auto& pair : m_themes) {
		ret.push_back(pair.second.get());
	}
	return ret;
}

QStringList ThemeManager::families()
{
	QStringList ret;
	QSet<QString> seen;
	for (auto& pair : m_themes) {
		QString fam = pair.second->family();
		if (!seen.contains(fam)) {
			seen.insert(fam);
			ret.append(fam);
		}
	}
	return ret;
}

std::vector<ITheme*> ThemeManager::themesInFamily(const QString& family)
{
	std::vector<ITheme*> ret;
	for (auto& pair : m_themes) {
		if (pair.second->family() == family) {
			ret.push_back(pair.second.get());
		}
	}
	return ret;
}

QList<IconThemeEntry> ThemeManager::iconThemes() const
{
	return m_iconThemes;
}

QStringList ThemeManager::iconThemeFamilies() const
{
	QStringList ret;
	QSet<QString> seen;
	for (const auto& entry : m_iconThemes) {
		const QString& fam = entry.family;
		if (!seen.contains(fam)) {
			seen.insert(fam);
			ret.append(fam);
		}
	}
	return ret;
}

QList<IconThemeEntry>
ThemeManager::iconThemesInFamily(const QString& family) const
{
	QList<IconThemeEntry> ret;
	for (const auto& entry : m_iconThemes) {
		if (entry.family == family) {
			ret.append(entry);
		}
	}
	return ret;
}

QString ThemeManager::resolveIconTheme(const QString& family) const
{
	auto entries = iconThemesInFamily(family);
	if (entries.size() <= 1) {
		return entries.isEmpty() ? QString() : entries[0].id;
	}

	// Check if family has variants
	bool hasVariants = false;
	for (const auto& entry : entries) {
		if (!entry.variant.isEmpty()) {
			hasVariants = true;
			break;
		}
	}

	if (!hasVariants) {
		return entries[0].id;
	}

	// Auto-detect based on current palette brightness
	auto windowColor = QApplication::palette().color(QPalette::Window);
	bool isDark = windowColor.lightnessF() < 0.5;

	for (const auto& entry : entries) {
		QString v = entry.variant.toLower();
		if (isDark && v == "dark")
			return entry.id;
		if (!isDark && v == "light")
			return entry.id;
	}

	return entries[0].id;
}

QString
ThemeManager::bestIconThemeForPalette(const QString& currentIconId) const
{
	// Find the family of the current icon theme
	QString family;
	for (const auto& entry : m_iconThemes) {
		if (entry.id == currentIconId) {
			family = entry.family;
			break;
		}
	}

	if (family.isEmpty()) {
		return currentIconId;
	}

	// Resolve the best variant for that family based on current palette
	QString resolved = resolveIconTheme(family);
	return resolved.isEmpty() ? currentIconId : resolved;
}

namespace
{

/*!
 * Reads `<dir>/index.theme` into \a out.
 *
 * The freedesktop `[Icon Theme] Name` key supplies the display name. Family
 * and variant are MeshMC extensions (`X-MeshMC-Family` / `X-MeshMC-Variant`):
 * two folders sharing a family but differing in variant are treated as the
 * dark and light flavours of one theme and picked automatically, matching how
 * the built-in Flat and Breeze themes behave.
 *
 * \return false when the folder is not an icon theme, or has no Name.
 */
bool readIconThemeIndex(const QDir& dir, IconThemeEntry& out)
{
	const QString indexPath =
		dir.absoluteFilePath(QStringLiteral("index.theme"));
	if (!QFileInfo::exists(indexPath)) {
		qDebug() << "Skipping" << dir.path() << ": no index.theme";
		return false;
	}

	QSettings index(indexPath, QSettings::IniFormat);
	index.beginGroup(QStringLiteral("Icon Theme"));
	const QString name = index.value(QStringLiteral("Name")).toString();
	const QString family =
		index.value(QStringLiteral("X-MeshMC-Family")).toString();
	const QString variant =
		index.value(QStringLiteral("X-MeshMC-Variant")).toString();
	index.endGroup();

	if (name.isEmpty()) {
		qWarning() << "Icon theme at" << dir.path()
				   << "has no Name in index.theme";
		return false;
	}

	// The folder name is the id: that is what Qt's icon loader resolves
	// against QIcon::themeSearchPaths().
	out.id = dir.dirName();
	out.name = name;
	// No family declared means the theme stands alone, so it is its own family.
	out.family = family.isEmpty() ? name : family;
	out.variant = variant;
	return true;
}

} // namespace

void ThemeManager::initIconThemes()
{
	m_iconThemes = {
		{"pe_colored", QObject::tr("Default"), QObject::tr("Default"),
		 QString()},
		{"multimc", QStringLiteral("MultiMC"), QStringLiteral("MultiMC"),
		 QString()},
		{"pe_dark", QObject::tr("Simple (Dark Icons)"),
		 QObject::tr("Simple (Dark Icons)"), QString()},
		{"pe_light", QObject::tr("Simple (Light Icons)"),
		 QObject::tr("Simple (Light Icons)"), QString()},
		{"pe_blue", QObject::tr("Simple (Blue Icons)"),
		 QObject::tr("Simple (Blue Icons)"), QString()},
		{"pe_colored", QObject::tr("Simple (Colored Icons)"),
		 QObject::tr("Simple (Colored Icons)"), QString()},
		{"OSX", QStringLiteral("OSX"), QStringLiteral("OSX"), QString()},
		{"iOS", QStringLiteral("iOS"), QStringLiteral("iOS"), QString()},
		{"flat", QObject::tr("Flat (Light)"), QStringLiteral("Flat"),
		 QObject::tr("Light")},
		{"flat_white", QObject::tr("Flat (Dark)"), QStringLiteral("Flat"),
		 QObject::tr("Dark")},
		{"breeze_dark", QObject::tr("Breeze (Dark)"), QStringLiteral("Breeze"),
		 QObject::tr("Dark")},
		{"breeze_light", QObject::tr("Breeze (Light)"),
		 QStringLiteral("Breeze"), QObject::tr("Light")},
	};

	// NOTE: there used to be a hardcoded "custom" entry here, but no such
	// theme ships in the resources, so selecting it left the UI without icons.
	// It is now discovered from iconthemes/custom like any other user theme,
	// and therefore only offered when it actually exists.
	initCustomIconThemes();
}

void ThemeManager::initCustomIconThemes()
{
	// NOTE: this folder is already on QIcon::themeSearchPaths(), set up in
	// Application::initialize(). All that is missing is discovery, so the
	// themes actually show up in the Appearance page.
	m_iconThemeFolder = QDir("iconthemes");
	if (!m_iconThemeFolder.mkpath(".")) {
		qWarning() << "Couldn't create iconthemes folder";
		return;
	}

	QSet<QString> knownIds;
	for (const auto& entry : m_iconThemes) {
		knownIds.insert(entry.id);
	}

	QDirIterator directoryIterator(m_iconThemeFolder.path(),
								   QDir::Dirs | QDir::NoDotAndDotDot);
	while (directoryIterator.hasNext()) {
		QDir dir(directoryIterator.next());

		IconThemeEntry entry;
		if (!readIconThemeIndex(dir, entry)) {
			continue;
		}

		if (knownIds.contains(entry.id)) {
			// Silently shadowing a built-in theme would be very confusing.
			qWarning() << "Icon theme" << entry.id
					   << "not added to prevent id duplication";
			continue;
		}

		knownIds.insert(entry.id);
		m_iconThemes.append(entry);
		qDebug() << "Loaded custom icon theme" << entry.id << "from"
				 << dir.path();
	}
}

void ThemeManager::initializeCatPacks()
{
	QList<std::pair<QString, QString>> defaultCats{
		{"kitteh", QObject::tr("Background Cat (from MultiMC)")},
		{"rory", QObject::tr("Rory ID 11 (drawn by Ashtaka)")},
		{"rory-flat",
		 QObject::tr("Rory ID 11 (flat edition, drawn by Ashtaka)")},
		{"teawie", QObject::tr("Teawie (drawn by SympathyTea)")}};

	for (const auto& [id, name] : defaultCats) {
		addCatPack(std::make_unique<BasicCatPack>(id, name));
	}

	// Create catpacks folder in data directory
	m_catPacksFolder = QDir("catpacks");
	if (!m_catPacksFolder.mkpath("."))
		qWarning() << "Couldn't create catpacks folder";

	QStringList supportedImageFormats;
	for (const auto& format : QImageReader::supportedImageFormats()) {
		supportedImageFormats.append("*." + format);
	}

	auto loadFiles = [this, &supportedImageFormats](const QDir& dir) {
		QDirIterator it(dir.absolutePath(), supportedImageFormats, QDir::Files);
		while (it.hasNext()) {
			QFileInfo info(it.next());
			addCatPack(std::make_unique<FileCatPack>(info));
		}
	};

	// Load image files in catpacks folder root
	loadFiles(m_catPacksFolder);

	// Load subdirectories
	QDirIterator directoryIterator(m_catPacksFolder.path(),
								   QDir::Dirs | QDir::NoDotAndDotDot);
	while (directoryIterator.hasNext()) {
		QDir dir(directoryIterator.next());
		QFileInfo manifest(dir.absoluteFilePath("catpack.json"));

		if (manifest.isFile()) {
			try {
				addCatPack(std::make_unique<JsonCatPack>(manifest));
			} catch (const Exception& e) {
				qWarning() << "Couldn't load catpack json:" << e.cause();
			}
		} else {
			loadFiles(dir);
		}
	}
}

void ThemeManager::addCatPack(std::unique_ptr<CatPack> catPack)
{
	QString id = catPack->id();
	if (m_catPacks.find(id) == m_catPacks.end())
		m_catPacks.emplace(id, std::move(catPack));
	else
		qWarning() << "CatPack" << id << "not added to prevent id duplication";
}

QString ThemeManager::getCatPack(const QString& catName)
{
	QString id = catName.isEmpty()
					 ? APPLICATION->settings()->get("BackgroundCat").toString()
					 : catName;

	auto it = m_catPacks.find(id);
	if (it != m_catPacks.end())
		return it->second->path();

	// Fallback to first available
	if (!m_catPacks.empty())
		return m_catPacks.begin()->second->path();

	return QString();
}

QList<CatPack*> ThemeManager::getValidCatPacks()
{
	QList<CatPack*> ret;
	ret.reserve(m_catPacks.size());
	for (auto& [id, pack] : m_catPacks) {
		ret.append(pack.get());
	}
	return ret;
}

QDir ThemeManager::getCatPacksFolder()
{
	return m_catPacksFolder;
}

QDir ThemeManager::getApplicationThemesFolder()
{
	return m_applicationThemeFolder;
}

QDir ThemeManager::getIconThemesFolder()
{
	return m_iconThemeFolder;
}
