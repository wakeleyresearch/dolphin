/*
 * SPDX-FileCopyrightText: 2009 Peter Penz <peter.penz19@gmail.com>
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "informationpanelcontent.h"

#include <KConfigGroup>
#include <KIO/PreviewJob>
#include <KIconEffect>
#include <KIconLoader>
#include <KIconUtils>
#include <KJobWidgets>
#include <KLocalizedString>
#include <KSeparator>
#include <KSharedConfig>
#include <KStringHandler>
#include <QPainterPath>

#include <QIcon>
#include <QStyle>
#include <QTextDocument>

#include <Baloo/FileMetaDataWidget>

#include <QDialogButtonBox>
#include <QFutureWatcher>
#include <QGesture>
#include <QImage>
#include <QLabel>
#include <QLinearGradient>
#include <QPainter>
#include <QPolygon>
#include <QScrollArea>
#include <QScroller>
#include <QTextLayout>
#include <QThreadPool>
#include <QTimer>
#include <QVBoxLayout>
#include <QtConcurrent>

#include <poppler-qt6.h>

#include <memory>

#include "dolphin_informationpanelsettings.h"
#include "mediawidget.h"
#include "pixmapviewer.h"

const int PLAY_ARROW_SIZE = 24;
const int PLAY_ARROW_BORDER_SIZE = 2;
// Longest side, in device pixels, that a first-page render is allowed to reach.
const int PDF_PREVIEW_MAX_DEVICE_PIXELS = 4096;

namespace
{
QThreadPool *pdfRenderPool()
{
    static QThreadPool pool;
    pool.setMaxThreadCount(1);
    return &pool;
}

QImage renderPdfFirstPage(const QString &path, const QSize &logicalSize, qreal dpr)
{
    if (path.isEmpty() || logicalSize.isEmpty()) {
        return {};
    }

    dpr = qMax(qreal(1), dpr);
    std::unique_ptr<Poppler::Document> document = Poppler::Document::load(path);
    if (!document || document->isLocked()) {
        return {};
    }

    document->setRenderHint(Poppler::Document::Antialiasing, true);
    document->setRenderHint(Poppler::Document::TextAntialiasing, true);

    std::unique_ptr<Poppler::Page> page = document->page(0);
    if (!page) {
        return {};
    }

    const QSizeF points = page->pageSizeF();
    if (points.isEmpty()) {
        return {};
    }

    int targetW = qMax(1, qRound(logicalSize.width() * dpr));
    int targetH = qMax(1, qRound(logicalSize.height() * dpr));
    const qreal capScale = qMin(qreal(1), qMin(qreal(PDF_PREVIEW_MAX_DEVICE_PIXELS) / targetW, qreal(PDF_PREVIEW_MAX_DEVICE_PIXELS) / targetH));
    targetW = qMax(1, qRound(targetW * capScale));
    targetH = qMax(1, qRound(targetH * capScale));

    const qreal scale = qMin(targetW / points.width(), targetH / points.height());
    if (!(scale > 0)) {
        return {};
    }

    QImage image = page->renderToImage(72.0 * scale, 72.0 * scale);
    if (image.isNull()) {
        return {};
    }

    // Logical size stays the fitted panel size, so the painter does not scale it.
    image.setDevicePixelRatio(dpr * capScale);
    return image;
}
}

InformationPanelContent::InformationPanelContent(QWidget *parent)
    : QWidget(parent)
    , m_item()
    , m_previewJob(nullptr)
    , m_outdatedPreviewTimer(nullptr)
    , m_preview(nullptr)
    , m_mediaWidget(nullptr)
    , m_nameLabel(nullptr)
    , m_metaDataWidget(nullptr)
    , m_metaDataArea(nullptr)
    , m_isVideo(false)
{
    parent->installEventFilter(this);

    // Initialize timer for disabling an outdated preview with a small
    // delay. This prevents flickering if the new preview can be generated
    // within a very small timeframe.
    m_outdatedPreviewTimer = new QTimer(this);
    m_outdatedPreviewTimer->setInterval(100);
    m_outdatedPreviewTimer->setSingleShot(true);
    connect(m_outdatedPreviewTimer, &QTimer::timeout, this, &InformationPanelContent::markOutdatedPreview);

    QVBoxLayout *layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);

    // preview
    const int minPreviewWidth = KIconLoader::SizeEnormous + KIconLoader::SizeMedium;

    m_preview = new PixmapViewer(parent);
    m_preview->setMinimumWidth(minPreviewWidth);
    m_preview->setMinimumHeight(KIconLoader::SizeEnormous);
    m_preview->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);

    m_mediaWidget = new MediaWidget(parent);
    m_mediaWidget->hide();
    m_mediaWidget->setMinimumWidth(minPreviewWidth);
    m_mediaWidget->setAutoPlay(InformationPanelSettings::previewsAutoPlay());
    connect(m_mediaWidget, &MediaWidget::hasVideoChanged, this, &InformationPanelContent::slotHasVideoChanged);

    // name
    m_nameLabel = new QTextEdit(parent);
    QFont font = m_nameLabel->font();
    font.setBold(true);
    m_nameLabel->setFont(font);
    m_nameLabel->setAlignment(Qt::AlignHCenter);
    m_nameLabel->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Fixed);
    m_nameLabel->setTextInteractionFlags(Qt::TextSelectableByMouse);
    m_nameLabel->setFrameShape(QFrame::NoFrame);
    m_nameLabel->setStyleSheet("background-color: transparent; padding: 0px;");
    m_nameLabel->setContentsMargins(0, 0, 0, 0); 
    m_nameLabel->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    m_nameLabel->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    QTextDocument *doc = m_nameLabel->document();
    QTextOption option = doc->defaultTextOption();
    option.setWrapMode(QTextOption::WrapAnywhere);
    doc->setDefaultTextOption(option);

    const bool previewsShown = InformationPanelSettings::previewsShown();
    m_preview->setVisible(previewsShown);

    m_metaDataWidget = new Baloo::FileMetaDataWidget(parent);
    m_metaDataWidget->setDateFormat(static_cast<Baloo::DateFormats>(InformationPanelSettings::dateFormat()));
    connect(m_metaDataWidget, &Baloo::FileMetaDataWidget::urlActivated, this, &InformationPanelContent::urlActivated);
    m_metaDataWidget->setFont(QFontDatabase::systemFont(QFontDatabase::SmallestReadableFont));
    m_metaDataWidget->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Minimum);

    // Configuration
    m_configureLabel = new QLabel(i18nc("@label::textbox", "Select which data should be shown:"), this);
    m_configureLabel->setWordWrap(true);
    m_configureLabel->setVisible(false);

    m_configureButtons = new QDialogButtonBox(QDialogButtonBox::Save | QDialogButtonBox::Cancel);
    m_configureButtons->setVisible(false);
    connect(m_configureButtons, &QDialogButtonBox::accepted, this, [this]() {
        m_metaDataWidget->setConfigurationMode(Baloo::ConfigurationMode::Accept);
        m_configureButtons->setVisible(false);
        m_configureLabel->setVisible(false);
        Q_EMIT configurationFinished();
    });
    connect(m_configureButtons, &QDialogButtonBox::rejected, this, [this]() {
        m_metaDataWidget->setConfigurationMode(Baloo::ConfigurationMode::Cancel);
        m_configureButtons->setVisible(false);
        m_configureLabel->setVisible(false);
        Q_EMIT configurationFinished();
    });

    m_metaDataArea = new QScrollArea(parent);
    m_metaDataArea->setWidget(m_metaDataWidget);
    m_metaDataArea->setWidgetResizable(true);
    m_metaDataArea->setFrameShape(QFrame::NoFrame);

    QWidget *viewport = m_metaDataArea->viewport();
    QScroller::grabGesture(viewport, QScroller::TouchGesture);
    viewport->installEventFilter(this);

    // The panel is the first page only. Name, separator, and metadata stay
    // constructed so the rest of the panel code still has them, but they are
    // not placed in the layout.
    layout->addWidget(m_preview, 1);
    layout->addWidget(m_mediaWidget, 1);
    m_nameLabel->hide();
    m_metaDataArea->hide();
    m_configureLabel->hide();
    m_configureButtons->hide();

    grabGesture(Qt::TapAndHoldGesture);
}

InformationPanelContent::~InformationPanelContent()
{
    InformationPanelSettings::self()->save();
}

void InformationPanelContent::showItem(const KFileItem &item)
{
    // compares item entries, comparing items only compares urls
    if (m_item.entry() != item.entry()) {
        m_item = item;
        m_preview->stopAnimatedImage();
        refreshMetaData();
    }

    refreshPreview();
}

void InformationPanelContent::refreshPixmapView()
{
    // If there is a preview job, kill it to prevent that we have jobs for
    // multiple items running, and thus a race condition (bug 250787).
    if (m_previewJob) {
        m_previewJob->kill();
    }

    // Reset disabled state when starting a new preview job
    m_disabledPreviewUrl.clear();

    if (startPdfPreview()) {
        return;
    }
    m_pdfPreviewPath.clear();
    m_pdfPreviewSize = QSize();
    ++m_pdfGeneration;
    startThumbnailPreview();
}

bool InformationPanelContent::startPdfPreview()
{
    if (!m_item.isLocalFile()) {
        return false;
    }

    const QString mimeType = m_item.mimetype();
    if (mimeType != QLatin1String("application/pdf") && mimeType != QLatin1String("application/x-pdf")) {
        return false;
    }

    const QString path = m_item.localPath();
    if (path.isEmpty()) {
        return false;
    }

    // KIO's thumbnail cache tops out around 1024px and the panel then
    // stretches that bitmap. Draw the first page at the panel's own pixels.
    QSize requestSize = m_preview->size();
    requestSize = requestSize.expandedTo(QSize(1, 1));
    if (path == m_pdfPreviewPath && requestSize == m_pdfPreviewSize) {
        return true;
    }

    ++m_pdfGeneration;
    m_pdfPreviewPath = path;
    m_pdfPreviewSize = requestSize;
    const qreal dpr = devicePixelRatioF();
    const quint64 generation = m_pdfGeneration;

    // Keep the current frame until the sharp page arrives. Greying it for
    // the duration of the render just flashes a soft image.
    m_outdatedPreviewTimer->stop();

    auto *watcher = new QFutureWatcher<QImage>(this);
    connect(watcher, &QFutureWatcher<QImage>::finished, this, [this, watcher, generation]() {
        const QImage image = watcher->result();
        watcher->deleteLater();
        if (generation != m_pdfGeneration) {
            return;
        }
        if (image.isNull()) {
            if (generation == m_pdfGeneration) {
                m_pdfPreviewPath.clear();
                m_pdfPreviewSize = QSize();
            }
            startThumbnailPreview();
            return;
        }
        m_outdatedPreviewTimer->stop();
        m_disabledPreviewUrl.clear();
        m_preview->setPixmap(QPixmap::fromImage(image));
    });
    watcher->setFuture(QtConcurrent::run(pdfRenderPool(), [path, requestSize, dpr]() {
        return renderPdfFirstPage(path, requestSize, dpr);
    }));
    return true;
}

void InformationPanelContent::startThumbnailPreview()
{
    const quint64 generation = m_pdfGeneration;
    m_outdatedPreviewTimer->start();

    const KConfigGroup globalConfig(KSharedConfig::openConfig(), "PreviewSettings");
    const QStringList plugins = globalConfig.readEntry("Plugins", KIO::PreviewJob::defaultPlugins());
    QSize requestSize = m_preview->size();
    if (QWidget *panel = parentWidget()) {
        requestSize = requestSize.expandedTo(panel->contentsRect().size());
    }
    requestSize = requestSize.expandedTo(QSize(1, 1));
    m_previewJob = new KIO::PreviewJob(KFileItemList() << m_item, requestSize, &plugins);
    m_previewJob->setScaleType(KIO::PreviewJob::Unscaled);
    m_previewJob->setIgnoreMaximumSize(m_item.isLocalFile() && !m_item.isSlow());
    m_previewJob->setDevicePixelRatio(devicePixelRatioF());
    if (m_previewJob->uiDelegate()) {
        KJobWidgets::setWindow(m_previewJob, this);
    }

    connect(m_previewJob.data(), &KIO::PreviewJob::gotPreview, this, [this, generation](const KFileItem &item, const QPixmap &pixmap) {
        if (generation != m_pdfGeneration) {
            return;
        }
        showPreview(item, pixmap);
    });
    connect(m_previewJob.data(), &KIO::PreviewJob::failed, this, [this, generation](const KFileItem &item) {
        if (generation != m_pdfGeneration) {
            return;
        }
        showIcon(item);
    });
}

void InformationPanelContent::refreshPreview()
{
    // If there is a preview job, kill it to prevent that we have jobs for
    // multiple items running, and thus a race condition (bug 250787).
    if (m_previewJob) {
        m_previewJob->kill();
    }

    m_preview->setCursor(Qt::ArrowCursor);
    setNameLabelText(m_item.text());
    if (InformationPanelSettings::previewsShown()) {
        const QUrl itemUrl = m_item.url();
        const bool isSearchUrl = itemUrl.scheme().contains(QLatin1String("search")) && m_item.localPath().isEmpty();
        if (isSearchUrl) {
            m_preview->show();
            m_mediaWidget->hide();

            // in the case of a search-URL the URL is not readable for humans
            // (at least not useful to show in the Information Panel)
            m_preview->setPixmap(QIcon::fromTheme(QStringLiteral("baloo")).pixmap(m_preview->height(), m_preview->width()));
        } else {
            refreshPixmapView();

            const QString mimeType = m_item.mimetype();
            const bool isAnimatedImage = m_preview->isAnimatedMimeType(mimeType);
            m_isVideo = !isAnimatedImage && mimeType.startsWith(QLatin1String("video/"));
            bool useMedia = m_isVideo || mimeType.startsWith(QLatin1String("audio/"));

            if (useMedia) {
                // change the cursor of the preview
                m_preview->setCursor(Qt::PointingHandCursor);
                m_preview->installEventFilter(m_mediaWidget);

                m_mediaWidget->show();

                // if the video is playing, has been paused or stopped
                // we don't need to update the preview/media widget states
                // unless the previewed file has changed,
                // or the setting previewshown has changed
                if ((m_mediaWidget->state() != QMediaPlayer::PlayingState && m_mediaWidget->state() != QMediaPlayer::PausedState
                     && m_mediaWidget->state() != QMediaPlayer::StoppedState)
                    || m_item.targetUrl() != m_mediaWidget->url() || (!m_preview->isVisible() && !m_mediaWidget->isVisible())) {
                    if (InformationPanelSettings::previewsAutoPlay() && m_isVideo) {
                        // hides the preview now to avoid flickering when the autoplay video starts
                        m_preview->hide();
                    } else {
                        // the video won't play before the preview is displayed
                        m_preview->show();
                    }

                    m_mediaWidget->setUrl(m_item.targetUrl(), m_isVideo ? MediaWidget::MediaKind::Video : MediaWidget::MediaKind::Audio);
                    adjustWidgetSizes(parentWidget()->width());
                }
            } else {
                if (isAnimatedImage) {
                    m_preview->setAnimatedImageFileName(itemUrl.toLocalFile());
                }
                // When we don't need it, hide the media widget first to avoid flickering
                m_mediaWidget->hide();
                m_preview->show();
                m_preview->removeEventFilter(m_mediaWidget);
                m_mediaWidget->clearUrl();
            }
        }
    } else {
        m_preview->stopAnimatedImage();
        m_preview->hide();
        m_mediaWidget->hide();
    }
}

void InformationPanelContent::configureShownProperties()
{
    m_configureLabel->setVisible(true);
    m_configureButtons->setVisible(true);
    m_metaDataWidget->setConfigurationMode(Baloo::ConfigurationMode::ReStart);
}

void InformationPanelContent::refreshMetaData()
{
    m_metaDataWidget->setDateFormat(static_cast<Baloo::DateFormats>(InformationPanelSettings::dateFormat()));
    m_metaDataWidget->show();
    m_metaDataWidget->setItems(KFileItemList() << m_item);
}

void InformationPanelContent::showItems(const KFileItemList &items)
{
    // If there is a preview job, kill it to prevent that we have jobs for
    // multiple items running, and thus a race condition (bug 250787).
    if (m_previewJob) {
        m_previewJob->kill();
    }

    m_preview->stopAnimatedImage();

    m_preview->setPixmap(QIcon::fromTheme(QStringLiteral("dialog-information")).pixmap(m_preview->height(), m_preview->width()));
    setNameLabelText(i18ncp("@label", "%1 item selected", "%1 items selected", items.count()));

    m_metaDataWidget->setItems(items);

    m_mediaWidget->hide();

    m_item = KFileItem();
}

bool InformationPanelContent::eventFilter(QObject *obj, QEvent *event)
{
    switch (event->type()) {
    case QEvent::Resize: {
        QResizeEvent *resizeEvent = static_cast<QResizeEvent *>(event);
        if (obj == m_metaDataArea->viewport()) {
            // The size of the meta text area has changed. Adjust the fixed
            // width in a way that no horizontal scrollbar needs to be shown.
            m_metaDataWidget->setFixedWidth(resizeEvent->size().width());
        } else if (obj == parent()) {
            adjustWidgetSizes(resizeEvent->size().width());
        }
        break;
    }

    case QEvent::Polish:
        adjustWidgetSizes(parentWidget()->width());
        break;

    case QEvent::FontChange:
        m_metaDataWidget->setFont(QFontDatabase::systemFont(QFontDatabase::SmallestReadableFont));
        break;

    default:
        break;
    }

    return QWidget::eventFilter(obj, event);
}

bool InformationPanelContent::event(QEvent *event)
{
    if (event->type() == QEvent::Gesture) {
        gestureEvent(static_cast<QGestureEvent *>(event));
        return true;
    }
    return QWidget::event(event);
}

bool InformationPanelContent::gestureEvent(QGestureEvent *event)
{
    if (!underMouse()) {
        return false;
    }

    QTapAndHoldGesture *tap = static_cast<QTapAndHoldGesture *>(event->gesture(Qt::TapAndHoldGesture));

    if (tap) {
        if (tap->state() == Qt::GestureFinished) {
            Q_EMIT contextMenuRequested(tap->position().toPoint());
        }
        event->accept();
        return true;
    }
    return false;
}

void InformationPanelContent::showIcon(const KFileItem &item)
{
    m_outdatedPreviewTimer->stop();
    QIcon icon = QIcon::fromTheme(item.iconName());
    QPixmap pixmap = KIconUtils::addOverlays(icon, item.overlays()).pixmap(m_preview->size(), devicePixelRatioF());
    pixmap.setDevicePixelRatio(devicePixelRatioF());
    m_preview->setPixmap(pixmap);
    m_disabledPreviewUrl.clear();
}

void InformationPanelContent::showPreview(const KFileItem &item, const QPixmap &pixmap)
{
    m_outdatedPreviewTimer->stop();

    QPixmap p = pixmap;
    if (!item.overlays().isEmpty()) {
        // Avoid scaling the images that are smaller than the preview size, to be consistent when there is no overlays
        if (pixmap.height() < m_preview->height() && pixmap.width() < m_preview->width()) {
            p = QPixmap(m_preview->size() * devicePixelRatioF());
            p.fill(Qt::transparent);
            p.setDevicePixelRatio(devicePixelRatioF());

            QPainter painter(&p);
            painter.drawPixmap(QPointF{m_preview->width() / 2.0 - pixmap.width() / pixmap.devicePixelRatioF() / 2,
                                       m_preview->height() / 2.0 - pixmap.height() / pixmap.devicePixelRatioF() / 2}
                                   .toPoint(),
                               pixmap);
        }
        p = KIconUtils::addOverlays(p, item.overlays()).pixmap(m_preview->size(), devicePixelRatioF());
        p.setDevicePixelRatio(devicePixelRatioF());
    }

    if (m_isVideo) {
        // adds a play arrow overlay

        auto maxDim = qMax(p.width(), p.height());
        auto arrowSize = qMax(PLAY_ARROW_SIZE, maxDim / 8);

        // compute relative pixel positions
        const int zeroX = static_cast<int>((p.width() / 2 - arrowSize / 2) / p.devicePixelRatio());
        const int zeroY = static_cast<int>((p.height() / 2 - arrowSize / 2) / p.devicePixelRatio());

        QPolygon arrow;
        arrow << QPoint(zeroX, zeroY);
        arrow << QPoint(zeroX, zeroY + arrowSize);
        arrow << QPoint(zeroX + arrowSize, zeroY + arrowSize / 2);

        QPainterPath path;
        path.addPolygon(arrow);

        QLinearGradient gradient(QPointF(zeroX, zeroY + arrowSize / 2), QPointF(zeroX + arrowSize, zeroY + arrowSize / 2));

        QColor whiteColor = Qt::white;
        QColor blackColor = Qt::black;
        gradient.setColorAt(0, whiteColor);
        gradient.setColorAt(1, blackColor);

        QBrush brush(gradient);

        QPainter painter(&p);

        QPen pen(blackColor, PLAY_ARROW_BORDER_SIZE, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin);
        painter.setPen(pen);

        painter.setRenderHint(QPainter::Antialiasing);
        painter.drawPolygon(arrow);
        painter.fillPath(path, brush);
    }

    m_preview->setPixmap(p);
}

void InformationPanelContent::markOutdatedPreview()
{
    if (m_item.isDir()) {
        // directory preview can be long
        // but since we always have icons to display
        // use it until the preview is done
        showIcon(m_item);
    } else {
        // Only apply disabled effect once per URL to avoid repeated brightening
        if (m_disabledPreviewUrl == m_item.url()) {
            return;
        }
        m_disabledPreviewUrl = m_item.url();

        QPixmap disabledPixmap = m_preview->pixmap();
        if (!disabledPixmap.isNull()) {
            KIconEffect::toDisabled(disabledPixmap);
            m_preview->setPixmap(disabledPixmap);
        }
    }
}

KFileItemList InformationPanelContent::items()
{
    return m_metaDataWidget->items();
}

void InformationPanelContent::slotHasVideoChanged(bool hasVideo)
{
    m_preview->setVisible(InformationPanelSettings::previewsShown() && !hasVideo);
    if (m_preview->isVisible() && m_preview->size().width() != m_preview->pixmap().size().width()) {
        // in case the information panel has been resized when the preview was not displayed
        // we need to refresh its content
        refreshPixmapView();
    }
}

void InformationPanelContent::setPreviewAutoPlay(bool autoPlay)
{
    m_mediaWidget->setAutoPlay(autoPlay);
}

void InformationPanelContent::setNameLabelText(const QString &text)
{
    QTextOption textOption;
    textOption.setWrapMode(QTextOption::WrapAtWordBoundaryOrAnywhere);

    const QString processedText = Qt::mightBeRichText(text) ? text : KStringHandler::preProcessWrap(text);

    m_nameLabel->setText(processedText);
    m_nameLabel->setFixedHeight(m_nameLabel->document()->size().height());
    m_nameLabel->setAlignment(Qt::AlignHCenter);
}

void InformationPanelContent::adjustWidgetSizes(int width)
{
    const int maxWidth = qMax(1, width);
    const int maxHeight = qMax(1, parentWidget() ? parentWidget()->height() : maxWidth);
    m_nameLabel->setMaximumWidth(maxWidth);
    m_metaDataWidget->setMaximumWidth(maxWidth);

    // Fill the dock in both directions. The dock itself tracks the window.
    m_preview->setSizeHint(QSize(maxWidth, maxHeight));
    m_mediaWidget->setVideoSize(QSize(maxWidth, maxHeight));
}

#include "moc_informationpanelcontent.cpp"
