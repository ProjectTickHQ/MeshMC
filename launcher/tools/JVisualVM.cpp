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

#include "JVisualVM.h"

#include <QDir>
#include <QStandardPaths>

#include "settings/SettingsObject.h"
#include "launch/LaunchTask.h"
#include "BaseInstance.h"

class JVisualVM : public BaseProfiler
{
	Q_OBJECT
  public:
	JVisualVM(SettingsObjectPtr settings, InstancePtr instance,
			  QObject* parent = 0);

  private slots:
	void profilerStarted();
	void profilerFinished(int exit, QProcess::ExitStatus status);

  protected:
	void beginProfilingImpl(shared_qobject_ptr<LaunchTask> process);
};

JVisualVM::JVisualVM(SettingsObjectPtr settings, InstancePtr instance,
					 QObject* parent)
	: BaseProfiler(settings, instance, parent)
{
}

void JVisualVM::profilerStarted()
{
	emit readyToLaunch(tr("JVisualVM started"));
}

void JVisualVM::profilerFinished(int exit, QProcess::ExitStatus status)
{
	if (status == QProcess::CrashExit) {
		emit abortLaunch(tr("Profiler aborted"));
	}
	if (m_profilerProcess) {
		m_profilerProcess->deleteLater();
		m_profilerProcess = 0;
	}
}

void JVisualVM::beginProfilingImpl(shared_qobject_ptr<LaunchTask> process)
{
	QProcess* profiler = new QProcess(this);
	QStringList profilerArgs = {"--openpid", QString::number(process->pid())};
	auto programPath = globalSettings->get("JVisualVMPath").toString();

	profiler->setArguments(profilerArgs);
	profiler->setProgram(programPath);

	connect(profiler, &QProcess::started, this, &JVisualVM::profilerStarted);
	connect(profiler, qOverload<int, QProcess::ExitStatus>(&QProcess::finished), this, &JVisualVM::profilerFinished);

	profiler->start();
	m_profilerProcess = profiler;
}

void JVisualVMFactory::registerSettings(SettingsObjectPtr settings)
{
	QString defaultValue = QStandardPaths::findExecutable("jvisualvm");
	if (defaultValue.isNull()) {
		defaultValue = QStandardPaths::findExecutable("visualvm");
	}
	settings->registerSetting("JVisualVMPath", defaultValue);
	globalSettings = settings;
}

BaseExternalTool* JVisualVMFactory::createTool(InstancePtr instance,
											   QObject* parent)
{
	return new JVisualVM(globalSettings, instance, parent);
}

bool JVisualVMFactory::check(QString* error)
{
	return check(globalSettings->get("JVisualVMPath").toString(), error);
}

bool JVisualVMFactory::check(const QString& path, QString* error)
{
	if (path.isEmpty()) {
		*error = QObject::tr("Empty path");
		return false;
	}
	QFileInfo finfo(path);
	if (!finfo.isExecutable() || !finfo.fileName().contains("visualvm")) {
		*error = QObject::tr("Invalid path to JVisualVM");
		return false;
	}
	return true;
}

#include "JVisualVM.moc"
