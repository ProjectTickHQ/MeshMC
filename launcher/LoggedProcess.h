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

#include <QProcess>
#include "MessageLevel.h"

/*
 * This is a basic process.
 * It has line-based logging support and hides some of the nasty bits.
 */
class LoggedProcess : public QProcess
{
	Q_OBJECT
  public:
	enum State {
		NotRunning,
		Starting,
		FailedToStart,
		Running,
		Finished,
		Crashed,
		Aborted
	};

  public:
	explicit LoggedProcess(QObject* parent = 0);
	virtual ~LoggedProcess();

	State state() const;
	int exitCode() const;
	qint64 processId() const;

	void setDetachable(bool detachable);

  signals:
	void log(QStringList lines, MessageLevel::Enum level);
	void stateChanged(LoggedProcess::State state);

  public slots:
	/**
	 * @brief kill the process - equivalent to kill -9
	 */
	void kill();

  private slots:
	void on_stdErr();
	void on_stdOut();
	void on_exit(int exit_code, QProcess::ExitStatus status);
	void on_error(QProcess::ProcessError error);
	void on_stateChange(QProcess::ProcessState);

#if QT_VERSION < QT_VERSION_CHECK(6, 0, 0) && defined(Q_OS_UNIX)
  protected:
	/**
	 * @brief put the child into a new process group so kill() reaches the
	 *        whole tree
	 *
	 * Qt 6 does this with setChildProcessModifier() in the constructor;
	 * Qt 5 has no such setter and expects this protected virtual to be
	 * overridden instead. Both run in the child between fork() and exec(),
	 * so only async-signal-safe calls belong here.
	 */
	void setupChildProcess() override;
#endif

  private:
	void changeState(LoggedProcess::State state);

#ifdef Q_OS_WIN
	/**
	 * @brief put the child into a fresh job object
	 *
	 * Called once the child actually exists. Without this, kill() can only
	 * reach the direct child - which is the wrapper command when one is
	 * configured, leaving java (and whatever java spawned) alive.
	 */
	void assignToJobObject();
#endif

  private:
	QString m_err_leftover;
	QString m_out_leftover;
	bool m_killed = false;
	State m_state = NotRunning;
	int m_exit_code = 0;
	bool m_is_aborting = false;
	bool m_is_detachable = false;
#ifdef Q_OS_WIN
	// The job object the child was assigned to, or null if we never got one
	// (then kill() falls back to only killing the direct child).
	// Typed as void* on purpose: this is a Windows HANDLE, but keeping it
	// opaque means windows.h stays out of every translation unit that
	// launches a process.
	void* m_job = nullptr;
#endif
};
