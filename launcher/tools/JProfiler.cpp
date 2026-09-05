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

#include "JProfiler.h"

#include <QDir>

#include "settings/SettingsObject.h"
#include "launch/LaunchTask.h"
#include "BaseInstance.h"

class JProfiler : public BaseProfiler
{
	Q_OBJECT
  public:
	JProfiler(SettingsObjectPtr settings, InstancePtr instance,
			  QObject* parent = 0);

  private slots:
	void profilerStarted();
	void profilerFinished(int exit, QProcess::ExitStatus status);

  protected:
	void beginProfilingImpl(shared_qobject_ptr<LaunchTask> process);

  private:
	int listeningPort = 0;
};

JProfiler::JProfiler(SettingsObjectPtr settings, InstancePtr instance,
					 QObject* parent)
	: BaseProfiler(settings, instance, parent)
{
}

void JProfiler::profilerStarted()
{
	emit readyToLaunch(tr("Listening on port: %1").arg(listeningPort));
}

void JProfiler::profilerFinished(int exit, QProcess::ExitStatus status)
{
	if (status == QProcess::CrashExit) {
		emit abortLaunch(tr("Profiler aborted"));
	}
	if (m_profilerProcess) {
		m_profilerProcess->deleteLater();
		m_profilerProcess = 0;
	}
}

void JProfiler::beginProfilingImpl(shared_qobject_ptr<LaunchTask> process)
{
	listeningPort = globalSettings->get("JProfilerPort").toInt();
	QProcess* profiler = new QProcess(this);
	QStringList profilerArgs = {"-d", QString::number(process->pid()), "--gui",
								"-p", QString::number(listeningPort)};
	auto basePath = globalSettings->get("JProfilerPath").toString();

#ifdef Q_OS_WIN
	QString profilerProgram =
		QDir(basePath).absoluteFilePath("bin/jpenable.exe");
#else
	QString profilerProgram = QDir(basePath).absoluteFilePath("bin/jpenable");
#endif

	profiler->setArguments(profilerArgs);
	profiler->setProgram(profilerProgram);

	connect(profiler, &QProcess::started, this, &JProfiler::profilerStarted);
	connect(profiler, qOverload<int, QProcess::ExitStatus>(&QProcess::finished), this, &JProfiler::profilerFinished);

	m_profilerProcess = profiler;
	profiler->start();
}

void JProfilerFactory::registerSettings(SettingsObjectPtr settings)
{
	settings->registerSetting("JProfilerPath");
	settings->registerSetting("JProfilerPort", 42042);
	globalSettings = settings;
}

BaseExternalTool* JProfilerFactory::createTool(InstancePtr instance,
											   QObject* parent)
{
	return new JProfiler(globalSettings, instance, parent);
}

bool JProfilerFactory::check(QString* error)
{
	return check(globalSettings->get("JProfilerPath").toString(), error);
}

bool JProfilerFactory::check(const QString& path, QString* error)
{
	if (path.isEmpty()) {
		*error = QObject::tr("Empty path");
		return false;
	}
	QDir dir(path);
	if (!dir.exists()) {
		*error = QObject::tr("Path does not exist");
		return false;
	}
	if (!dir.exists("bin") ||
		!(dir.exists("bin/jprofiler") || dir.exists("bin/jprofiler.exe")) ||
		!dir.exists("bin/agent.jar")) {
		*error = QObject::tr("Invalid JProfiler install");
		return false;
	}
	return true;
}

#include "JProfiler.moc"
