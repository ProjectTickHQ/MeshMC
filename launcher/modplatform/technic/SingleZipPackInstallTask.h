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

#pragma once

#include "InstanceTask.h"
#include "net/HttpMetaCache.h"
#include "net/NetJob.h"

#include <QFutureWatcher>
#include <QStringList>
#include <QUrl>
#include <optional>

namespace Technic
{

	class SingleZipPackInstallTask : public InstanceTask
	{
		Q_OBJECT

	  public:
		SingleZipPackInstallTask(const QUrl& sourceUrl,
								 const QString& minecraftVersion);

		bool canAbort() const override
		{
			return true;
		}
		bool abort() override;

	  protected:
		void executeTask() override;

	  private slots:
		void downloadSucceeded();
		void downloadFailed(QString reason);
		void downloadProgressChanged(qint64 current, qint64 total);
		void extractFinished();
		void extractAborted();

	  private:
		bool m_abortable = false;

		QUrl m_sourceUrl;
		QString m_minecraftVersion;
		QString m_archivePath;
		/* The cache slot the archive was downloaded into. Kept so that a
		 * failed extraction can throw the file away: an archive that
		 * cannot be unpacked is worthless, and leaving it cached and
		 * fresh means every retry unpacks the same broken bytes. */
		MetaEntryPtr m_archiveEntry;
		NetJob::Ptr m_filesNetJob;
		QFuture<std::optional<QStringList>> m_extractFuture;
		QFutureWatcher<std::optional<QStringList>> m_extractFutureWatcher;
	};

} // namespace Technic
