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

#include "ExportListDialog.h"

#include <QCheckBox>
#include <QComboBox>
#include <QDebug>
#include <QDialogButtonBox>
#include <QDir>
#include <QFileDialog>
#include <QGridLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QSignalBlocker>
#include <QSizePolicy>
#include <QTextBrowser>
#include <QTextEdit>
#include <QVBoxLayout>

#include <utility>

#include "FileSystem.h"
#include "HoeDown.h"

namespace
{
	/* Combo entries, in this order. The index doubles as the format, so
	 * the two must not be allowed to drift apart. */
	ContentListExport::Format formatForIndex(int index)
	{
		switch (index) {
			case 0:
				return ContentListExport::Format::Html;
			case 1:
				return ContentListExport::Format::Markdown;
			case 2:
				return ContentListExport::Format::PlainText;
			case 3:
				return ContentListExport::Format::Json;
			case 4:
				return ContentListExport::Format::Csv;
			default:
				return ContentListExport::Format::Custom;
		}
	}

	const int kCustomIndex = 5;
} // namespace

ExportListDialog::ExportListDialog(QString name,
								   QList<ContentListExport::Item> items,
								   QWidget* parent)
	: QDialog(parent), m_items(std::move(items)), m_name(std::move(name))
{
	buildUi();
	setCustomMode(false);
	regenerate();
}

void ExportListDialog::buildUi()
{
	setWindowTitle(tr("Export Pack to ModList"));
	setModal(true);
	setSizeGripEnabled(true);
	resize(650, 522);

	/* Two nested vertical layouts, the way the reference dialog is put
	 * together: the inner one carries the panels, the outer adds the
	 * button row underneath them. */
	auto* root = new QVBoxLayout(this);
	auto* panels = new QVBoxLayout();
	root->addLayout(panels);

	auto* settings = new QGroupBox(tr("Settings"), this);
	auto* settingsLayout = new QGridLayout(settings);

	settingsLayout->addWidget(new QLabel(tr("Format"), settings), 0, 0);
	m_formatBox = new QComboBox(settings);
	m_formatBox->addItem(tr("HTML"));
	m_formatBox->addItem(tr("Markdown"));
	m_formatBox->addItem(tr("Plaintext"));
	m_formatBox->addItem(tr("JSON"));
	m_formatBox->addItem(tr("CSV"));
	m_formatBox->addItem(tr("Custom"));
	settingsLayout->addWidget(m_formatBox, 0, 1);

	/* The template box on the left, the fields it can hold on the right,
	 * sharing the row under the format picker. */
	m_templateGroup = new QGroupBox(tr("Template"), settings);
	m_templateGroup->setSizePolicy(QSizePolicy::Preferred,
								   QSizePolicy::Maximum);
	auto* templateLayout = new QVBoxLayout(m_templateGroup);
	m_templateText = new QTextEdit(m_templateGroup);
	m_templateText->setSizePolicy(QSizePolicy::Expanding,
								  QSizePolicy::Maximum);
	m_templateText->setToolTip(
		tr("This text supports the following placeholders:\n"
		   "{name}     - Mod name\n"
		   "{mod_id}   - Mod ID\n"
		   "{url}      - Mod URL\n"
		   "{version}  - Mod version\n"
		   "{authors}  - Mod authors"));
	templateLayout->addWidget(m_templateText);
	settingsLayout->addWidget(m_templateGroup, 1, 0);

	auto* options = new QGroupBox(tr("Optional Info"), settings);
	options->setSizePolicy(QSizePolicy::Preferred,
						   QSizePolicy::MinimumExpanding);
	auto* optionsLayout = new QVBoxLayout(options);

	m_versionCheck = new QCheckBox(tr("Version"), options);
	m_authorsCheck = new QCheckBox(tr("Authors"), options);
	m_urlCheck = new QCheckBox(tr("URL"), options);
	m_fileNameCheck = new QCheckBox(tr("Filename"), options);
	m_versionButton = new QPushButton(tr("Version"), options);
	m_authorsButton = new QPushButton(tr("Authors"), options);
	m_urlButton = new QPushButton(tr("URL"), options);
	m_fileNameButton = new QPushButton(tr("Filename"), options);

	/* Checkboxes first, then the buttons that replace them in custom
	 * mode. Only one of the two sets is ever visible, so they stack in
	 * the same column and the panel does not change shape when the
	 * format does. */
	optionsLayout->addWidget(m_versionCheck);
	optionsLayout->addWidget(m_authorsCheck);
	optionsLayout->addWidget(m_urlCheck);
	optionsLayout->addWidget(m_fileNameCheck);
	optionsLayout->addWidget(m_versionButton);
	optionsLayout->addWidget(m_authorsButton);
	optionsLayout->addWidget(m_urlButton);
	optionsLayout->addWidget(m_fileNameButton);
	settingsLayout->addWidget(options, 1, 1);

	panels->addWidget(settings);

	/* The text that will be written on the left, how it will look on the
	 * right. */
	auto* result = new QGroupBox(tr("Result"), this);
	auto* resultLayout = new QHBoxLayout(result);
	m_finalText = new QPlainTextEdit(result);
	m_finalText->setReadOnly(true);
	m_finalText->setMinimumHeight(143);
	resultLayout->addWidget(m_finalText);
	m_resultText = new QTextBrowser(result);
	m_resultText->setOpenExternalLinks(true);
	resultLayout->addWidget(m_resultText);
	panels->addWidget(result);

	auto* warning = new QLabel(
		tr("This depends on the mods' metadata. To ensure it is available, "
		   "run an update on the instance. Installing the updates isn't "
		   "necessary."),
		this);
	warning->setWordWrap(true);
	panels->addWidget(warning);

	/* Copy shares the bottom row with the dialog's own buttons. */
	auto* buttonRow = new QHBoxLayout();
	auto* copyButton = new QPushButton(tr("Copy"), this);
	buttonRow->addWidget(copyButton);

	auto* buttons = new QDialogButtonBox(
		QDialogButtonBox::Save | QDialogButtonBox::Cancel, this);
	buttons->button(QDialogButtonBox::Save)->setText(tr("Save"));
	buttons->button(QDialogButtonBox::Cancel)->setText(tr("Cancel"));
	buttonRow->addWidget(buttons);
	root->addLayout(buttonRow);

	connect(m_formatBox, qOverload<int>(&QComboBox::currentIndexChanged), this,
			&ExportListDialog::formatChanged);
	for (QCheckBox* box : {m_versionCheck, m_authorsCheck, m_urlCheck,
						   m_fileNameCheck}) {
		connect(box, &QCheckBox::toggled, this,
				[this](bool) { regenerate(); });
	}
	connect(m_versionButton, &QPushButton::clicked, this, [this] {
		insertPlaceholder(ContentListExport::Version);
	});
	connect(m_authorsButton, &QPushButton::clicked, this, [this] {
		insertPlaceholder(ContentListExport::Authors);
	});
	connect(m_urlButton, &QPushButton::clicked, this, [this] {
		insertPlaceholder(ContentListExport::Url);
	});
	connect(m_fileNameButton, &QPushButton::clicked, this, [this] {
		insertPlaceholder(ContentListExport::FileName);
	});
	connect(m_templateText, &QTextEdit::textChanged, this, [this] {
		/* Typing in the box means the user wants their own line, so the
		 * format follows the box rather than the other way round. */
		if (m_templateText->toPlainText()
			!= ContentListExport::exampleLine(m_format)) {
			m_formatBox->setCurrentIndex(kCustomIndex);
		}
		regenerate();
	});
	connect(copyButton, &QPushButton::clicked, this, [this] {
		m_finalText->selectAll();
		m_finalText->copy();
	});
	connect(buttons, &QDialogButtonBox::accepted, this,
			&ExportListDialog::accept);
	connect(buttons, &QDialogButtonBox::rejected, this,
			&ExportListDialog::reject);
}

void ExportListDialog::formatChanged(int index)
{
	m_format = formatForIndex(index);
	const bool custom = m_format == ContentListExport::Format::Custom;
	if (custom) {
		m_templateEdited = true;
	}
	setCustomMode(custom);

	/* Only the marked-up formats have anything to preview; for the rest
	 * the rendered text is the whole story. */
	m_resultText->setVisible(m_format == ContentListExport::Format::Html
							 || m_format
									== ContentListExport::Format::Markdown);
	regenerate();
}

ContentListExport::Fields ExportListDialog::selectedFields() const
{
	ContentListExport::Fields fields = ContentListExport::NoFields;
	if (m_versionCheck->isChecked()) {
		fields |= ContentListExport::Version;
	}
	if (m_authorsCheck->isChecked()) {
		fields |= ContentListExport::Authors;
	}
	if (m_urlCheck->isChecked()) {
		fields |= ContentListExport::Url;
	}
	if (m_fileNameCheck->isChecked()) {
		fields |= ContentListExport::FileName;
	}
	return fields;
}

void ExportListDialog::regenerate()
{
	if (m_format == ContentListExport::Format::Custom) {
		m_finalText->setPlainText(ContentListExport::render(
			m_items, m_templateText->toPlainText()));
		return;
	}

	const QString text =
		ContentListExport::render(m_items, m_format, selectedFields());
	m_finalText->setPlainText(text);

	if (m_format == ContentListExport::Format::Html) {
		m_resultText->setHtml(text);
	} else if (m_format == ContentListExport::Format::Markdown) {
		HoeDown renderer;
		m_resultText->setHtml(renderer.process(text.toUtf8()));
	}

	/* Keep the template box showing what this format produces, so that
	 * switching to Custom starts from something that works. Signals are
	 * blocked because the box's own change handler would otherwise flip
	 * the format straight back to Custom. */
	const QString example = ContentListExport::exampleLine(m_format);
	if (!m_templateEdited && m_templateText->toPlainText() != example) {
		const QSignalBlocker blocker(m_templateText);
		m_templateText->setPlainText(example);
	}
}

void ExportListDialog::insertPlaceholder(ContentListExport::Field field)
{
	if (m_format != ContentListExport::Format::Custom) {
		return;
	}
	switch (field) {
		case ContentListExport::Authors:
			m_templateText->insertPlainText(QStringLiteral("{authors}"));
			break;
		case ContentListExport::Url:
			m_templateText->insertPlainText(QStringLiteral("{url}"));
			break;
		case ContentListExport::Version:
			m_templateText->insertPlainText(QStringLiteral("{version}"));
			break;
		case ContentListExport::FileName:
			m_templateText->insertPlainText(QStringLiteral("{filename}"));
			break;
		case ContentListExport::NoFields:
			break;
	}
}

void ExportListDialog::setCustomMode(bool custom)
{
	m_versionCheck->setHidden(custom);
	m_authorsCheck->setHidden(custom);
	m_urlCheck->setHidden(custom);
	m_fileNameCheck->setHidden(custom);

	m_versionButton->setHidden(!custom);
	m_authorsButton->setHidden(!custom);
	m_urlButton->setHidden(!custom);
	m_fileNameButton->setHidden(!custom);
}

void ExportListDialog::done(int result)
{
	if (result == Accepted) {
		const QString suggested = FS::RemoveInvalidFilenameChars(m_name)
								  + ContentListExport::fileExtension(m_format);
		const QString target = QFileDialog::getSaveFileName(
			this, tr("Export %1").arg(m_name),
			FS::PathCombine(QDir::homePath(), suggested),
			tr("File") + QStringLiteral(" (*.txt *.html *.md *.json *.csv)"));

		/* Backing out of the file dialog leaves this one open, so the
		 * work that went into the template is not thrown away. */
		if (target.isEmpty()) {
			return;
		}

		try {
			FS::write(target, m_finalText->toPlainText().toUtf8());
		} catch (const FS::FileSystemException& e) {
			qCritical() << "Could not save the exported list:" << e.cause();
		}
	}

	QDialog::done(result);
}
