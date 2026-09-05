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

#include "DependencyResolver.h"
#include "Application.h"
#include "Json.h"
#include "minecraft/mod/ModMetadataIndex.h"
#include "modplatform/ContentType.h"
#include "modplatform/VersionPicker.h"
#include "modplatform/flame/FlameApi.h"
#include "modplatform/modrinth/ModrinthApi.h"
#include "net/Download.h"
#include "net/NetJob.h"

#include <QJsonDocument>
#include <QJsonArray>
#include <QJsonObject>
#include <QRegularExpression>
#include <QUrl>

namespace
{

	/* CurseForge's releaseType as the name the review dialog and the
	 * release channel filter use. Modrinth states this outright, in a
	 * "version_type" field, so only this side needs translating. */
	QString curseForgeReleaseTypeName(int releaseType)
	{
		switch (releaseType) {
			case 1:
				return QStringLiteral("release");
			case 2:
				return QStringLiteral("beta");
			case 3:
				return QStringLiteral("alpha");
			default:
				return QString();
		}
	}

} // namespace

DependencyResolver::DependencyResolver(
	const QList<ModPlatform::SelectedMod>& selectedMods,
	const QString& mcVersion, const QString& loader, QObject* parent)
	: Task(parent), m_selectedMods(selectedMods), m_mcVersion(mcVersion),
	  m_loader(loader)
{
	// pre-populate resolved set so we don't fetch deps for things we're already
	// installing
	for (const auto& mod : m_selectedMods) {
		m_resolvedProjectIds.insert(mod.platform + ":" + mod.projectId);
		m_resolvedNames.insert(normalizeName(mod.name));
	}
}

void DependencyResolver::setInstalledIndex(
	std::shared_ptr<ModMetadataIndex> index)
{
	m_installed = std::move(index);

	/* Deliberately not used to pre-populate the resolved sets any more.
	 * Treating everything on disk as already dealt with saved a lookup
	 * per installed dependency, but it also meant a library that is
	 * present at the wrong version - built for another Minecraft
	 * version, or left behind damaged - was silently declared fine and
	 * never mentioned again.
	 *
	 * What happens instead, and what the reference launcher does: the
	 * dependency is looked up either way, dropped when the very version
	 * it asks for is the one on disk, and otherwise offered to the user
	 * unticked. See acceptDependency(). */
}

bool DependencyResolver::isVersionInstalled(const QString& platform,
											const QString& projectId,
											const QString& versionId) const
{
	if (!m_installed || platform.isEmpty() || projectId.isEmpty() ||
		versionId.isEmpty()) {
		return false;
	}
	const auto entry = m_installed->findByPlatformProject(platform, projectId);
	return entry.isValid() && entry.versionId == versionId;
}

bool DependencyResolver::isProjectInstalled(const QString& platform,
											const QString& projectId,
											const QString& name) const
{
	if (!m_installed) {
		return false;
	}
	if (!platform.isEmpty() && !projectId.isEmpty() &&
		m_installed->findByPlatformProject(platform, projectId).isValid()) {
		return true;
	}
	/* Also by name, which catches the same library installed from the
	 * other site. */
	const QString normalized = normalizeName(name);
	return !normalized.isEmpty() &&
		   m_installed->findByNormalizedName(normalized).isValid();
}

void DependencyResolver::acceptDependency(ModPlatform::DependencyInfo dep)
{
	if (dep.downloadUrl.isEmpty()) {
		qWarning() << "Dependency" << dep.name << "on" << dep.platform
				   << "has no download URL, skipping";
		return;
	}

	if (isVersionInstalled(dep.platform, dep.projectId, dep.versionId)) {
		/* The exact file it asks for is the one already on disk. There
		 * is nothing to do and nothing worth saying about it. */
		qDebug() << "Dependency" << dep.name
				 << "is already installed at this version, dropping";
		return;
	}

	/* Present, but not as this version. Offered rather than forced: the
	 * review dialog shows the row unticked. */
	dep.maybeInstalled =
		isProjectInstalled(dep.platform, dep.projectId, dep.name);

	m_resolvedNames.insert(normalizeName(dep.name));
	m_dependencies.append(dep);
}

void DependencyResolver::noteAlsoRequiredBy(const QString& platform,
											const QString& projectId,
											const QStringList& requiredBy)
{
	if (requiredBy.isEmpty()) {
		return;
	}
	for (auto& dep : m_dependencies) {
		if (dep.platform == platform && dep.projectId == projectId) {
			for (const QString& name : requiredBy) {
				if (!name.isEmpty() && !dep.requiredBy.contains(name)) {
					dep.requiredBy.append(name);
				}
			}
			return;
		}
	}
}

QString DependencyResolver::normalizeName(const QString& name)
{
	/* One implementation, in the index.
	 *
	 * There were two, spelled out separately here and there, and they
	 * were meant to agree: the resolver decides whether a dependency is
	 * the same thing as something already installed, and the index
	 * decides what "already installed" looks up as. Two copies of that
	 * rule is one copy too many - they only have to disagree by a
	 * bracket for a mod to be resolved and installed twice. */
	return ModMetadataIndex::normalizeName(name);
}

void DependencyResolver::executeTask()
{
	if (m_selectedMods.isEmpty()) {
		emitSucceeded();
		return;
	}
	setStatus(tr("Resolving dependencies..."));
	m_currentModIndex = 0;
	resolveNextMod();
}

bool DependencyResolver::abort()
{
	if (m_aborted) {
		return true;
	}
	m_aborted = true;

	/* Call off what is still in the air. Each job reports failed() as it
	 * unwinds, which lands in the handler below and is dropped because
	 * m_aborted is already set. */
	for (const QPointer<NetJob>& job : m_activeJobs) {
		if (job && job->isRunning()) {
			job->abort();
		}
	}
	m_activeJobs.clear();

	/* Only a running task may report a verdict; emitAborted() complains
	 * loudly otherwise. The latch above holds either way. */
	if (isRunning()) {
		emitAborted();
	}
	return true;
}

void DependencyResolver::request(const QString& name, const QUrl& url,
								 std::function<void(const QByteArray&)> onDone)
{
	if (m_aborted) {
		return;
	}

	/* Held by both handlers rather than freed by hand in each: only one
	 * of them runs, and if the job dies without either firing the buffer
	 * goes with it instead of leaking. */
	auto response = std::make_shared<QByteArray>();
	auto* job = new NetJob(name, APPLICATION->network());
	job->addNetAction(Net::Download::makeByteArray(url, response.get()));

	m_pendingRequests++;
	m_activeJobs.append(job);

	auto finish = [this, job](const std::function<void(const QByteArray&)>& fn,
							  const QByteArray& bytes) {
		m_activeJobs.removeAll(QPointer<NetJob>(job));
		job->deleteLater();

		if (m_aborted) {
			/* Still decremented: the count has to end up honest even
			 * when nobody is going to look at it again. */
			m_pendingRequests--;
			return;
		}

		fn(bytes);
		m_pendingRequests--;
		checkCompletion();
	};

	connect(job, &NetJob::succeeded, this,
			[finish, onDone, response] { finish(onDone, *response); });
	connect(job, &NetJob::failed, this,
			[finish, onDone, response, name](QString reason) {
				qWarning() << "Lookup failed:" << name << ":" << reason;
				/* An empty reply, so the handler takes its "could not
				 * resolve" path rather than being skipped entirely and
				 * leaving the dependency silently unaccounted for. */
				finish(onDone, QByteArray());
			});

	// Show the lookup as its own line in the progress dialog.
	propagateStepsFrom(job);
	job->start();
}

void DependencyResolver::resolveNextMod()
{
	if (m_aborted) {
		return;
	}

	if (m_currentModIndex >= m_selectedMods.size()) {
		checkCompletion();
		return;
	}

	const auto& mod = m_selectedMods[m_currentModIndex];
	if (mod.platform == "curseforge") {
		resolveCurseForgeDependencies(mod);
	} else if (mod.platform == "modrinth") {
		resolveModrinthDependencies(mod);
	} else {
		m_currentModIndex++;
		resolveNextMod();
	}
}

void DependencyResolver::resolveCurseForgeDependencies(
	const ModPlatform::SelectedMod& mod)
{
	if (mod.versionId.isEmpty()) {
		m_currentModIndex++;
		resolveNextMod();
		return;
	}

	const auto currentMod = mod;
	request(QString("CF::DepResolve(%1)").arg(mod.name),
			FlameApi::fileUrl(mod.projectId, mod.versionId),
			[this, currentMod](const QByteArray& bytes) {
				if (!bytes.isEmpty()) {
					onCurseForgeVersionResolved(currentMod, bytes);
				}
				/* On to the next mod either way: one lookup that came
				 * back empty must not stall the whole queue. */
				m_currentModIndex++;
				resolveNextMod();
			});
}

void DependencyResolver::resolveModrinthDependencies(
	const ModPlatform::SelectedMod& mod)
{
	/* Two ways to name the version whose dependencies we want, and the
	 * shape of what we were given decides which.
	 *
	 * A version id addresses the version directly. But plenty of what we
	 * have on record is not an id at all: a file installed from an mrpack
	 * has only its hash and its download URL, and the URL spells out the
	 * version *number* ("1.1.1+1.17"). Handed to the version endpoint
	 * that produces a 404, which looks exactly like a mod with no
	 * dependencies - the failure this is here to stop being silent.
	 *
	 * The hash is the way back in that case: it names one file, and the
	 * endpoint answers with the same version object. */
	QUrl lookup;
	if (ModrinthApi::isVersionId(mod.versionId)) {
		lookup = ModrinthApi::versionUrl(mod.versionId);
	} else if (!mod.sha1.isEmpty()) {
		lookup = ModrinthApi::versionByHashUrl(mod.sha1);
	} else {
		if (!mod.versionId.isEmpty()) {
			qWarning() << "Not resolving dependencies for" << mod.name
					   << ": recorded version" << mod.versionId
					   << "is not a Modrinth version id and no file hash "
						  "is known";
		}
		m_currentModIndex++;
		resolveNextMod();
		return;
	}

	const auto currentMod = mod;
	request(QString("MR::DepResolve(%1)").arg(mod.name), lookup,
			[this, currentMod](const QByteArray& bytes) {
				if (!bytes.isEmpty()) {
					onModrinthVersionResolved(currentMod, bytes);
				}
			});

	/* Unlike the CurseForge path this does not wait for the answer: the
	 * next mod is started straight away and the replies are collected as
	 * they come. */
	m_currentModIndex++;
	resolveNextMod();
}

void DependencyResolver::onCurseForgeVersionResolved(
	const ModPlatform::SelectedMod& mod, const QByteArray& data)
{
	QJsonParseError parseError;
	QJsonDocument doc = QJsonDocument::fromJson(data, &parseError);
	if (parseError.error != QJsonParseError::NoError)
		return;

	QJsonObject fileObj;
	if (doc.isObject() && doc.object().contains("data")) {
		fileObj = doc.object().value("data").toObject();
	} else {
		fileObj = doc.object();
	}

	processCFFileDeps(fileObj, {mod.name});
}

void DependencyResolver::onModrinthVersionResolved(
	const ModPlatform::SelectedMod& mod, const QByteArray& data)
{
	QJsonParseError parseError;
	QJsonDocument doc = QJsonDocument::fromJson(data, &parseError);
	if (parseError.error != QJsonParseError::NoError)
		return;

	processMRVersionDeps(doc.object(), {mod.name});
}

void DependencyResolver::onDependencyProjectResolved(
	const QString& platform, const QString& projectId, const QByteArray& data,
	const QStringList& requiredBy)
{
	QJsonParseError parseError;
	QJsonDocument doc = QJsonDocument::fromJson(data, &parseError);
	if (parseError.error != QJsonParseError::NoError)
		return;

	if (platform == "curseforge") {
		// Response is from /v1/mods/{id}/files endpoint
		QJsonArray filesArr;
		if (doc.isObject() && doc.object().contains("data")) {
			auto dataVal = doc.object().value("data");
			if (dataVal.isArray()) {
				filesArr = dataVal.toArray();
			} else if (dataVal.isObject()) {
				filesArr.append(dataVal);
			}
		}

		if (filesArr.isEmpty()) {
			qWarning() << "No compatible files found for CurseForge dependency"
					   << projectId << "- attempting cross resolve to Modrinth";
			crossResolveFromCurseForge(projectId, requiredBy);
			return;
		}

		/* Newest file that can run on this loader, rather than whatever
		 * CurseForge happens to list first - it will gladly return a
		 * Forge build among the matches for a Fabric instance. */
		const auto fileObj =
			ModPlatform::newestCurseForgeFile(filesArr, m_loader);
		if (fileObj.isEmpty()) {
			qWarning() << "No file for loader" << m_loader
					   << "among CurseForge dependency" << projectId
					   << "files - attempting cross resolve to Modrinth";
			crossResolveFromCurseForge(projectId, requiredBy);
			return;
		}

		ModPlatform::DependencyInfo dep;
		dep.projectId = projectId;
		dep.platform = "curseforge";
		dep.isRequired = true;
		dep.requiredBy = requiredBy;
		dep.versionId = QString::number(Json::ensureInteger(fileObj, "id", 0));
		dep.fileName = Json::ensureString(fileObj, "fileName", "");
		dep.downloadUrl = Json::ensureString(fileObj, "downloadUrl", "");
		dep.fileSize = Json::ensureInteger(fileObj, "fileLength", 0);
		dep.sha1 = ModPlatform::curseForgeSha1FromFileObject(fileObj);
		dep.name = Json::ensureString(fileObj, "displayName", dep.fileName);
		dep.versionType = curseForgeReleaseTypeName(
			Json::ensureInteger(fileObj, "releaseType", 0));

		// Handle restricted downloads (downloadUrl is null)
		if (dep.downloadUrl.isEmpty()) {
			int fileId = Json::ensureInteger(fileObj, "id", 0);
			if (fileId > 0 && !dep.fileName.isEmpty()) {
				dep.downloadUrl = FlameApi::browserDownloadUrl(
					projectId, QString::number(fileId));
				dep.browserDownloadOnly = true;
				qWarning() << "CurseForge dependency" << projectId
						   << "has restricted download, using fallback URL";
			}
		}

		if (dep.downloadUrl.isEmpty()) {
			/* Nothing to fetch, so nothing this file needs is worth
			 * chasing either. */
			qWarning() << "CurseForge dependency" << projectId
					   << "has no download URL, skipping";
			return;
		}

		qDebug() << "Resolved CurseForge dependency:" << dep.name
				 << "file:" << dep.fileName << "url:" << dep.downloadUrl;
		const QString depName = dep.name;
		acceptDependency(dep);
		/* Into its own dependencies, which are now required by it. */
		processCFFileDeps(fileObj, {depName});
	} else if (platform == "modrinth") {
		// data is an array of versions
		QJsonArray arr;
		if (doc.isArray()) {
			arr = doc.array();
		} else {
			return;
		}

		if (arr.isEmpty()) {
			qWarning() << "No compatible files found for Modrinth dependency"
					   << projectId
					   << "- attempting cross resolve to CurseForge";
			crossResolveFromModrinth(projectId, requiredBy);
			return;
		}

		/* Newest version that lists this loader; see the CurseForge
		 * branch above for why the order is not trusted. */
		const auto vObj = ModPlatform::newestModrinthVersion(arr, m_loader);
		if (vObj.isEmpty()) {
			qWarning() << "No version for loader" << m_loader
					   << "among Modrinth dependency" << projectId
					   << "versions - attempting cross resolve to CurseForge";
			crossResolveFromModrinth(projectId, requiredBy);
			return;
		}

		ModPlatform::DependencyInfo dep;
		dep.projectId = projectId;
		dep.versionId = Json::ensureString(vObj, "id", "");
		dep.name = Json::ensureString(vObj, "name", projectId);
		dep.platform = "modrinth";
		dep.isRequired = true;
		dep.requiredBy = requiredBy;
		dep.versionType = Json::ensureString(vObj, "version_type", "");

		auto files = Json::ensureArray(vObj, "files");
		for (auto fileRaw : files) {
			auto fileObj = fileRaw.toObject();
			bool primary = Json::ensureBoolean(fileObj, QStringLiteral("primary"), false);
			if (primary || files.size() == 1) {
				dep.downloadUrl = Json::ensureString(fileObj, "url", "");
				dep.fileName = Json::ensureString(fileObj, "filename", "");
				dep.fileSize = Json::ensureInteger(fileObj, "size", 0);
				auto hashes = Json::ensureObject(fileObj, "hashes");
				dep.sha1 = Json::ensureString(hashes, "sha1", "");
				break;
			}
		}

		if (dep.downloadUrl.isEmpty()) {
			qWarning() << "Modrinth dependency" << projectId
					   << "has no downloadable file, skipping";
			return;
		}

		const QString depName = dep.name;
		acceptDependency(dep);
		processMRVersionDeps(vObj, {depName});
	}
}

void DependencyResolver::processCFFileDeps(const QJsonObject& fileObj,
										   const QStringList& requiredBy)
{
	auto deps = Json::ensureArray(fileObj, "dependencies");
	for (auto depRaw : deps) {
		auto depObj = depRaw.toObject();
		int relationType = Json::ensureInteger(depObj, "relationType", 0);

		int modId = Json::ensureInteger(depObj, "modId", 0);
		if (modId == 0)
			continue;

		QString depProjectId = QString::number(modId);
		QString key = "curseforge:" + depProjectId;

		// Track embedded/included deps so they won't be re-downloaded from
		// other mods
		if (relationType == 1 ||
			relationType == 6) { // 1=EmbeddedLibrary, 6=Include
			m_resolvedProjectIds.insert(key);
			continue;
		}

		if (relationType != 3) {
			/* 2 = Optional, 4 = Tool, 5 = Incompatible. Deliberately not
			 * followed: the reference launcher only walks required
			 * dependencies too, and pulling in everything a mod merely
			 * suggests would quietly install mods nobody asked for. */
			continue;
		}

		if (m_resolvedProjectIds.contains(key)) {
			/* Already on its way in for something else - that something
			 * just joins the list of things asking for it. */
			noteAlsoRequiredBy(QStringLiteral("curseforge"), depProjectId,
							   requiredBy);
			continue;
		}
		m_resolvedProjectIds.insert(key);

		ModPlatform::VersionQuery cfQuery;
		cfQuery.projectId = depProjectId;
		cfQuery.contentType = ModPlatform::ContentType::Mod;
		cfQuery.mcVersions = ModPlatform::singleVersionList(m_mcVersion);
		cfQuery.loaders = ModPlatform::singleLoaderList(m_loader);
		const auto url = FlameApi::get().projectVersionsUrl(cfQuery);
		qDebug() << "Fetching CurseForge dependency files (recursive):"
				 << url.toString();

		request(QString("CF::DepFetch(%1)").arg(depProjectId), url,
				[this, depProjectId, requiredBy](const QByteArray& bytes) {
					if (!bytes.isEmpty()) {
						onDependencyProjectResolved("curseforge", depProjectId,
													bytes, requiredBy);
					}
				});
	}
}

void DependencyResolver::processMRVersionDeps(const QJsonObject& versionObj,
											  const QStringList& requiredBy)
{
	auto deps = Json::ensureArray(versionObj, "dependencies");
	for (auto depRaw : deps) {
		auto depObj = depRaw.toObject();
		QString depType = Json::ensureString(depObj, "dependency_type", "");

		QString depProjectId = Json::ensureString(depObj, "project_id", "");
		QString depVersionId = Json::ensureString(depObj, "version_id", "");
		if (depProjectId.isEmpty())
			continue;

		QString key = "modrinth:" + depProjectId;

		// Track embedded deps so they won't be re-downloaded from other mods
		if (depType == "embedded") {
			m_resolvedProjectIds.insert(key);
			continue;
		}

		if (depType != "required") {
			/* "optional" and "incompatible" are deliberately not
			 * followed - see the CurseForge side for why. */
			continue;
		}

		if (m_resolvedProjectIds.contains(key)) {
			noteAlsoRequiredBy(QStringLiteral("modrinth"), depProjectId,
							   requiredBy);
			continue;
		}
		m_resolvedProjectIds.insert(key);

		if (!depVersionId.isEmpty()) {
			request(
				QString("MR::DepVersion(%1)").arg(depProjectId),
				ModrinthApi::versionUrl(depVersionId),
				[this, depProjectId, depVersionId,
				 requiredBy](const QByteArray& bytes) {
					if (bytes.isEmpty()) {
						/* The lookup itself failed. Not retried through
						 * the project endpoint below: on a systematic
						 * failure that turns one dead request into two
						 * for every dependency in the tree. */
						return;
					}

					const auto vObj = QJsonDocument::fromJson(bytes).object();

					/* The dependency names one exact version, but that
					 * version may predate the Minecraft version this
					 * instance runs. */
					bool compatible = false;
					for (auto v : Json::ensureArray(vObj, "game_versions")) {
						if (v.toString() == m_mcVersion) {
							compatible = true;
							break;
						}
					}

					if (!compatible) {
						qWarning()
							<< "Modrinth dep version" << depVersionId
							<< "is not compatible with MC" << m_mcVersion
							<< "- falling back to project version search";

						/* Ask the project as a whole for something that
						 * does fit. The pending count needs no nudging:
						 * this lookup still counts until we return, and
						 * the new one has already been counted. */
						ModPlatform::VersionQuery fbQuery;
						fbQuery.projectId = depProjectId;
						fbQuery.contentType = ModPlatform::ContentType::Mod;
						fbQuery.mcVersions =
							ModPlatform::singleVersionList(m_mcVersion);
						fbQuery.loaders =
							ModPlatform::singleLoaderList(m_loader);

						request(
							QString("MR::DepProject(%1)").arg(depProjectId),
							ModrinthApi::get().projectVersionsUrl(fbQuery),
							[this, depProjectId,
							 requiredBy](const QByteArray& fb) {
								if (!fb.isEmpty()) {
									onDependencyProjectResolved(
										"modrinth", depProjectId, fb,
										requiredBy);
								}
							});
						return;
					}

					ModPlatform::DependencyInfo dep;
					dep.projectId = depProjectId;
					dep.versionId = Json::ensureString(vObj, "id", "");
					dep.name = Json::ensureString(vObj, "name", depProjectId);
					dep.platform = "modrinth";
					dep.isRequired = true;
					dep.requiredBy = requiredBy;
					dep.versionType =
						Json::ensureString(vObj, "version_type", "");

					auto files = Json::ensureArray(vObj, "files");
					for (auto fileRaw : files) {
						auto fileObj = fileRaw.toObject();
						bool primary =
							Json::ensureBoolean(fileObj, QStringLiteral("primary"), false);
						if (primary || files.size() == 1) {
							dep.downloadUrl =
								Json::ensureString(fileObj, "url", "");
							dep.fileName =
								Json::ensureString(fileObj, "filename", "");
							dep.fileSize =
								Json::ensureInteger(fileObj, "size", 0);
							auto hashes = Json::ensureObject(fileObj, "hashes");
							dep.sha1 = Json::ensureString(hashes, "sha1", "");
							break;
						}
					}

					if (dep.downloadUrl.isEmpty()) {
						qWarning() << "Modrinth dep version" << depVersionId
								   << "has no downloadable file, skipping";
						return;
					}

					const QString depName = dep.name;
					acceptDependency(dep);
					processMRVersionDeps(vObj, {depName});
				});
		} else {
			ModPlatform::VersionQuery mrQuery;
			mrQuery.projectId = depProjectId;
			mrQuery.contentType = ModPlatform::ContentType::Mod;
			mrQuery.mcVersions = ModPlatform::singleVersionList(m_mcVersion);
			mrQuery.loaders = ModPlatform::singleLoaderList(m_loader);

			request(QString("MR::DepProject(%1)").arg(depProjectId),
					ModrinthApi::get().projectVersionsUrl(mrQuery),
					[this, depProjectId, requiredBy](const QByteArray& bytes) {
						if (!bytes.isEmpty()) {
							onDependencyProjectResolved(
								"modrinth", depProjectId, bytes, requiredBy);
						}
					});
		}
	}
}

void DependencyResolver::crossResolveFromCurseForge(
	const QString& projectId, const QStringList& requiredBy)
{
	request(QString("CF::FetchName(%1)").arg(projectId),
			FlameApi::get().projectUrl(projectId),
			[this, projectId, requiredBy](const QByteArray& bytes) {
				const QJsonDocument doc = QJsonDocument::fromJson(bytes);

				QString name;
				QString slug;
				if (doc.isObject() && doc.object().contains("data")) {
					const auto dataObj = doc.object().value("data").toObject();
					name = Json::ensureString(dataObj, "name", "");
					slug = Json::ensureString(dataObj, "slug", "");
				}
				if (!name.isEmpty()) {
					qDebug() << "CF cross resolve extracted name:" << name
							 << "slug:" << slug << "for" << projectId;
					executeCrossResolve("modrinth", name, slug, requiredBy);
				}
			});
}

void DependencyResolver::crossResolveFromModrinth(
	const QString& projectId, const QStringList& requiredBy)
{
	request(QString("MR::FetchName(%1)").arg(projectId),
			ModrinthApi::get().projectUrl(projectId),
			[this, projectId, requiredBy](const QByteArray& bytes) {
				const QJsonDocument doc = QJsonDocument::fromJson(bytes);

				QString name;
				QString slug;
				if (doc.isObject()) {
					name = Json::ensureString(doc.object(), "title", "");
					slug = Json::ensureString(doc.object(), "slug", "");
				}
				if (!name.isEmpty()) {
					qDebug() << "MR cross resolve extracted title:" << name
							 << "slug:" << slug << "for" << projectId;
					executeCrossResolve("curseforge", name, slug, requiredBy);
				}
			});
}

void DependencyResolver::executeCrossResolve(const QString& targetPlatform,
											 const QString& projectName,
											 const QString& sourceSlug,
											 const QStringList& requiredBy)
{
	// Check if we already resolved something with this name (cross-platform
	// dedup)
	QString normalized = normalizeName(projectName);
	if (m_resolvedNames.contains(normalized)) {
		qDebug() << "Cross resolve skipped - already resolved by name:"
				 << projectName;
		return;
	}

	const QString query = QUrl::toPercentEncoding(projectName);
	const QUrl url = (targetPlatform == "modrinth")
						 ? ModrinthApi::nameSearchUrl(query)
						 : FlameApi::nameSearchUrl(query);

	request(
		QString("CrossSearch(%1)").arg(projectName), url,
		[this, targetPlatform, projectName, sourceSlug,
		 requiredBy](const QByteArray& bytes) {
			if (bytes.isEmpty()) {
				/* The search itself did not come back. The dependency is
				 * no less unresolved for it, and saying so beats
				 * dropping it without a word. */
				qWarning() << "Cross resolve search failed for" << projectName;
				ModPlatform::UnresolvedDep unresolved;
				unresolved.name = projectName;
				m_unresolvedDeps.append(unresolved);
				return;
			}

			const QJsonDocument doc = QJsonDocument::fromJson(bytes);

			// Extract multiple hits and find the best match using slug
			// comparison
			QString hitId;
			QString hitSlug;
			QString hitName;

			if (targetPlatform == "modrinth") {
				if (doc.isObject() && doc.object().contains("hits")) {
					auto hits = Json::ensureArray(doc.object(), "hits");
					for (auto hitRaw : hits) {
						auto hitObj = hitRaw.toObject();
						QString candidateSlug =
							Json::ensureString(hitObj, "slug", "");
						QString candidateId =
							Json::ensureString(hitObj, "project_id", "");
						QString candidateName =
							Json::ensureString(hitObj, "title", "");

						// Prefer exact slug match
						if (!sourceSlug.isEmpty() &&
							candidateSlug.compare(sourceSlug,
												  Qt::CaseInsensitive) == 0) {
							hitId = candidateId;
							hitSlug = candidateSlug;
							hitName = candidateName;
							break;
						}
						// Accept normalized name match
						if (normalizeName(candidateName) ==
							normalizeName(projectName)) {
							hitId = candidateId;
							hitSlug = candidateSlug;
							hitName = candidateName;
							break;
						}
						// Keep first result as fallback only if no slug was
						// provided
						if (hitId.isEmpty() && sourceSlug.isEmpty()) {
							hitId = candidateId;
							hitSlug = candidateSlug;
							hitName = candidateName;
						}
					}
				}
			} else {
				if (doc.isObject() && doc.object().contains("data")) {
					auto dataArr = Json::ensureArray(doc.object(), "data");
					for (auto hitRaw : dataArr) {
						auto hitObj = hitRaw.toObject();
						QString candidateSlug =
							Json::ensureString(hitObj, "slug", "");
						QString candidateId = QString::number(
							Json::ensureInteger(hitObj, "id", 0));
						QString candidateName =
							Json::ensureString(hitObj, "name", "");

						if (!sourceSlug.isEmpty() &&
							candidateSlug.compare(sourceSlug,
												  Qt::CaseInsensitive) == 0) {
							hitId = candidateId;
							hitSlug = candidateSlug;
							hitName = candidateName;
							break;
						}
						if (normalizeName(candidateName) ==
							normalizeName(projectName)) {
							hitId = candidateId;
							hitSlug = candidateSlug;
							hitName = candidateName;
							break;
						}
						if (hitId.isEmpty() && sourceSlug.isEmpty()) {
							hitId = candidateId;
							hitSlug = candidateSlug;
							hitName = candidateName;
						}
					}
				}
			}

			// Validate the match quality
			if (!hitId.isEmpty() && !sourceSlug.isEmpty() &&
				hitSlug.isEmpty()) {
				// We had a source slug but couldn't find a slug match — reject
				// ambiguous result
				qWarning() << "Cross resolve rejected ambiguous match for"
						   << projectName << "- source slug:" << sourceSlug
						   << "didn't match any result";
				hitId.clear();
			}

			if (!hitId.isEmpty()) {
				QString key = targetPlatform + ":" + hitId;
				if (!m_resolvedProjectIds.contains(key)) {
					m_resolvedProjectIds.insert(key);
					m_resolvedNames.insert(normalizeName(projectName));
					qDebug() << "Cross resolved to" << targetPlatform
							 << "project" << hitId << "slug:" << hitSlug
							 << "(source slug:" << sourceSlug << ")";

					ModPlatform::VersionQuery crossQuery;
					crossQuery.projectId = hitId;
					crossQuery.contentType = ModPlatform::ContentType::Mod;
					crossQuery.mcVersions =
						ModPlatform::singleVersionList(m_mcVersion);
					crossQuery.loaders =
						ModPlatform::singleLoaderList(m_loader);
					const QUrl fileUrl =
						(targetPlatform == "modrinth")
							? ModrinthApi::get().projectVersionsUrl(crossQuery)
							: FlameApi::get().projectVersionsUrl(crossQuery);

					request(QString("CrossDepResolve(%1)").arg(hitId), fileUrl,
							[this, targetPlatform, hitId, projectName,
							 requiredBy](const QByteArray& depBytes) {
								if (depBytes.isEmpty()) {
									ModPlatform::UnresolvedDep unresolved;
									unresolved.name = projectName;
									m_unresolvedDeps.append(unresolved);
									return;
								}
								onDependencyProjectResolved(
									targetPlatform, hitId, depBytes,
									requiredBy);
							});
				} else {
					noteAlsoRequiredBy(targetPlatform, hitId, requiredBy);
					bool wasResolved = false;
					for (const auto& dep : m_dependencies) {
						if ((dep.platform + ":" + dep.projectId) == key) {
							wasResolved = true;
							break;
						}
					}
					if (!wasResolved) {
						qWarning() << "Dependency" << projectName
								   << "could not be resolved on any platform";
						ModPlatform::UnresolvedDep unresolved;
						unresolved.name = projectName;
						m_unresolvedDeps.append(unresolved);
					}
				}
			} else {
				qWarning() << "Cross resolve search returned no valid match for"
						   << projectName;
				ModPlatform::UnresolvedDep unresolved;
				unresolved.name = projectName;
				m_unresolvedDeps.append(unresolved);
			}
		});
}

void DependencyResolver::checkCompletion()
{
	if (m_aborted) {
		/* Given up on. The task has already reported aborted, and
		 * emitting succeeded on top of that would be a second verdict on
		 * the same run. */
		return;
	}

	if (m_pendingRequests <= 0 && m_currentModIndex >= m_selectedMods.size()) {
		// === Pass 1: Deduplicate dependencies by same platform+projectId
		// (version conflict) === If multiple dep chains resolved different
		// versions of the same project, keep the first.
		{
			QSet<QString> seenKeys;
			QList<ModPlatform::DependencyInfo> deduped;
			for (const auto& dep : m_dependencies) {
				QString key = dep.platform + ":" + dep.projectId;
				if (seenKeys.contains(key)) {
					qDebug()
						<< "Removing duplicate dep (same project, version "
						   "conflict):"
						<< dep.name << dep.versionId << "on" << dep.platform;
					continue;
				}
				seenKeys.insert(key);
				deduped.append(dep);
			}
			m_dependencies = deduped;
		}

		// === Pass 2: Cross-platform dedup by normalized name ===
		// Same library resolved from both CF and MR should only appear once.
		{
			QMap<QString, int>
				nameToIndex; // normalized name -> index in deduped list
			QList<ModPlatform::DependencyInfo> deduped;
			for (const auto& dep : m_dependencies) {
				QString normalized = normalizeName(dep.name);
				if (normalized.isEmpty()) {
					deduped.append(dep);
					continue;
				}
				if (nameToIndex.contains(normalized)) {
					// Prefer the one with SHA1, then the one with a download
					// URL
					int existingIdx = nameToIndex[normalized];
					const auto& existing = deduped[existingIdx];
					bool replaceExisting =
						existing.sha1.isEmpty() && !dep.sha1.isEmpty();
					if (replaceExisting) {
						qDebug() << "Cross-platform dedup: replacing"
								 << existing.name << "(" << existing.platform
								 << ") with" << dep.name << "(" << dep.platform
								 << ") - better metadata";
						deduped[existingIdx] = dep;
					} else {
						qDebug() << "Cross-platform dedup: skipping duplicate"
								 << dep.name << "(" << dep.platform
								 << ") - already have" << existing.name << "("
								 << existing.platform << ")";
					}
				} else {
					nameToIndex.insert(normalized, deduped.size());
					deduped.append(dep);
				}
			}
			m_dependencies = deduped;
		}

		// === Pass 3: Deduplicate by fileName (exact same file from different
		// paths) ===
		{
			QSet<QString> seenFiles;
			QList<ModPlatform::DependencyInfo> deduped;
			for (const auto& dep : m_dependencies) {
				if (!dep.fileName.isEmpty() &&
					seenFiles.contains(dep.fileName)) {
					qDebug() << "Removing dep with duplicate filename:"
							 << dep.fileName;
					continue;
				}
				if (!dep.fileName.isEmpty()) {
					seenFiles.insert(dep.fileName);
				}
				deduped.append(dep);
			}
			m_dependencies = deduped;
		}

		// === Pass 4: Also dedup against selected mods by name ===
		// Also check platform+projectId directly: CurseForge dependency
		// entries carry the *file's* displayName (e.g. "sodium-fabric-
		// mc1.20.1-0.5.8.jar") in `name`, not the project's name, so the
		// normalized-name comparison alone misses the case where the user
		// explicitly selected the same project that a dependency chain also
		// pulled in - letting two versions of the same mod slip into the
		// same download plan.
		{
			QList<ModPlatform::DependencyInfo> deduped;
			for (const auto& dep : m_dependencies) {
				QString normalized = normalizeName(dep.name);
				bool isDuplicate = false;
				for (const auto& selected : m_selectedMods) {
					const bool sameProject =
						!dep.platform.isEmpty() && !dep.projectId.isEmpty() &&
						selected.platform == dep.platform &&
						selected.projectId == dep.projectId;
					const bool sameName =
						!normalized.isEmpty() &&
						normalizeName(selected.name) == normalized;
					if (sameProject || sameName) {
						qDebug() << "Removing dep that duplicates selected mod:"
								 << dep.name << "(" << dep.platform
								 << dep.projectId << ") vs selected"
								 << selected.name << "(" << selected.platform
								 << selected.projectId << ")";
						isDuplicate = true;
						break;
					}
				}
				if (!isDuplicate) {
					deduped.append(dep);
				}
			}
			m_dependencies = deduped;
		}

		// === Clean up unresolved list ===
		QList<ModPlatform::UnresolvedDep> finalUnresolved;
		QSet<QString> seenNames;
		for (const auto& unresolved : m_unresolvedDeps) {
			if (seenNames.contains(unresolved.name))
				continue;
			bool wasResolved = false;
			for (const auto& dep : m_dependencies) {
				if (dep.name == unresolved.name ||
					dep.projectId == unresolved.projectId ||
					normalizeName(dep.name) == normalizeName(unresolved.name)) {
					wasResolved = true;
					break;
				}
			}
			if (!wasResolved) {
				seenNames.insert(unresolved.name);
				finalUnresolved.append(unresolved);
			}
		}
		m_unresolvedDeps = finalUnresolved;

		if (!m_unresolvedDeps.isEmpty()) {
			qWarning() << "Unresolved dependencies:";
			for (const auto& u : m_unresolvedDeps) {
				qWarning() << "  -" << u.name << "(" << u.platform
						   << u.projectId << ")";
			}
		}

		qDebug() << "Dependency resolution complete:" << m_dependencies.size()
				 << "resolved," << m_unresolvedDeps.size() << "unresolved";

		emitSucceeded();
	}
}
