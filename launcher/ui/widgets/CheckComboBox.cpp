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

#include "CheckComboBox.h"

#include "QtCompat.h"

#include <QAbstractItemView>
#include <QEvent>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QStyleOptionComboBox>
#include <QStylePainter>

CheckComboBox::CheckComboBox(QWidget* parent)
	: QComboBox(parent), m_separator(QStringLiteral(", "))
{
	/* The popup is a window of its own, so the presses that have to be
	 * caught arrive at three different objects; the box itself is
	 * watched for the keys and the wheel. */
	view()->installEventFilter(this);
	view()->window()->installEventFilter(this);
	view()->viewport()->installEventFilter(this);
	installEventFilter(this);

	/* Activation is a tick here, not a choice. */
	connect(this, qOverload<int>(&QComboBox::activated), this, &CheckComboBox::toggleItem);
}

void CheckComboBox::setDefaultText(const QString& text)
{
	m_defaultText = text;
	update();
}

void CheckComboBox::setSeparator(const QString& separator)
{
	m_separator = separator;
	update();
}

void CheckComboBox::setItems(const QStringList& items)
{
	const QStringList previouslyChecked = checkedItems();

	/* Signals are blocked while the box is refilled: clear() and
	 * addItem() move the current index about, and each move would
	 * otherwise be reported as a tick. */
	const bool blocked = blockSignals(true);
	clear();
	for (const QString& item : items) {
		addItem(item);
		/* Every entry carries a check state from the start - the popup
		 * only draws a tick box for entries that have one. */
		setItemData(count() - 1,
					previouslyChecked.contains(item) ? Qt::Checked
													: Qt::Unchecked,
					Qt::CheckStateRole);
	}
	blockSignals(blocked);

	update();

	/* Only worth reporting when the refill actually lost something the
	 * user had ticked. */
	const QStringList nowChecked = checkedItems();
	if (nowChecked != previouslyChecked) {
		emit checkedItemsChanged(nowChecked);
	}
}

QStringList CheckComboBox::checkedItems() const
{
	QStringList checked;
	for (int i = 0; i < count(); ++i) {
		const QVariant state = itemData(i, Qt::CheckStateRole);
		if (state.isValid() &&
			static_cast<Qt::CheckState>(state.toInt()) == Qt::Checked) {
			checked.append(itemText(i));
		}
	}
	return checked;
}

void CheckComboBox::setCheckedItems(const QStringList& items)
{
	const QStringList before = checkedItems();

	for (int i = 0; i < count(); ++i) {
		setItemData(i,
					items.contains(itemText(i)) ? Qt::Checked : Qt::Unchecked,
					Qt::CheckStateRole);
	}

	update();

	const QStringList after = checkedItems();
	if (after != before) {
		emit checkedItemsChanged(after);
	}
}

void CheckComboBox::toggleItem(int index)
{
	if (index < 0 || index >= count()) {
		return;
	}
	const QVariant state = itemData(index, Qt::CheckStateRole);
	if (!state.isValid()) {
		return;
	}

	setItemData(index,
				static_cast<Qt::CheckState>(state.toInt()) == Qt::Checked
					? Qt::Unchecked
					: Qt::Checked,
				Qt::CheckStateRole);

	update();
	emit checkedItemsChanged(checkedItems());
}

void CheckComboBox::hidePopup()
{
	/* A press that landed on an entry means "tick this", not "I am
	 * done" - closing here would make ticking several values a matter
	 * of reopening the popup for each one. */
	if (!m_pressOnItem) {
		QComboBox::hidePopup();
	}
}

bool CheckComboBox::eventFilter(QObject* watched, QEvent* event)
{
	switch (event->type()) {
		case QEvent::KeyPress:
		case QEvent::KeyRelease: {
			auto* keyEvent = static_cast<QKeyEvent*>(event);
			if (watched == this && (keyEvent->key() == Qt::Key_Up ||
									keyEvent->key() == Qt::Key_Down)) {
				/* Up and Down would step through the entries on an
				 * ordinary combo box, which here would silently tick
				 * one; opening the popup is the useful reading. */
				showPopup();
				return true;
			}
			if (keyEvent->key() == Qt::Key_Enter ||
				keyEvent->key() == Qt::Key_Return ||
				keyEvent->key() == Qt::Key_Escape) {
				/* Straight to the base class: our own hidePopup() is
				 * there to keep the popup open through clicks, and
				 * these keys mean the user is finished with it. */
				QComboBox::hidePopup();
				/* Escape is passed on so that it can still close the
				 * dialog once the popup is gone. */
				return keyEvent->key() != Qt::Key_Escape;
			}
			break;
		}

		case QEvent::MouseButtonPress: {
			auto* mouseEvent = static_cast<QMouseEvent*>(event);
			const QPoint position = QtCompat::mousePosition(mouseEvent).toPoint();
			m_pressOnItem = view()->indexAt(position).isValid() &&
							view()->rect().contains(position);
			break;
		}

		case QEvent::Wheel:
			/* Scrolling over a closed combo box changes its value, which
			 * for a filter means a search the user never asked for -
			 * usually while they were scrolling the panel. */
			return watched == this;

		default:
			break;
	}

	return QComboBox::eventFilter(watched, event);
}

void CheckComboBox::paintEvent(QPaintEvent* event)
{
	Q_UNUSED(event)

	QStylePainter painter(this);
	painter.setPen(palette().color(QPalette::Text));

	QStyleOptionComboBox option;
	initStyleOption(&option);

	/* The current entry means nothing here - what the box has to show is
	 * everything that is ticked. */
	const QStringList checked = checkedItems();
	option.currentText =
		checked.isEmpty() ? m_defaultText : checked.join(m_separator);

	painter.drawComplexControl(QStyle::CC_ComboBox, option);
	painter.drawControl(QStyle::CE_ComboBoxLabel, option);
}
