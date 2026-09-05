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
#include <optional>

#include "InstanceTask.h"
#include "net/NetJob.h"
#include "meta/Index.h"
#include "meta/Version.h"
#include "meta/VersionList.h"
#include "PackHelpers.h"

#include "net/NetJob.h"

namespace LegacyFTB
{

	class PackInstallTask : public InstanceTask
	{
		Q_OBJECT

	  public:
		explicit PackInstallTask(
			shared_qobject_ptr<QNetworkAccessManager> network, Modpack pack,
			QString version);
		virtual ~PackInstallTask() {}

		bool canAbort() const override
		{
			/* Two abortable stretches, with a gap between them: the
			 * pack's own download while `abortable` is set, and the
			 * optional game-file download the base class runs once the
			 * instance is built. Answering "always" would light up a
			 * button that abort() then refuses to act on. */
			return abortable || InstanceTask::canAbort();
		}
		bool abort() override;

	  protected:
		//! Entry point for tasks.
		virtual void executeTask() override;

	  private:
		void downloadPack();
		void unzip();
		void install();

	  private slots:
		void onDownloadSucceeded();
		void onDownloadFailed(QString reason);
		void onDownloadProgress(qint64 current, qint64 total);

		void onUnzipFinished();
		void onUnzipCanceled();

	  private: /* data */
		shared_qobject_ptr<QNetworkAccessManager> m_network;
		bool abortable = false;
		QFuture<std::optional<QStringList>> m_extractFuture;
		QFutureWatcher<std::optional<QStringList>> m_extractFutureWatcher;
		NetJob::Ptr netJobContainer;
		QString archivePath;

		Modpack m_pack;
		QString m_version;
	};

} // namespace LegacyFTB
