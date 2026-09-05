/* SPDX-FileCopyrightText: 2026 Project Tick
 * SPDX-FileContributor: Project Tick
 * SPDX-License-Identifier: Apache-2.0
 *
 * Copyright 2013-2021 MultiMC Contributors
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

#include "BaseInstance.h"

#include <QFileInfo>
#include <QDir>
#include <QDebug>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRegularExpression>

#include <algorithm>

#include "settings/INISettingsObject.h"
#include "settings/Setting.h"
#include "settings/OverrideSetting.h"

#include "FileSystem.h"
#include "Commandline.h"
#include "BuildConfig.h"

BaseInstance::BaseInstance(SettingsObjectPtr globalSettings,
						   SettingsObjectPtr settings, const QString& rootDir)
	: QObject()
{
	m_settings = settings;
	m_rootDir = rootDir;

	m_settings->registerSetting("name", "Unnamed Instance");
	m_settings->registerSetting("iconKey", "default");
	m_settings->registerSetting("notes", "");
	m_settings->registerSetting("lastLaunchTime", 0);
	m_settings->registerSetting("totalTimePlayed", 0);
	m_settings->registerSetting("lastTimePlayed", 0);

	/* Shortcuts this instance has had written for it, as a compact JSON
	 * array. One string keeps it to a single INI key, and the list is
	 * short enough that nothing here needs to be fast. */
	m_settings->registerSetting("shortcuts", QString());

	/* Which profiler this instance launches under. Empty means none. This
	 * is a property of the instance rather than of one click on a menu
	 * entry, so that Launch Offline (and anything else that starts the
	 * game) profiles too. */
	m_settings->registerSetting("Profiler", "");

	/* Pack-source provenance keys (consumed by the PackUpdater
	 * plugin via instance_setting_get). All optional — empty string
	 * means "not a pack-managed instance" or "we couldn't recover
	 * this field". InstanceImportTask writes these when a pack is
	 * imported through Modrinth / CurseForge; PackUpdater's attach
	 * UI can also write them after the fact. */
	m_settings->registerSetting("PackProvider", "");
	m_settings->registerSetting("PackId", "");
	m_settings->registerSetting("PackSlug", "");
	m_settings->registerSetting("PackVersionId", "");
	m_settings->registerSetting("PackVersionLabel", "");
	m_settings->registerSetting("PackIconUrl", "");
	m_settings->registerSetting("PackSourceUrl", "");
	m_settings->registerSetting("PackInstalledAt", "");
	m_settings->registerSetting("PackManifestSha512", "");

	/* The pack's own title, as the catalogue spells it. Separate from
	 * "name" because the user may rename the instance and we still
	 * want to be able to say which pack it is. Instances imported
	 * before this key existed leave it empty; managedPackName() falls
	 * back rather than showing a blank field. */
	m_settings->registerSetting("PackName", "");

	/* An update source the user supplied by hand, for instances with
	 * no catalogue entry (drag-dropped zips, hand-made instances).
	 * Remembered so the field is still filled in next time the page
	 * is opened. */
	m_settings->registerSetting("PackUpdateUrl", "");

	// Custom Commands
	auto commandSetting = m_settings->registerSetting(
		{"OverrideCommands", "OverrideLaunchCmd"}, false);
	m_settings->registerOverride(globalSettings->getSetting("PreLaunchCommand"),
								 commandSetting);
	m_settings->registerOverride(globalSettings->getSetting("WrapperCommand"),
								 commandSetting);
	m_settings->registerOverride(globalSettings->getSetting("PostExitCommand"),
								 commandSetting);

	// Console
	auto consoleSetting = m_settings->registerSetting("OverrideConsole", false);
	m_settings->registerOverride(globalSettings->getSetting("ShowConsole"),
								 consoleSetting);
	m_settings->registerOverride(globalSettings->getSetting("AutoCloseConsole"),
								 consoleSetting);
	m_settings->registerOverride(
		globalSettings->getSetting("ShowConsoleOnError"), consoleSetting);
	m_settings->registerOverride(globalSettings->getSetting("LogPrePostOutput"),
								 consoleSetting);

	m_settings->registerPassthrough(
		globalSettings->getSetting("ConsoleMaxLines"), nullptr);
	m_settings->registerPassthrough(
		globalSettings->getSetting("ConsoleOverflowStop"), nullptr);
}

QString BaseInstance::getPreLaunchCommand()
{
	return settings()->get("PreLaunchCommand").toString();
}

QString BaseInstance::getWrapperCommand()
{
	return settings()->get("WrapperCommand").toString();
}

QString BaseInstance::getPostExitCommand()
{
	return settings()->get("PostExitCommand").toString();
}

int BaseInstance::getConsoleMaxLines() const
{
	auto lineSetting = settings()->getSetting("ConsoleMaxLines");
	bool conversionOk = false;
	int maxLines = lineSetting->get().toInt(&conversionOk);
	if (!conversionOk) {
		maxLines = lineSetting->defValue().toInt();
		qWarning() << "ConsoleMaxLines has nonsensical value, defaulting to"
				   << maxLines;
	}
	return maxLines;
}

bool BaseInstance::shouldStopOnConsoleOverflow() const
{
	return settings()->get("ConsoleOverflowStop").toBool();
}

void BaseInstance::iconUpdated(QString key)
{
	if (iconKey() == key) {
		emit propertiesChanged(this);
	}
}

void BaseInstance::invalidate()
{
	changeStatus(Status::Gone);
	qDebug() << "Instance" << id() << "has been invalidated.";
}

void BaseInstance::changeStatus(BaseInstance::Status newStatus)
{
	Status status = currentStatus();
	if (status != newStatus) {
		m_status = newStatus;
		emit statusChanged(status, newStatus);
	}
}

BaseInstance::Status BaseInstance::currentStatus() const
{
	return m_status;
}

QString BaseInstance::id() const
{
	return QFileInfo(instanceRoot()).fileName();
}

bool BaseInstance::isRunning() const
{
	return m_isRunning;
}

void BaseInstance::setRunning(bool running)
{
	if (running == m_isRunning)
		return;

	m_isRunning = running;

	if (!m_settings->get("RecordGameTime").toBool()) {
		emit runningStatusChanged(running);
		return;
	}

	if (running) {
		m_timeStarted = QDateTime::currentDateTime();
	} else {
		QDateTime timeEnded = QDateTime::currentDateTime();

		qint64 current = settings()->get("totalTimePlayed").toLongLong();
		settings()->set("totalTimePlayed",
						current + m_timeStarted.secsTo(timeEnded));
		settings()->set("lastTimePlayed", m_timeStarted.secsTo(timeEnded));

		emit propertiesChanged(this);
	}

	emit runningStatusChanged(running);
}

int64_t BaseInstance::totalTimePlayed() const
{
	qint64 current = settings()->get("totalTimePlayed").toLongLong();
	if (m_isRunning) {
		QDateTime timeNow = QDateTime::currentDateTime();
		return current + m_timeStarted.secsTo(timeNow);
	}
	return current;
}

int64_t BaseInstance::lastTimePlayed() const
{
	if (m_isRunning) {
		QDateTime timeNow = QDateTime::currentDateTime();
		return m_timeStarted.secsTo(timeNow);
	}
	return settings()->get("lastTimePlayed").toLongLong();
}

void BaseInstance::resetTimePlayed()
{
	settings()->reset("totalTimePlayed");
	settings()->reset("lastTimePlayed");
}

QString BaseInstance::instanceType() const
{
	return m_settings->get("InstanceType").toString();
}

QString BaseInstance::instanceRoot() const
{
	return m_rootDir;
}

SettingsObjectPtr BaseInstance::settings() const
{
	return m_settings;
}

bool BaseInstance::canLaunch() const
{
	return (!hasVersionBroken() && !isRunning());
}

bool BaseInstance::reloadSettings()
{
	return m_settings->reload();
}

qint64 BaseInstance::lastLaunch() const
{
	return m_settings->get("lastLaunchTime").value<qint64>();
}

void BaseInstance::setLastLaunch(qint64 val)
{
	// FIXME: if no change, do not set. setting involves saving a file.
	m_settings->set("lastLaunchTime", val);
	emit propertiesChanged(this);
}

void BaseInstance::setNotes(QString val)
{
	// FIXME: if no change, do not set. setting involves saving a file.
	m_settings->set("notes", val);
}

QString BaseInstance::notes() const
{
	return m_settings->get("notes").toString();
}

/* ---- Managed pack provenance ------------------------------------------
 *
 * Every getter trims. These keys are written by the importer, but they
 * are also plain text in instance.cfg, and a stray trailing space in a
 * pack id turns into a 404 that is very hard to read back from a log.
 */

QString BaseInstance::managedPackProvider() const
{
	return m_settings->get("PackProvider").toString().trimmed();
}

QString BaseInstance::managedPackId() const
{
	return m_settings->get("PackId").toString().trimmed();
}

QString BaseInstance::managedPackSlug() const
{
	return m_settings->get("PackSlug").toString().trimmed();
}

QString BaseInstance::managedPackName() const
{
	const QString recorded = m_settings->get("PackName").toString().trimmed();
	if (!recorded.isEmpty()) {
		return recorded;
	}
	/* Older imports never recorded the title. The slug is the next best
	 * thing - it is derived from the title and is at least stable - and
	 * failing that the instance name, which the user chose and will
	 * recognise even if it is not what the catalogue calls the pack. */
	const QString slug = managedPackSlug();
	if (!slug.isEmpty()) {
		return slug;
	}
	return name();
}

QString BaseInstance::managedPackVersionId() const
{
	return m_settings->get("PackVersionId").toString().trimmed();
}

QString BaseInstance::managedPackVersionName() const
{
	return m_settings->get("PackVersionLabel").toString().trimmed();
}

QString BaseInstance::managedPackSourceUrl() const
{
	return m_settings->get("PackSourceUrl").toString().trimmed();
}

bool BaseInstance::isManagedPack() const
{
	return !managedPackProvider().isEmpty();
}

bool BaseInstance::hasManagedPackId() const
{
	/* An id without a provider does not say which API to ask, and a
	 * provider without an id gives every endpoint nothing to key on, so
	 * a catalogue lookup needs both. */
	return !managedPackProvider().isEmpty() && !managedPackId().isEmpty();
}

QString BaseInstance::managedPackUpdateUrl() const
{
	return m_settings->get("PackUpdateUrl").toString().trimmed();
}

void BaseInstance::setManagedPackUpdateUrl(const QString& url)
{
	const QString trimmed = url.trimmed();
	if (managedPackUpdateUrl() == trimmed) {
		// Writing a setting writes a file; this one is fed by a
		// textChanged signal, so it would otherwise write on every
		// keystroke that does not change the trimmed value.
		return;
	}
	m_settings->set("PackUpdateUrl", trimmed);
}

void BaseInstance::setManagedPackVersion(const QString& versionId,
										 const QString& versionName)
{
	if (managedPackVersionId() == versionId.trimmed() &&
		managedPackVersionName() == versionName.trimmed()) {
		return;
	}
	m_settings->set("PackVersionId", versionId.trimmed());
	m_settings->set("PackVersionLabel", versionName.trimmed());
	emit propertiesChanged(this);
}

QList<ShortcutData> BaseInstance::shortcuts() const
{
	const QJsonDocument document = QJsonDocument::fromJson(
		m_settings->get("shortcuts").toString().toUtf8());
	if (!document.isArray()) {
		return {};
	}

	QList<ShortcutData> results;
	for (const QJsonValue& entry : document.array()) {
		const QJsonObject object = entry.toObject();
		const QString name = object.value("name").toString();
		const QString filePath = object.value("filePath").toString();
		const int target = object.value("target").toInt(-1);

		if (filePath.isEmpty() || target < 0 ||
			target > static_cast<int>(ShortcutTarget::Other)) {
			qWarning() << "Skipping an unreadable shortcut entry of instance"
					   << id();
			continue;
		}

		/* Whatever is no longer there is no longer ours: the user has
		 * moved or removed it by hand, and something else may well be
		 * sitting at that path now. */
		if (!QFileInfo::exists(filePath)) {
			qDebug() << "Forgetting shortcut" << name << "of instance" << id()
					 << "-- nothing at" << filePath << "any more";
			continue;
		}

		results.append(
			{name, filePath, static_cast<ShortcutTarget>(target)});
	}
	return results;
}

void BaseInstance::registerShortcut(const ShortcutData& shortcut)
{
	if (shortcut.filePath.isEmpty()) {
		return;
	}

	QList<ShortcutData> current = shortcuts();

	/* Writing over an existing shortcut is one shortcut, not two. */
	/* QList::removeIf() is Qt 6.1+; the erase-remove idiom is equivalent and
	 * compiles against both Qt 5 and Qt 6. */
	current.erase(std::remove_if(current.begin(), current.end(),
								 [&shortcut](const ShortcutData& known) {
									 return known.filePath ==
											shortcut.filePath;
								 }),
				  current.end());
	current.append(shortcut);

	qDebug() << "Instance" << id() << "now owns shortcut" << shortcut.name
			 << "at" << shortcut.filePath;
	setShortcuts(current);
}

void BaseInstance::setShortcuts(const QList<ShortcutData>& shortcuts)
{
	QJsonArray array;
	for (const ShortcutData& shortcut : shortcuts) {
		array.append(QJsonObject{{"name", shortcut.name},
								 {"filePath", shortcut.filePath},
								 {"target", static_cast<int>(shortcut.target)}});
	}

	m_settings->set("shortcuts",
					QString::fromUtf8(QJsonDocument(array).toJson(
						QJsonDocument::Compact)));
}

QString BaseInstance::profilerKey() const
{
	return m_settings->get("Profiler").toString();
}

void BaseInstance::setProfilerKey(const QString& key)
{
	if (profilerKey() == key) {
		// Writing a setting means writing a file. Not for a no-op.
		return;
	}
	m_settings->set("Profiler", key);
	emit profilerChanged();
}

void BaseInstance::setIconKey(QString val)
{
	// FIXME: if no change, do not set. setting involves saving a file.
	m_settings->set("iconKey", val);
	emit propertiesChanged(this);
}

QString BaseInstance::iconKey() const
{
	return m_settings->get("iconKey").toString();
}

void BaseInstance::setName(QString val)
{
	// FIXME: if no change, do not set. setting involves saving a file.
	m_settings->set("name", val);
	emit propertiesChanged(this);
}

QString BaseInstance::name() const
{
	return m_settings->get("name").toString();
}

QString BaseInstance::windowTitle() const
{
	return BuildConfig.MESHMC_NAME + ": " +
		   name().replace(QRegularExpression("[ \n\r\t]+"), " ");
}

// FIXME: why is this here? move it to MinecraftInstance!!!
QStringList BaseInstance::extraArguments() const
{
	return Commandline::splitArgs(settings()->get("JvmArgs").toString());
}

shared_qobject_ptr<LaunchTask> BaseInstance::getLaunchTask()
{
	return m_launchProcess;
}
