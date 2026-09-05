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

#include "ModUpdateCheckTask.h"

#include <QDebug>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QUrl>

#include "Application.h"
#include "Json.h"
#include "minecraft/mod/ModMetadataIndex.h"
#include "modplatform/ContentType.h"
#include "modplatform/VersionPicker.h"
#include "modplatform/flame/FlameApi.h"
#include "modplatform/modrinth/ModrinthApi.h"
#include "net/Download.h"
#include "net/NetJob.h"

ModUpdateCheckTask::ModUpdateCheckTask(std::shared_ptr<ModMetadataIndex> index,
									   QString mcVersion, QString loader,
									   ModPlatform::ContentType contentType,
									   QObject* parent)
	: Task(parent), m_index(std::move(index)),
	  m_mcVersion(std::move(mcVersion)), m_loader(std::move(loader)),
	  m_contentType(contentType)
{
	/* A resource pack has no loader, and asking for one would filter
	 * every candidate out. The kind of content decides this, not the
	 * caller, so that every page can hand over the instance's loader
	 * without having to know whether it means anything here. */
	if (!ModPlatform::contentTypeUsesLoader(m_contentType)) {
		m_loader.clear();
	}
}

namespace
{
	/* Build the per-platform "latest matching version" endpoint URL. */
	QString buildQueryUrl(const ModMetadataIndex::Entry& e,
						  const QString& mcVersion, const QString& loader,
						  ModPlatform::ContentType contentType)
	{
		ModPlatform::VersionQuery query;
		query.projectId = e.projectId;
		query.contentType = contentType;
		query.mcVersions = ModPlatform::singleVersionList(mcVersion);
		/* Empty for content that knows nothing about loaders; the
		 * constructor has already cleared it in that case. */
		query.loaders = loader.isEmpty()
							? QStringList()
							: ModPlatform::singleLoaderList(loader);

		if (e.platform == ModrinthApi::get().id()) {
			return ModrinthApi::get().projectVersionsUrl(query).toString();
		}
		if (e.platform == FlameApi::get().id()) {
			return FlameApi::get().projectVersionsUrl(query).toString();
		}
		return {};
	}

	bool buildModrinthUpdate(const ModMetadataIndex::Entry& entry,
							 const QByteArray& body, const QString& loader,
							 ModUpdateCheckTask::UpdateInfo& out)
	{
		QJsonDocument doc = QJsonDocument::fromJson(body);
		if (!doc.isArray() || doc.array().isEmpty()) {
			return false;
		}
		/* Newest build that runs on this loader, not whichever the
		 * provider listed first: offering an update to a file that
		 * cannot load is worse than offering none. */
		const auto vObj =
			ModPlatform::newestModrinthVersion(doc.array(), loader);
		if (vObj.isEmpty()) {
			return false;
		}
		const QString newVer = Json::ensureString(vObj, "id", "");
		if (newVer.isEmpty() || newVer == entry.versionId) {
			return false;
		}

		ModPlatform::DownloadItem item;
		item.name = entry.name;
		item.platform = entry.platform;
		item.projectId = entry.projectId;
		item.versionId = newVer;
		item.isDependency = entry.isDependency;
		item.replaceExisting = true;
		item.replacesFileName = entry.fileName;

		const auto files = Json::ensureArray(vObj, "files");
		for (auto fileRaw : files) {
			const auto fObj = fileRaw.toObject();
			const bool primary = Json::ensureBoolean(fObj, QStringLiteral("primary"), false);
			if (primary || files.size() == 1) {
				item.downloadUrl = Json::ensureString(fObj, "url", "");
				item.fileName = Json::ensureString(fObj, "filename", "");
				item.fileSize = Json::ensureInteger(fObj, "size", 0);
				const auto hashes = Json::ensureObject(fObj, "hashes");
				item.sha1 = Json::ensureString(hashes, "sha1", "");
				break;
			}
		}
		if (item.downloadUrl.isEmpty() || item.fileName.isEmpty()) {
			return false;
		}

		/* The same bytes we already have, under a version id we never
		 * recorded.
		 *
		 * Comparing ids alone is not enough: a file installed from an
		 * mrpack has no version id on record - that format lists hashes
		 * and URLs, nothing else - so every such mod compared unequal to
		 * the newest version and was offered as an update to itself,
		 * every time the check ran. The hash settles it. */
		if (!entry.sha1.isEmpty() && !item.sha1.isEmpty() &&
			item.sha1.compare(entry.sha1, Qt::CaseInsensitive) == 0) {
			return false;
		}

		out.currentFileName = entry.fileName;
		out.currentVersionId = entry.versionId;
		out.newVersionId = newVer;
		out.name = entry.name;
		out.platform = entry.platform;
		out.item = item;
		return true;
	}

	bool buildCurseForgeUpdate(const ModMetadataIndex::Entry& entry,
							   const QByteArray& body, const QString& loader,
							   ModUpdateCheckTask::UpdateInfo& out)
	{
		QJsonDocument doc = QJsonDocument::fromJson(body);
		QJsonArray arr;
		if (doc.isObject() && doc.object().contains("data")) {
			const auto v = doc.object().value("data");
			if (v.isArray()) {
				arr = v.toArray();
			}
		}
		if (arr.isEmpty()) {
			return false;
		}
		const auto fObj = ModPlatform::newestCurseForgeFile(arr, loader);
		if (fObj.isEmpty()) {
			return false;
		}
		const QString newVer =
			QString::number(Json::ensureInteger(fObj, "id", 0));
		if (newVer.isEmpty() || newVer == QStringLiteral("0") ||
			newVer == entry.versionId) {
			return false;
		}

		ModPlatform::DownloadItem item;
		item.name = Json::ensureString(fObj, "displayName", entry.name);
		item.platform = entry.platform;
		item.projectId = entry.projectId;
		item.versionId = newVer;
		item.fileName = Json::ensureString(fObj, "fileName", "");
		item.downloadUrl = Json::ensureString(fObj, "downloadUrl", "");
		item.fileSize = Json::ensureInteger(fObj, "fileLength", 0);
		item.sha1 = ModPlatform::curseForgeSha1FromFileObject(fObj);
		item.isDependency = entry.isDependency;
		item.replaceExisting = true;
		item.replacesFileName = entry.fileName;

		if (item.downloadUrl.isEmpty()) {
			const int fileId = Json::ensureInteger(fObj, "id", 0);
			if (fileId > 0 && !item.fileName.isEmpty()) {
				item.downloadUrl = FlameApi::browserDownloadUrl(
					entry.projectId, QString::number(fileId));
			}
		}
		if (item.downloadUrl.isEmpty() || item.fileName.isEmpty()) {
			return false;
		}

		out.currentFileName = entry.fileName;
		out.currentVersionId = entry.versionId;
		out.newVersionId = newVer;
		out.name = entry.name;
		out.platform = entry.platform;
		out.item = item;
		return true;
	}
} // namespace

void ModUpdateCheckTask::executeTask()
{
	if (!m_index) {
		emitSucceeded();
		return;
	}

	const auto entries = m_index->all();
	for (const auto& e : entries) {
		if (!e.hasPlatformOrigin()) {
			continue;
		}
		if (e.platform != QStringLiteral("modrinth") &&
			e.platform != QStringLiteral("curseforge")) {
			continue;
		}
		m_total++;
	}

	if (m_total == 0) {
		setStatus(tr("No tracked mods to check."));
		emitSucceeded();
		return;
	}

	setStatus(tr("Checking %1 mod(s) for updates...").arg(m_total));

	for (const auto& e : entries) {
		if (!e.hasPlatformOrigin()) {
			continue;
		}
		if (e.platform != QStringLiteral("modrinth") &&
			e.platform != QStringLiteral("curseforge")) {
			continue;
		}

		const QString url =
			buildQueryUrl(e, m_mcVersion, m_loader, m_contentType);
		if (url.isEmpty()) {
			continue;
		}

		/* Held by both handlers rather than freed by hand in each: only
		 * one of them runs, and if the job dies without either firing
		 * the buffer goes with it instead of leaking. */
		auto response = std::make_shared<QByteArray>();
		NetJob* job = new NetJob(
			QString("UpdateCheck(%1:%2)").arg(e.platform, e.projectId),
			APPLICATION->network());
		job->addNetAction(
			Net::Download::makeByteArray(QUrl(url), response.get()));

		const ModMetadataIndex::Entry entry = e;
		m_pending++;
		m_activeJobs.append(job);

		connect(job, &NetJob::succeeded, this, [this, entry, response, job]() {
			m_activeJobs.removeAll(QPointer<NetJob>(job));
			job->deleteLater();
			if (m_aborted) {
				/* Given up on. The count is still kept honest, but no
				 * verdict follows - see onOneDone(). */
				m_pending--;
				return;
			}
			UpdateInfo u;
			bool found = false;
			if (entry.platform == QStringLiteral("modrinth")) {
				found = buildModrinthUpdate(entry, *response, m_loader, u);
			} else if (entry.platform == QStringLiteral("curseforge")) {
				found = buildCurseForgeUpdate(entry, *response, m_loader, u);
			}
			if (found) {
				m_updates.append(u);
			}
			onOneDone();
		});
		connect(job, &NetJob::failed, this,
				[this, response, job, entry](QString reason) {
					m_activeJobs.removeAll(QPointer<NetJob>(job));
					job->deleteLater();
					if (m_aborted) {
						m_pending--;
						return;
					}
					qWarning() << "Update check failed for" << entry.name << ":"
							   << reason;
					onOneDone();
				});
		// Show the check as its own line in the progress dialog.
		propagateStepsFrom(job);
		job->start();
	}
}

bool ModUpdateCheckTask::abort()
{
	if (m_aborted) {
		return true;
	}
	m_aborted = true;

	/* Call off what is still in the air. Each job reports failed() as it
	 * unwinds, which the handlers above drop because the latch is
	 * already set. */
	for (const QPointer<NetJob>& job : m_activeJobs) {
		if (job && job->isRunning()) {
			job->abort();
		}
	}
	m_activeJobs.clear();

	if (isRunning()) {
		emitAborted();
	}
	return true;
}

void ModUpdateCheckTask::onOneDone()
{
	if (m_aborted) {
		return;
	}

	m_completed++;
	m_pending--;
	setProgress(m_completed, m_total);
	if (m_pending <= 0) {
		qDebug() << "ModUpdateCheckTask:" << m_updates.size()
				 << "update(s) found across" << m_total << "mod(s)";
		emitSucceeded();
	}
}
