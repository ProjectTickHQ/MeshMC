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

#include "ManagedPackVersions.h"

#include <QCoreApplication>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <algorithm>

#include "Json.h"

namespace ManagedPack
{

	QString releaseTypeToString(ReleaseType type)
	{
		switch (type) {
			case ReleaseType::Release:
				return QStringLiteral("Release");
			case ReleaseType::Beta:
				return QStringLiteral("Beta");
			case ReleaseType::Alpha:
				return QStringLiteral("Alpha");
			case ReleaseType::Unknown:
				break;
		}
		/* Deliberately empty rather than "Unknown": label() tests for an
		 * empty string to decide whether to print the bracket at all. */
		return QString();
	}

	ReleaseType releaseTypeFromModrinth(const QString& value)
	{
		const QString normalised = value.trimmed().toLower();
		if (normalised == QLatin1String("release")) {
			return ReleaseType::Release;
		}
		if (normalised == QLatin1String("beta")) {
			return ReleaseType::Beta;
		}
		if (normalised == QLatin1String("alpha")) {
			return ReleaseType::Alpha;
		}
		return ReleaseType::Unknown;
	}

	ReleaseType releaseTypeFromCurseForge(int value)
	{
		switch (value) {
			case 1:
				return ReleaseType::Release;
			case 2:
				return ReleaseType::Beta;
			case 3:
				return ReleaseType::Alpha;
			default:
				return ReleaseType::Unknown;
		}
	}

	QString Version::label() const
	{
		/* Bracketed suffix, or nothing when we do not know the type. */
		const QString typeName = releaseTypeToString(releaseType);
		const QString releaseSuffix =
			typeName.isEmpty() ? QString()
							   : QStringLiteral(" [%1]").arg(typeName);

		/* The bare number is only worth repeating when the display name
		 * does not already contain it, which for most packs it does.
		 *
		 * The isEmpty() test is not redundant: contains("") is true for
		 * every string, and CurseForge never sends a version number at
		 * all, so without it this would take the "already contains it"
		 * branch by accident rather than on purpose. Same answer, but
		 * only the explicit test keeps saying so if the format below
		 * changes. */
		const QString numberPart =
			(versionNumber.isEmpty() || displayName.contains(versionNumber))
				? QString()
				: versionNumber;

		/* Append a game version only if none of the declared ones is
		 * already visible in the display name. Scanning the whole list
		 * (rather than stopping at the first) matters: a pack declaring
		 * ["1.20.1", "1.20.4"] whose name mentions 1.20.4 should not
		 * gain a contradictory " for 1.20.1". */
		QString gameVersionPart;
		for (const QString& mcVersion : mcVersions) {
			if (mcVersion.isEmpty()) {
				continue;
			}
			if (displayName.contains(mcVersion)) {
				gameVersionPart.clear();
				break;
			}
			if (gameVersionPart.isEmpty()) {
				gameVersionPart =
					QCoreApplication::translate("ManagedPack", " for %1")
						.arg(mcVersion);
			}
		}

		/* The separator earns its place only when something follows it.
		 *
		 * CurseForge publishes no version number and not every file
		 * carries a usable release type, which left entries reading
		 * "Some Pack — " with the dash dangling, or "Some Pack —
		 * [Release]" with a doubled space where the number would have
		 * gone. Assembling the tail first and attaching the separator to
		 * it means neither can happen. */
		QString tail = numberPart;
		if (!releaseSuffix.isEmpty()) {
			/* releaseSuffix already starts with a space, which is the
			 * gap between the number and the bracket when both are
			 * present - and must not survive when the number is not. */
			tail += tail.isEmpty() ? releaseSuffix.trimmed() : releaseSuffix;
		}

		if (tail.isEmpty()) {
			return displayName + gameVersionPart;
		}
		return QStringLiteral("%1%2 — %3")
			.arg(displayName, gameVersionPart, tail);
	}

	VersionList parseModrinthVersions(const QByteArray& bytes, bool* ok)
	{
		VersionList versions;
		if (ok != nullptr) {
			*ok = false;
		}

		const QJsonDocument doc = QJsonDocument::fromJson(bytes);
		if (!doc.isArray()) {
			return versions;
		}

		const QJsonArray array = doc.array();
		versions.reserve(array.size());

		for (const auto& entryRaw : array) {
			const QJsonObject entry = entryRaw.toObject();

			Version version;
			version.versionId = Json::ensureString(entry, "id", "");
			version.versionNumber =
				Json::ensureString(entry, "version_number", "");

			/* Modrinth lets `name` be absent or blank, in which case the
			 * version number is the only human-readable handle there
			 * is. Falling back keeps the combo box from showing a row
			 * that begins with " — ". */
			version.displayName = Json::ensureString(entry, "name", "");
			if (version.displayName.isEmpty()) {
				version.displayName = version.versionNumber;
			}

			version.releaseType = releaseTypeFromModrinth(
				Json::ensureString(entry, "version_type", ""));

			for (const auto& gameVersionRaw :
				 Json::ensureArray(entry, "game_versions")) {
				const QString gameVersion = gameVersionRaw.toString();
				if (!gameVersion.isEmpty()) {
					version.mcVersions.append(gameVersion);
				}
			}

			/* Modrinth hands the changelog over with the version list,
			 * so there is never a second request to make. Marking it
			 * loaded even when empty is the point of the flag: an empty
			 * changelog here is a fact, not a gap. */
			version.changelog = Json::ensureString(entry, "changelog", "");
			version.changelogLoaded = true;

			/* A version may ship several files (the pack plus, say, a
			 * server bundle). `primary` says which one is the pack; when
			 * there is exactly one file the flag is often omitted. */
			const QJsonArray files = Json::ensureArray(entry, "files");
			for (const auto& fileRaw : files) {
				const QJsonObject file = fileRaw.toObject();
				const bool primary =
					Json::ensureBoolean(file, QStringLiteral("primary"), false);
				if (!primary && files.size() != 1) {
					continue;
				}
				version.downloadUrl = Json::ensureString(file, "url", "");
				break;
			}

			/* An entry with no id is unusable: it cannot be matched
			 * against what is installed, nor passed to the importer. */
			if (version.versionId.isEmpty()) {
				continue;
			}
			versions.append(version);
		}

		if (ok != nullptr) {
			*ok = true;
		}
		return versions;
	}

	/* CurseForge's `gameVersions` mixes Minecraft versions with loader
	 * names and, on older projects, Java versions: ["1.20.1", "Forge",
	 * "Java 17"]. There is no type tag to separate them, so go by the
	 * only thing that reliably differs - a Minecraft version starts with
	 * a digit, a loader name does not. Being wrong here costs a slightly
	 * odd " for ..." in one combo box entry, which is why a heuristic is
	 * acceptable where it would not be for, say, launching the game. */
	static bool looksLikeMinecraftVersion(const QString& value)
	{
		if (value.isEmpty()) {
			return false;
		}
		if (!value.at(0).isDigit()) {
			return false;
		}
		/* "Java 17" fails the first-character test already; this rules
		 * out anything else that merely begins with a number. */
		return !value.contains(QLatin1Char(' '));
	}

	VersionList parseCurseForgeFiles(const QByteArray& bytes, bool* ok)
	{
		VersionList versions;
		if (ok != nullptr) {
			*ok = false;
		}

		const QJsonDocument doc = QJsonDocument::fromJson(bytes);
		if (!doc.isObject()) {
			return versions;
		}

		const QJsonArray array = Json::ensureArray(doc.object(), "data");
		versions.reserve(array.size());

		/* Sorted at the end, so remember the numeric id alongside each
		 * entry rather than re-parsing it in the comparator. */
		QVector<QPair<int, Version>> withIds;
		withIds.reserve(array.size());

		for (const auto& entryRaw : array) {
			const QJsonObject entry = entryRaw.toObject();

			const int fileId = Json::ensureInteger(entry, "id", 0);
			if (fileId == 0) {
				continue;
			}

			Version version;
			version.versionId = QString::number(fileId);

			/* `displayName` is what the CurseForge site shows; the raw
			 * `fileName` is the fallback because it is always present
			 * and at least identifies the archive. */
			version.displayName = Json::ensureString(entry, "displayName", "");
			if (version.displayName.isEmpty()) {
				version.displayName = Json::ensureString(entry, "fileName", "");
			}

			/* CurseForge has no field for a bare version number - the
			 * version is embedded in displayName - so this stays empty
			 * and label() omits that part. */

			version.releaseType = releaseTypeFromCurseForge(
				Json::ensureInteger(entry, "releaseType", 0));

			for (const auto& gameVersionRaw :
				 Json::ensureArray(entry, "gameVersions")) {
				const QString gameVersion = gameVersionRaw.toString();
				if (looksLikeMinecraftVersion(gameVersion)) {
					version.mcVersions.append(gameVersion);
				}
			}

			/* Absent for packs whose author opted out of third-party
			 * distribution. Left empty on purpose: the version is still
			 * listed, and isInstallable() is what refuses the update. */
			version.downloadUrl = Json::ensureString(entry, "downloadUrl", "");

			/* Needs a separate request per file, so nothing is loaded
			 * yet. */
			version.changelogLoaded = false;

			withIds.append({ fileId, version });
		}

		/* Newest first. CurseForge does not promise an order, and file
		 * ids are handed out in ascending order per project, so they
		 * sort by age without having to parse `fileDate`. */
		std::sort(withIds.begin(), withIds.end(),
				  [](const QPair<int, Version>& a,
					 const QPair<int, Version>& b) -> bool {
					  return a.first > b.first;
				  });

		for (const auto& entry : withIds) {
			versions.append(entry.second);
		}

		if (ok != nullptr) {
			*ok = true;
		}
		return versions;
	}

	QString parseCurseForgeChangelog(const QByteArray& bytes)
	{
		const QJsonDocument doc = QJsonDocument::fromJson(bytes);
		if (!doc.isObject()) {
			return QString();
		}
		return Json::ensureString(doc.object(), "data", "");
	}

	int indexOfVersionId(const VersionList& versions, const QString& versionId)
	{
		const QString wanted = versionId.trimmed();
		if (wanted.isEmpty()) {
			return -1;
		}
		for (int i = 0; i < versions.size(); ++i) {
			if (versions.at(i).versionId.trimmed() == wanted) {
				return i;
			}
		}
		return -1;
	}

	int indexOfVersionName(const VersionList& versions, const QString& name)
	{
		const QString wanted = name.trimmed();
		if (wanted.isEmpty()) {
			return -1;
		}
		for (int i = 0; i < versions.size(); ++i) {
			const Version& version = versions.at(i);
			if (version.versionNumber.trimmed() == wanted ||
				version.displayName.trimmed() == wanted) {
				return i;
			}
		}
		return -1;
	}

} // namespace ManagedPack
