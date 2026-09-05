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

#include "ModrinthPackIndex.h"

#include "Json.h"

void Modrinth::loadIndexedPack(Modrinth::IndexedPack& pack, QJsonObject& obj)
{
	pack.projectId = Json::ensureString(obj, "project_id", "");
	if (pack.projectId.isEmpty()) {
		pack.projectId = Json::requireString(obj, "id");
	}
	pack.slug = Json::ensureString(obj, "slug", "");
	pack.name = Json::requireString(obj, "title");
	pack.description = Json::ensureString(obj, "description", "");
	pack.author = Json::ensureString(obj, "author", "");
	pack.downloads = Json::ensureInteger(obj, "downloads", 0);

	pack.iconUrl = Json::ensureString(obj, "icon_url", "");
}

void Modrinth::loadIndexedPackVersions(Modrinth::IndexedPack& pack,
									   QJsonArray& arr)
{
	pack.versions.clear();
	for (auto versionRaw : arr) {
		auto obj = versionRaw.toObject();
		Modrinth::IndexedVersion version;
		version.id = Json::requireString(obj, "id");
		version.projectId =
			Json::ensureString(obj, "project_id", pack.projectId);
		version.name = Json::ensureString(obj, "name", "");
		version.versionNumber = Json::requireString(obj, "version_number");

		auto gameVersions = Json::ensureArray(obj, "game_versions");
		if (!gameVersions.isEmpty()) {
			version.mcVersion = gameVersions.first().toString();
		}

		auto loaders = Json::ensureArray(obj, "loaders");
		QStringList loaderList;
		for (auto loader : loaders) {
			loaderList.append(loader.toString());
		}
		version.loaders = loaderList.join(", ");

		auto files = Json::ensureArray(obj, "files");
		for (auto fileRaw : files) {
			auto fileObj = fileRaw.toObject();
			bool primary = Json::ensureBoolean(fileObj, QStringLiteral("primary"), false);
			if (primary || files.size() == 1) {
				version.downloadUrl = Json::ensureString(fileObj, "url", "");
				version.downloadSize = Json::ensureInteger(fileObj, "size", 0);
				auto hashes = Json::ensureObject(fileObj, "hashes");
				version.sha1 = Json::ensureString(hashes, "sha1", "");
				break;
			}
		}

		if (!version.downloadUrl.isEmpty()) {
			pack.versions.append(version);
		}
	}
	pack.versionsLoaded = true;
}
