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

#include "ProjectItemDelegate.h"

#include "QtCompat.h"

#include <QApplication>
#include <QFontMetrics>
#include <QMouseEvent>
#include <QPainter>
#include <QTextLayout>
#include <QTextLine>

namespace
{

	/* How much bigger the title is than the body text. */
	constexpr int kTitlePointSizeBump = 2;
	/* Left margin for the checkbox, matching the icon's top margin so the
	 * two line up on the same optical grid. */
	constexpr int kCheckboxMargin = 5;
	/* An installed row is drawn faded: it is still worth showing (so the
	 * user can see they already have it) but it cannot be added. */
	constexpr qreal kInstalledOpacity = 0.4;

	/* Break `text` into at most `maxLines` lines that fit `width`.
	 *
	 * Returns the lines; sets `elidedTail` when the text did not fit, in
	 * which case the caller should elide the last line. Qt has no
	 * ready-made "wrap to N lines then elide" call, and QFontMetrics
	 * alone cannot tell us where the wrap points land. */
	QStringList wrapToLines(const QString& text, const QFont& font, int width,
							int maxLines, bool& didNotFit)
	{
		QStringList lines;
		didNotFit = false;

		if (text.isEmpty() || width <= 0 || maxLines <= 0) {
			return lines;
		}

		QTextLayout layout(text, font);
		layout.beginLayout();

		int consumed = 0;
		while (lines.size() < maxLines) {
			QTextLine line = layout.createLine();
			if (!line.isValid()) {
				break;
			}
			line.setLineWidth(width);
			lines.append(text.mid(line.textStart(), line.textLength()));
			consumed = line.textStart() + line.textLength();
		}

		layout.endLayout();

		didNotFit = consumed < text.length();
		return lines;
	}

} // namespace

ProjectItemDelegate::ProjectItemDelegate(QWidget* parent)
	: QStyledItemDelegate(parent)
{
}

void ProjectItemDelegate::paint(QPainter* painter,
								const QStyleOptionViewItem& option,
								const QModelIndex& index) const
{
	painter->save();

	QStyleOptionViewItem opt(option);
	initStyleOption(&opt, index);

	const QStyle* style =
		opt.widget == nullptr ? QApplication::style() : opt.widget->style();

	const bool isInstalled = index.data(ProjectItemRole::Installed).toBool();
	const bool isChecked = opt.checkState == Qt::Checked;
	const bool isSelected = (option.state & QStyle::State_Selected) != 0;

	QRect rect = opt.rect;

	/* The Windows style paints the selection highlight as part of the
	 * panel primitive in a way that then covers our text, so there we
	 * fill the highlight ourselves and skip the primitive. */
	const bool windowsStyle = style->objectName().startsWith("windows");

	if (!windowsStyle) {
		style->drawPrimitive(QStyle::PE_PanelItemViewItem, &opt, painter,
							 opt.widget);
	}

	if (isSelected) {
		if (windowsStyle) {
			painter->fillRect(rect, opt.palette.highlight());
		}
		painter->setPen(opt.palette.highlightedText().color());
	}

	if (opt.features & QStyleOptionViewItem::HasCheckIndicator) {
		const QStyleOptionViewItem boxOpt = checkboxOption(opt, style);
		style->drawPrimitive(QStyle::PE_IndicatorItemViewItemCheck, &boxOpt,
							 painter, opt.widget);
		rect.setX(boxOpt.rect.right());
	}

	/* Fade the row only when nothing else is already drawing attention to
	 * it - a selected or checked row should stay legible. */
	if (isInstalled && !isSelected && !isChecked) {
		painter->setOpacity(kInstalledOpacity);
	}

	// Icons are square; fall back to the row height when there is none so
	// the text still starts at a consistent x.
	int iconWidth = rect.height();
	int iconMargin = 0;

	if (!opt.icon.isNull()) {
		const QSize iconSize = opt.decorationSize;
		iconWidth = iconSize.width();
		const int iconHeight = iconSize.height();

		// Same margin horizontally and vertically, so the icon sits in a
		// visually square cell no matter the row height.
		iconMargin = (rect.height() - iconHeight) / 2;

		if (opt.features & QStyleOptionViewItem::HasCheckIndicator) {
			rect.translate(iconMargin / 2, 0);
		}

		// Qt warns when asked to scale a null pixmap.
		if (iconWidth > 0 && iconHeight > 0) {
			opt.icon.paint(painter, rect.x() + iconMargin,
						   rect.y() + iconMargin, iconWidth, iconHeight);
		}
	}

	// Everything from here on is drawn to the right of the icon.
	const int textWidth = rect.width() - iconWidth - 2 * iconMargin;
	rect.setRect(rect.x() + iconWidth + 2 * iconMargin, rect.y(), textWidth,
				 rect.height());

	int titleHeight = 0;
	{
		QString title = index.data(ProjectItemRole::Title).toString();
		if (isInstalled) {
			//: Suffix on a search result the user already has installed
			title = tr("%1 [installed]").arg(title);
		}

		QFont titleFont = opt.font;
		titleFont.setPointSize(titleFont.pointSize() + kTitlePointSizeBump);
		if (isChecked) {
			titleFont.setBold(true);
		}

		painter->save();
		painter->setFont(titleFont);
		titleHeight = QFontMetrics(titleFont).height();
		painter->drawText(rect.x(), rect.y() + titleHeight, title);
		painter->restore();
	}

	{
		const QString description =
			index.data(ProjectItemRole::Description).toString().simplified();
		const QFontMetrics metrics = opt.fontMetrics;
		const int spaceBelowTitle = rect.height() - titleHeight;

		/* Two lines need a bit more than twice the line height to avoid
		 * looking cramped against the row edge; below that, settle for
		 * one elided line. */
		const int maxLines =
			(spaceBelowTitle <= 2.5 * metrics.height()) ? 1 : 2;

		bool didNotFit = false;
		QStringList lines =
			wrapToLines(description, opt.font, textWidth, maxLines, didNotFit);

		if (!lines.isEmpty() && didNotFit) {
			lines.last() =
				metrics.elidedText(lines.last(), opt.textElideMode, textWidth);
		}

		if (!lines.isEmpty()) {
			const int lineCount = lines.size();

			// Centre the description block in the space under the title.
			int y = rect.y() + titleHeight + spaceBelowTitle / 2;
			y -= (lineCount == 1) ? metrics.height() / 2 : metrics.height();

			painter->drawText(rect.x(), y, textWidth,
							  lineCount * metrics.height(), Qt::TextWordWrap,
							  lines.join(QString()));
		}
	}

	painter->restore();
}

bool ProjectItemDelegate::editorEvent(QEvent* event, QAbstractItemModel* model,
									  const QStyleOptionViewItem& option,
									  const QModelIndex& index)
{
	if (event->type() != QEvent::MouseButtonRelease &&
		event->type() != QEvent::MouseButtonPress &&
		event->type() != QEvent::MouseButtonDblClick) {
		return QStyledItemDelegate::editorEvent(event, model, option, index);
	}

	auto* mouseEvent = static_cast<QMouseEvent*>(event);
	if (mouseEvent->button() != Qt::LeftButton) {
		return QStyledItemDelegate::editorEvent(event, model, option, index);
	}

	QStyleOptionViewItem opt(option);
	initStyleOption(&opt, index);

	if (!(opt.features & QStyleOptionViewItem::HasCheckIndicator)) {
		return QStyledItemDelegate::editorEvent(event, model, option, index);
	}

	const QStyle* style =
		opt.widget == nullptr ? QApplication::style() : opt.widget->style();
	const QStyleOptionViewItem boxOpt = checkboxOption(opt, style);

	if (!boxOpt.rect.contains(QtCompat::mousePosition(mouseEvent).toPoint())) {
		return QStyledItemDelegate::editorEvent(event, model, option, index);
	}

	/* Swallow press and double-click so a checkbox click does not also
	 * move the selection or fire the view's double-click action; only
	 * act on release, the way a real checkbox does. */
	if (event->type() != QEvent::MouseButtonRelease) {
		return true;
	}

	emit checkboxClicked(index);
	return true;
}

QStyleOptionViewItem
ProjectItemDelegate::checkboxOption(const QStyleOptionViewItem& option,
									const QStyle* style) const
{
	QStyleOptionViewItem boxOpt = option;

	boxOpt.state &= ~QStyle::State_HasFocus;
	boxOpt.state |=
		(boxOpt.checkState == Qt::Checked) ? QStyle::State_On : QStyle::State_Off;

	const QRect natural = style->subElementRect(
		QStyle::SE_ItemViewItemCheckIndicator, &boxOpt, option.widget);

	boxOpt.rect =
		QRect(option.rect.x() + kCheckboxMargin,
			  option.rect.y() + (option.rect.height() - natural.height()) / 2,
			  natural.width(), natural.height());

	return boxOpt;
}
