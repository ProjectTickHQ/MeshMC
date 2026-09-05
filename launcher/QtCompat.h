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

/* Shims for the handful of Qt APIs that MeshMC uses and that differ between
 * Qt 5 and Qt 6.
 *
 * This exists so the difference is spelled out once, here, instead of as a
 * #if scattered through call sites. Only add things that genuinely cannot be
 * written the same way against both majors -- if an API works unchanged on
 * both (even if a nicer spelling exists on Qt 6), it does not belong here.
 *
 * Each shim deliberately avoids APIs that are merely deprecated on one side:
 * QMouseEvent::pos(), for instance, compiles on both but is deprecated on
 * Qt 6, and building on it would just relocate the problem to Qt 7.
 */

#include <QtGlobal>
#include <QMouseEvent>
#include <QObject>
#include <QPointF>

#include <memory>
#include <utility>

namespace QtCompat
{
	/* Connect a signal to a functor that should fire at most once.
	 *
	 * Qt 6 expresses this as a connection type, Qt::SingleShotConnection.
	 * Qt 5 has no equivalent, so the connection handle is captured and
	 * disconnected from inside the wrapper the first time it runs. The
	 * shared_ptr is what makes that possible: the handle has to outlive
	 * this call and be reachable from the functor.
	 *
	 * The Qt 5 wrapper takes its arguments variadically because only the
	 * signal knows how many there are; Qt derives the count from the
	 * signal and calls the functor with exactly that many.
	 */
	template <typename Sender, typename Signal, typename Receiver,
			  typename Functor>
	inline void connectOnce(Sender* sender, Signal signal, Receiver* receiver,
							Functor functor)
	{
#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
		QObject::connect(sender, signal, receiver, std::move(functor),
						 Qt::SingleShotConnection);
#else
		auto connection = std::make_shared<QMetaObject::Connection>();
		*connection = QObject::connect(
			sender, signal, receiver,
			[connection, functor = std::move(functor)](auto&&... args) {
				/* Before the call, not after: the functor is free to
				 * spin an event loop, and a second emission reaching it
				 * in the meantime is exactly what this prevents. */
				QObject::disconnect(*connection);
				functor(std::forward<decltype(args)>(args)...);
			});
#endif
	}

	/* Widget-local position of a mouse event.
	 *
	 * Qt 6 has position(); Qt 5 spells the same value localPos(). Qt 6 does
	 * still carry pos(), but only as a deprecated alias for
	 * position().toPoint().
	 */
	inline QPointF mousePosition(const QMouseEvent* event)
	{
#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
		return event->position();
#else
		return event->localPos();
#endif
	}
} // namespace QtCompat
