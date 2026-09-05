/* SPDX-FileCopyrightText: 2026 Project Tick
 * SPDX-License-Identifier: Apache-2.0
 *
 *  SkinManagerDialog implementation.  See the header comment for the
 *  high-level shape of the dialog.
 */

#include "SkinManagerDialog.h"
#include "SkinViewerWidget.h"
#include "ui_SkinManagerDialog.h"

#include <QDialogButtonBox>
#include <QFileDialog>
#include <QFileInfo>
#include <QPainter>
#include <QPushButton>
#include <QStandardPaths>

using SkinManagerNS::ModelVariant;
using SkinManagerNS::SkinViewerWidget;

namespace
{

	/* Same heuristic as the global page used earlier — kept self-contained
	 * so the two files don't share state. */
	ModelVariant detectVariant(const QImage& image)
	{
		if (image.isNull())
			return ModelVariant::Classic;
		QImage img =
			image.scaled(64, 64, Qt::IgnoreAspectRatio, Qt::FastTransformation);
		img = img.convertToFormat(QImage::Format_ARGB32);
		const QRgb px = img.pixel(54, 20);
		return (qAlpha(px) > 0) ? ModelVariant::Classic : ModelVariant::Slim;
	}

	QImage normaliseSkin(const QImage& src)
	{
		if (src.isNull())
			return src;
		if (src.width() == 64 && src.height() == 64)
			return src.convertToFormat(QImage::Format_RGBA8888);

		/* Legacy 64×32 skin → mirror right limbs onto the bottom half. */
		QImage upgraded(64, 64, QImage::Format_RGBA8888);
		upgraded.fill(Qt::transparent);
		const QImage base =
			src.scaled(64, 32, Qt::IgnoreAspectRatio, Qt::FastTransformation)
				.convertToFormat(QImage::Format_RGBA8888);
		for (int y = 0; y < 32; ++y) {
			memcpy(upgraded.scanLine(y), base.constScanLine(y),
				   base.bytesPerLine());
		}
		for (int y = 0; y < 16; ++y) {
			for (int x = 0; x < 16; ++x) {
				upgraded.setPixelColor(32 + x, 48 + y,
									   base.pixelColor(40 + (15 - x), 16 + y));
				upgraded.setPixelColor(16 + x, 48 + y,
									   base.pixelColor(0 + (15 - x), 16 + y));
			}
		}
		return upgraded;
	}

} // namespace

/* Helper: pull the player-facing profile name from the C ABI. The
 * accountId is the same string S7's account_get_profile_id returns. */
static QString profileNameFromCtx(MMCOContext* ctx, const QString& accountId)
{
	if (!ctx || accountId.isEmpty())
		return {};
	const int total = ctx->account_count(ctx->module_handle);
	for (int i = 0; i < total; ++i) {
		const char* idC = ctx->account_get_id_by_index(ctx->module_handle, i);
		if (!idC)
			continue;
		if (QString::fromUtf8(idC) != accountId)
			continue;
		const char* n = ctx->account_get_profile_name(ctx->module_handle, i);
		return n ? QString::fromUtf8(n) : QString();
	}
	return {};
}

SkinManagerDialog::SkinManagerDialog(const QString& accountId, MMCOContext* ctx,
									 QWidget* parent)
	: QDialog(parent), ui(new Ui::SkinManagerDialog), m_accountId(accountId),
	  m_ctx(ctx)
{
	ui->setupUi(this);
	const QString profileName = profileNameFromCtx(m_ctx, m_accountId);
	setWindowTitle(
		tr("Skin Upload — %1")
			.arg(profileName.isEmpty() ? tr("(no account)") : profileName));

	/* Replace the placeholder viewerHost with a real OpenGL viewer. */
	m_viewer = new SkinViewerWidget(ui->viewerHost);
	ui->viewerHostLayout->addWidget(m_viewer);

	QObject::connect(m_viewer, &SkinViewerWidget::skinFileDropped, this,
					 &SkinManagerDialog::onSkinFileDropped);
	QObject::connect(ui->btnBrowse, &QPushButton::clicked, this,
					 &SkinManagerDialog::onBrowseClicked);
	QObject::connect(ui->btnReset, &QPushButton::clicked, this,
					 &SkinManagerDialog::onResetClicked);
	QObject::connect(ui->rdoClassic, &QRadioButton::toggled, this,
					 &SkinManagerDialog::onVariantToggled);
	QObject::connect(ui->rdoSlim, &QRadioButton::toggled, this,
					 &SkinManagerDialog::onVariantToggled);
	QObject::connect(ui->chkOverlay, &QCheckBox::toggled, this,
					 &SkinManagerDialog::onOverlayToggled);
	QObject::connect(ui->chkAutoRotate, &QCheckBox::toggled, this,
					 &SkinManagerDialog::onAutoRotateToggled);
	QObject::connect(ui->capeCombo,
					 QOverload<int>::of(qOverload<int>(&QComboBox::currentIndexChanged)), this,
					 &SkinManagerDialog::onCapeChanged);
	QObject::connect(ui->buttonBox, &QDialogButtonBox::accepted, this,
					 &SkinManagerDialog::onAccept);
	QObject::connect(ui->buttonBox, &QDialogButtonBox::rejected, this,
					 &QDialog::reject);

	loadAccountState();
}

SkinManagerDialog::~SkinManagerDialog()
{
	delete ui;
}

/* ── load: render whatever the account currently has ─────────────── */

void SkinManagerDialog::loadAccountState()
{
	if (!m_ctx || m_accountId.isEmpty())
		return;

	const QByteArray idUtf8 = m_accountId.toUtf8();
	ui->accountLabel->setText(
		tr("Account: %1").arg(profileNameFromCtx(m_ctx, m_accountId)));

	/* Skin blob via the C ABI. */
	QImage skinImg;
	const void* skinPtr = nullptr;
	const int64_t skinLen = m_ctx->account_get_skin_blob(
		m_ctx->module_handle, idUtf8.constData(), &skinPtr);
	if (skinLen > 0 && skinPtr) {
		skinImg.loadFromData(
			QByteArray::fromRawData(static_cast<const char*>(skinPtr),
									static_cast<int>(skinLen)),
			"PNG");
		skinImg = normaliseSkin(skinImg);
	}

	const char* variantC = m_ctx->account_get_skin_variant(m_ctx->module_handle,
														   idUtf8.constData());
	ModelVariant variant = ModelVariant::Classic;
	if (variantC && QString::fromUtf8(variantC).compare(
						QStringLiteral("SLIM"), Qt::CaseInsensitive) == 0) {
		variant = ModelVariant::Slim;
	} else if (!skinImg.isNull()) {
		variant = detectVariant(skinImg);
	}
	if (variant == ModelVariant::Slim)
		ui->rdoSlim->setChecked(true);
	else
		ui->rdoClassic->setChecked(true);
	m_viewer->setSkin(skinImg, variant);

	/* Capes — walk the C ABI cape list. */
	const char* currentCapeC = m_ctx->account_get_current_cape_id(
		m_ctx->module_handle, idUtf8.constData());
	const QString currentCape =
		currentCapeC ? QString::fromUtf8(currentCapeC) : QString();

	ui->capeCombo->blockSignals(true);
	ui->capeCombo->clear();
	ui->capeCombo->addItem(tr("No Cape"), QString());
	int activeRow = 0;
	const int capeTotal =
		m_ctx->account_cape_count(m_ctx->module_handle, idUtf8.constData());
	for (int i = 0; i < capeTotal; ++i) {
		const char* capeIdC = m_ctx->account_cape_get_id(m_ctx->module_handle,
														 idUtf8.constData(), i);
		const char* capeAliasC = m_ctx->account_cape_get_alias(
			m_ctx->module_handle, idUtf8.constData(), i);
		const void* capePtr = nullptr;
		const int64_t capeLen = m_ctx->account_cape_get_blob(
			m_ctx->module_handle, idUtf8.constData(), i, &capePtr);

		const QString capeId = capeIdC ? QString::fromUtf8(capeIdC) : QString();
		const QString alias =
			capeAliasC ? QString::fromUtf8(capeAliasC) : QString();

		QPixmap preview;
		if (capeLen > 0 && capePtr) {
			QPixmap pix;
			if (pix.loadFromData(
					QByteArray::fromRawData(static_cast<const char*>(capePtr),
											static_cast<int>(capeLen)),
					"PNG")) {
				preview = pix.copy(1, 1, 10, 16);
			}
		}
		const QString label = alias.isEmpty() ? capeId : alias;
		const int row = i + 1; /* +1 for the "No Cape" entry */
		if (preview.isNull())
			ui->capeCombo->addItem(label, capeId);
		else
			ui->capeCombo->addItem(QIcon(preview), label, capeId);
		if (capeId == currentCape)
			activeRow = row;
	}
	ui->capeCombo->setCurrentIndex(activeRow);
	ui->capeCombo->blockSignals(false);

	/* Push cape preview into the 3D viewer. */
	onCapeChanged(activeRow);
}

/* ── browse / drop handlers ──────────────────────────────────────── */

void SkinManagerDialog::onBrowseClicked()
{
	const QString picturesDir =
		QStandardPaths::writableLocation(QStandardPaths::PicturesLocation);
	const QString chosen =
		QFileDialog::getOpenFileName(this, tr("Select Skin Texture"),
									 picturesDir, tr("Minecraft skin (*.png)"));
	if (chosen.isEmpty())
		return;
	loadSkinFile(chosen);
}

void SkinManagerDialog::onSkinFileDropped(const QString& path)
{
	loadSkinFile(path);
}

void SkinManagerDialog::loadSkinFile(const QString& path)
{
	QImage img(path);
	if (img.isNull() || img.width() != 64 ||
		(img.height() != 64 && img.height() != 32)) {
		setStatus(tr("Skin PNGs must be 64×64 (modern) or 64×32 (legacy). "
					 "The chosen file is %1×%2.")
					  .arg(img.width())
					  .arg(img.height()),
				  /*error=*/true);
		return;
	}

	m_chosenSkinPath = path;
	m_chosenSkinImage = normaliseSkin(img);
	ui->skinPathText->setText(path);

	/* Auto-detect variant from the new file unless the user has
	 * manually clicked one of the radios since opening the dialog —
	 * detect-once heuristic. */
	const ModelVariant detected = detectVariant(m_chosenSkinImage);
	if (detected == ModelVariant::Slim)
		ui->rdoSlim->setChecked(true);
	else
		ui->rdoClassic->setChecked(true);

	m_viewer->setSkin(m_chosenSkinImage, detected);
	setStatus(QString());
}

void SkinManagerDialog::onResetClicked()
{
	if (!m_ctx || m_accountId.isEmpty())
		return;

	const QString profileName = profileNameFromCtx(m_ctx, m_accountId);
	const int rc = QMessageBox::question(
		this, tr("Reset skin"),
		tr("Remove the active custom skin for %1?\n\n"
		   "Mojang will revert the account to the "
		   "default Steve or Alex skin.")
			.arg(profileName),
		QMessageBox::Yes | QMessageBox::No, QMessageBox::No);
	if (rc != QMessageBox::Yes)
		return;

	const QByteArray idUtf8 = m_accountId.toUtf8();
	if (m_ctx->account_skin_reset(m_ctx->module_handle, idUtf8.constData()) !=
		0) {
		QMessageBox::warning(this, tr("Skin reset failed"),
							 tr("Mojang did not process the request."));
		return;
	}

	/* Drop the cached skin so the next refresh re-fetches the default. */
	m_ctx->account_set_skin_blob(m_ctx->module_handle, idUtf8.constData(),
								 nullptr, 0);
	m_viewer->clearSkin();
	setStatus(tr("Skin reset to the Mojang default."));
}

/* ── viewer toggles ───────────────────────────────────────────────── */

void SkinManagerDialog::onVariantToggled()
{
	const ModelVariant v =
		ui->rdoSlim->isChecked() ? ModelVariant::Slim : ModelVariant::Classic;
	QImage current;
	if (!m_chosenSkinImage.isNull()) {
		current = m_chosenSkinImage;
	} else if (m_ctx && !m_accountId.isEmpty()) {
		const void* ptr = nullptr;
		const int64_t len = m_ctx->account_get_skin_blob(
			m_ctx->module_handle, m_accountId.toUtf8().constData(), &ptr);
		if (len > 0 && ptr) {
			current.loadFromData(
				QByteArray::fromRawData(static_cast<const char*>(ptr),
										static_cast<int>(len)),
				"PNG");
			current = normaliseSkin(current);
		}
	}
	m_viewer->setSkin(current, v);
}

void SkinManagerDialog::onOverlayToggled(bool on)
{
	m_viewer->setShowOverlay(on);
}

void SkinManagerDialog::onAutoRotateToggled(bool on)
{
	m_viewer->setAutoRotate(on);
}

void SkinManagerDialog::onCapeChanged(int row)
{
	if (!m_ctx || m_accountId.isEmpty())
		return;
	const QString capeId = ui->capeCombo->itemData(row).toString();
	if (capeId.isEmpty()) {
		m_viewer->setCape(QImage());
		return;
	}
	/* Find the cape blob whose id matches the user's pick. */
	const QByteArray idUtf8 = m_accountId.toUtf8();
	const int total =
		m_ctx->account_cape_count(m_ctx->module_handle, idUtf8.constData());
	for (int i = 0; i < total; ++i) {
		const char* cid = m_ctx->account_cape_get_id(m_ctx->module_handle,
													 idUtf8.constData(), i);
		if (!cid || QString::fromUtf8(cid) != capeId)
			continue;
		const void* ptr = nullptr;
		const int64_t len = m_ctx->account_cape_get_blob(
			m_ctx->module_handle, idUtf8.constData(), i, &ptr);
		QImage capeImg;
		if (len > 0 && ptr)
			capeImg.loadFromData(
				QByteArray::fromRawData(static_cast<const char*>(ptr),
										static_cast<int>(len)),
				"PNG");
		m_viewer->setCape(capeImg);
		return;
	}
	m_viewer->setCape(QImage());
}

/* ── commit ───────────────────────────────────────────────────────── */

void SkinManagerDialog::onAccept()
{
	if (!m_ctx || m_accountId.isEmpty()) {
		reject();
		return;
	}

	const QByteArray idUtf8 = m_accountId.toUtf8();

	/* Read the user's chosen skin bytes once up front so we don't
	 * touch the file twice on the happy path. */
	QByteArray skinBytes;
	if (!m_chosenSkinPath.isEmpty()) {
		QFile f(m_chosenSkinPath);
		if (!f.open(QIODevice::ReadOnly)) {
			setStatus(tr("Could not read %1: %2")
						  .arg(m_chosenSkinPath, f.errorString()),
					  /*error=*/true);
			return;
		}
		skinBytes = f.readAll();
	}

	/* Mirror the legacy SequentialTask: optional skin upload, then
	 * optional cape change, both routed through the S26 helpers
	 * which pump their own modal progress dialogs. */
	const QString chosenCape = ui->capeCombo->currentData().toString();
	const char* currentCapeC = m_ctx->account_get_current_cape_id(
		m_ctx->module_handle, idUtf8.constData());
	const QString currentCape =
		currentCapeC ? QString::fromUtf8(currentCapeC) : QString();
	const bool capeChanged = chosenCape != currentCape;

	if (skinBytes.isEmpty() && !capeChanged) {
		accept();
		return;
	}

	if (!skinBytes.isEmpty()) {
		const char* variant = ui->rdoSlim->isChecked() ? "ALEX" : "STEVE";
		if (m_ctx->account_skin_upload(m_ctx->module_handle, idUtf8.constData(),
									   skinBytes.constData(), skinBytes.size(),
									   variant) != 0) {
			QMessageBox::warning(this, tr("Skin Upload"),
								 tr("Failed to apply skin changes."));
			return;
		}
	}
	if (capeChanged) {
		const QByteArray capeUtf8 = chosenCape.toUtf8();
		if (m_ctx->account_cape_set(m_ctx->module_handle, idUtf8.constData(),
									capeUtf8.constData()) != 0) {
			QMessageBox::warning(this, tr("Skin Upload"),
								 tr("Failed to apply cape change."));
			return;
		}
	}

	/* Update in-memory cache via the C ABI setters so the launcher's
	 * account list re-renders immediately. */
	if (!skinBytes.isEmpty()) {
		m_ctx->account_set_skin_blob(m_ctx->module_handle, idUtf8.constData(),
									 skinBytes.constData(), skinBytes.size());
		m_ctx->account_set_skin_variant(
			m_ctx->module_handle, idUtf8.constData(),
			ui->rdoSlim->isChecked() ? "SLIM" : "CLASSIC");
	}
	if (capeChanged) {
		const QByteArray capeUtf8 = chosenCape.toUtf8();
		m_ctx->account_set_current_cape(
			m_ctx->module_handle, idUtf8.constData(), capeUtf8.constData());
	}

	QMessageBox::information(this, tr("Skin Upload"),
							 tr("Successfully applied skin changes."));
	accept();
}

/* ── status line ──────────────────────────────────────────────────── */

void SkinManagerDialog::setStatus(const QString& text, bool error)
{
	ui->statusLabel->setText(text);
	ui->statusLabel->setStyleSheet(
		error ? QStringLiteral("color: #d33; font-size: 11px;")
			  : QStringLiteral("color: palette(mid); font-size: 11px;"));
}
