// SPDX-License-Identifier: GPL-2.0-or-later
#include "PlaylistDock.hpp"
#include "RevealFile.hpp"
#include <obs-module.h>
#include <obs-hotkey.h>
#include <QAbstractItemModel>
#include <QAction>
#include <QApplication>
#include <QComboBox>
#include <QDesktopServices>
#include <QDir>
#include <QDropEvent>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QHash>
#include <QHelpEvent>
#include <QJsonDocument>
#include <QJsonObject>
#include <QItemSelectionModel>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMenu>
#include <QMimeData>
#include <QMouseEvent>
#include <QMessageBox>
#include <QPainter>
#include <QResizeEvent>
#include <QToolButton>
#include <QSaveFile>
#include <QScreen>
#include <QScrollBar>
#include <QSet>
#include <QSignalBlocker>
#include <QShortcut>
#include <QSlider>
#include <QToolTip>
#include <QStyle>
#include <QStyledItemDelegate>
#include <QStyleOptionViewItem>
#include <QStyleOptionSlider>
#include <QUrl>
#include <QUuid>
#include <QVBoxLayout>
#include <algorithm>

namespace {
constexpr const char *StateKey = "OBS-PlaylistDock";
constexpr int IdRole = Qt::UserRole;
constexpr int PathRole = Qt::UserRole + 1;
constexpr int SizeRole = Qt::UserRole + 2;
constexpr qint64 MissingFileSize = -3;

QString text(const char *utf8) { return QString::fromUtf8(utf8 ? utf8 : ""); }
QString ui(const char *key) { return text(obs_module_text(key)); }
QString localFilePath(const QString &path)
{
    const QUrl url(path);
    return url.isLocalFile() ? url.toLocalFile() : path.contains(QStringLiteral("://")) ? QString() : path;
}

QString sizeText(qint64 bytes)
{
    if (bytes == MissingFileSize)
        return ui("MissingFile");
    if (bytes < 0)
        return QStringLiteral("—");
    if (bytes < 1024)
        return QString::number(bytes) + QStringLiteral(" B");
    static const QStringList units{QStringLiteral("KiB"), QStringLiteral("MiB"), QStringLiteral("GiB"),
        QStringLiteral("TiB"), QStringLiteral("PiB"), QStringLiteral("EiB")};
    double value = double(bytes) / 1024;
    int unit = 0;
    while (value >= 1024 && unit + 1 < units.size()) {
        value /= 1024;
        ++unit;
    }
    return QString::number(value, 'f', 1) + QLatin1Char(' ') + units[unit];
}

QRect fileSizeRect(const QRect &textRect, const QFontMetrics &metrics)
{
    const int width = metrics.horizontalAdvance(QStringLiteral("1024.0 MiB"));
    return QRect(textRect.right() - width + 1, textRect.top(), width, textRect.height());
}

class SeekSlider final : public QSlider {
public:
    using QSlider::QSlider;

protected:
    void mousePressEvent(QMouseEvent *event) override
    {
        if (event->button() == Qt::LeftButton) {
            QStyleOptionSlider option;
            initStyleOption(&option);
            const QRect handle = style()->subControlRect(QStyle::CC_Slider, &option, QStyle::SC_SliderHandle, this);
            if (!handle.contains(event->position().toPoint())) {
                const QRect groove = style()->subControlRect(QStyle::CC_Slider, &option, QStyle::SC_SliderGroove, this);
                const int position = qRound(event->position().x()) - groove.left() - handle.width() / 2;
                setValue(QStyle::sliderValueFromPosition(minimum(), maximum(), position,
                    std::max(0, groove.width() - handle.width()), option.upsideDown));
            }
        }
        QSlider::mousePressEvent(event);
    }
};

class TrackDelegate final : public QStyledItemDelegate {
public:
    using QStyledItemDelegate::QStyledItemDelegate;

    void paint(QPainter *painter, const QStyleOptionViewItem &option, const QModelIndex &index) const override
    {
        QStyleOptionViewItem item(option);
        initStyleOption(&item, index);
        auto *style = item.widget ? item.widget->style() : QApplication::style();
        QRect textRect = style->subElementRect(QStyle::SE_ItemViewItemText, &item, item.widget);
        const QString title = item.text;
        item.text.clear();
        style->drawControl(QStyle::CE_ItemViewItem, &item, painter, item.widget);

        QFont numberFont(item.font);
        numberFont.setBold(true);
        const int digits = QString::number(index.model()->rowCount()).size();
        const int numberWidth = QFontMetrics(numberFont).horizontalAdvance(QString(digits, QLatin1Char('8')));
        const QRect numberRect(textRect.left(), textRect.top(), numberWidth, textRect.height());
        textRect.setLeft(numberRect.right() + 10);
        const QRect sizeRect = fileSizeRect(textRect, item.fontMetrics);
        textRect.setRight(sizeRect.left() - 12);
        const auto role = item.state.testFlag(QStyle::State_Selected) ? QPalette::HighlightedText : QPalette::Text;
        const bool enabled = item.state.testFlag(QStyle::State_Enabled);
        const qint64 bytes = index.data(SizeRole).toLongLong();
        painter->save();
        painter->setFont(numberFont);
        style->drawItemText(painter, numberRect, Qt::AlignRight | Qt::AlignVCenter, item.palette, enabled,
            QString::number(index.row() + 1), role);
        painter->setFont(bytes == MissingFileSize ? numberFont : item.font);
        style->drawItemText(painter, sizeRect, Qt::AlignRight | Qt::AlignVCenter, item.palette, enabled,
            sizeText(bytes), role);
        painter->setFont(item.font);
        style->drawItemText(painter, textRect, Qt::AlignLeft | Qt::AlignVCenter, item.palette, enabled,
            item.fontMetrics.elidedText(title, item.textElideMode, std::max(0, textRect.width())), role);
        painter->restore();
    }

    QRect sizeRect(const QStyleOptionViewItem &option, const QModelIndex &index) const
    {
        QStyleOptionViewItem item(option);
        initStyleOption(&item, index);
        auto *style = item.widget ? item.widget->style() : QApplication::style();
        return fileSizeRect(style->subElementRect(QStyle::SE_ItemViewItemText, &item, item.widget), item.fontMetrics);
    }
};

QString labelFor(const QString &path, bool playing = false)
{
    const QUrl url(path);
    const QString name = QFileInfo(url.isLocalFile() ? url.toLocalFile() : path).fileName();
    const QString label = name.isEmpty() ? path : name;
    return playing ? QStringLiteral("\u25B6\uFE0E ") + label : label;
}
QString clockText(qint64 milliseconds)
{
    const qint64 seconds = std::max<qint64>(0, milliseconds / 1000);
    return QStringLiteral("%1:%2").arg(seconds / 60).arg(seconds % 60, 2, 10, QLatin1Char('0'));
}
QStringList pathsIn(obs_data_t *settings, const char *key)
{
    QStringList paths;
    OBSDataArrayAutoRelease array = obs_data_get_array(settings, key);
    const size_t count = obs_data_array_count(array);
    paths.reserve(static_cast<qsizetype>(count));
    for (size_t i = 0; i < count; ++i) {
        OBSDataAutoRelease item = obs_data_array_item(array, i);
        const QString path = text(obs_data_get_string(item, "value"));
        if (!path.isEmpty())
            paths.append(path);
    }
    return paths;
}
obs_data_array_t *pathArray(const QStringList &paths)
{
    auto *array = obs_data_array_create();
    for (const QString &path : paths) {
        OBSDataAutoRelease item = obs_data_create();
        obs_data_set_string(item, "value", path.toUtf8().constData());
        obs_data_array_push_back(array, item);
    }
    return array;
}
void collectSceneSources(obs_source_t *source, const QString &sceneName,
    QHash<QString, QStringList> &names, QSet<obs_source_t *> &visited)
{
    if (!source || visited.contains(source))
        return;
    visited.insert(source);
    if (qstrcmp(obs_source_get_unversioned_id(source), "vlc_source") == 0) {
        names[text(obs_source_get_uuid(source))].append(sceneName);
        return;
    }
    obs_scene_t *scene = obs_group_or_scene_from_source(source);
    if (!scene)
        return;
    struct Scan {
        const QString *sceneName;
        QHash<QString, QStringList> *names;
        QSet<obs_source_t *> *visited;
    } scan{&sceneName, &names, &visited};
    obs_scene_enum_items(scene, [](obs_scene_t *, obs_sceneitem_t *item, void *context) {
        const auto &scan = *static_cast<Scan *>(context);
        collectSceneSources(obs_sceneitem_get_source(item), *scan.sceneName, *scan.names, *scan.visited);
        return true;
    }, &scan);
}
QStringList expandFolders(const QStringList &paths)
{
    // Match VLC's non-recursive folder behavior; explicit files and URLs pass through.
    const QStringList filters = QStringLiteral("*.mp3 *.wav *.flac *.aac *.ogg *.m4a *.opus *.wma *.mp4 *.mkv *.mov *.avi *.webm *.m4v *.ts *.flv *.wmv *.mpg *.mpeg").split(QLatin1Char(' '));
    QStringList result;
    for (const QString &path : paths) {
        if (!path.contains(QStringLiteral("://")) && QFileInfo(path).isDir()) {
            const QDir directory(path);
            const auto files = directory.entryInfoList(filters, QDir::Files | QDir::Readable, QDir::NoSort);
            for (const auto &file : files)
                result.append(file.absoluteFilePath());
        } else {
            result.append(path);
        }
    }
    return result;
}
}

PlaylistDock::PlaylistDock(const QString &settingsFile, QWidget *parent)
    : QWidget(parent), settingsFile_(settingsFile)
{
    setObjectName(QStringLiteral("OBS-PlaylistDock-content"));
    buildUi();
    if (!settingsFile_.isEmpty()) {
        QFile file(settingsFile_);
        if (file.open(QIODevice::ReadOnly)) {
            const auto settings = QJsonDocument::fromJson(file.readAll()).object();
            wantedUuid_ = settings.value(QStringLiteral("sourceUuid")).toString();
            remainingTime_ = settings.value(QStringLiteral("remainingTime")).toBool();
        }
    }
    updateTime(0);
    poll_.setInterval(250);
    connect(&poll_, &QTimer::timeout, this, &PlaylistDock::tick);
    poll_.start();
    save_.setSingleShot(true);
    save_.setInterval(500);
    connect(&save_, &QTimer::timeout, this, &PlaylistDock::configurationChanged);
    updateButtons();
}

PlaylistDock::~PlaylistDock() { shutdown(); }

void PlaylistDock::buildUi()
{
    setMinimumSize(360, 110);
    resize(920, 200);
    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(4, 4, 4, 4);
    layout->setSpacing(4);
    auto makeIconButton = [this](const QString &name, const QString &caption, const QString &iconClass) {
        auto *button = new QToolButton(this);
        button->setObjectName(name);
        button->setProperty("class", iconClass);
        button->setAccessibleName(caption);
        button->setIconSize(QSize(16, 16));
        button->setToolButtonStyle(Qt::ToolButtonIconOnly);
        button->setAutoRaise(true);
        button->setFixedSize(26, 26);
        return button;
    };
    heading_ = new QGridLayout;
    heading_->setSpacing(4);
    heading_->setColumnStretch(0, 2);
    heading_->setColumnStretch(1, 1);
    sourceBar_ = new QWidget(this);
    auto *sourceTools = new QHBoxLayout(sourceBar_);
    sourceTools->setContentsMargins(0, 0, 0, 0);
    sourceTools->setSpacing(4);
    count_ = new QLabel(ui("TrackCount").arg(0), this);
    count_->setObjectName(QStringLiteral("trackCount"));
    count_->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
    search_ = new QLineEdit(this);
    search_->setObjectName(QStringLiteral("search"));
    search_->setPlaceholderText(ui("SearchPlaceholder"));
    search_->setClearButtonEnabled(true);
    search_->setFixedHeight(26);
    search_->setStyleSheet(QStringLiteral("QLineEdit { margin-top: 0; margin-bottom: 0; }"));
    heading_->addWidget(sourceBar_, 0, 0);
    heading_->addWidget(search_, 0, 1);
    heading_->addWidget(count_, 0, 2);
    add_ = makeIconButton(QStringLiteral("addFiles"), ui("AddFiles"), QStringLiteral("icon-plus"));
    sourceTools->addWidget(add_);
    layout->addLayout(heading_);
    list_ = new QListWidget(this);
    list_->setObjectName(QStringLiteral("playlist"));
    list_->setItemDelegate(new TrackDelegate(list_));
    list_->setSelectionMode(QAbstractItemView::ExtendedSelection);
    list_->setDragDropMode(QAbstractItemView::InternalMove);
    list_->setDefaultDropAction(Qt::MoveAction);
    list_->setUniformItemSizes(true);
    list_->setSpacing(0);
    list_->setContextMenuPolicy(Qt::CustomContextMenu);
    list_->installEventFilter(this);
    list_->viewport()->installEventFilter(this);
    search_->installEventFilter(this);
    layout->addWidget(list_, 1);
    auto *listMenu = new QMenu(list_);
    listMenu->setObjectName(QStringLiteral("playlistMenu"));
    auto makeListAction = [listMenu](const QString &name, const QString &caption) {
        auto *action = listMenu->addAction(caption);
        action->setObjectName(name);
        return action;
    };
    up_ = makeListAction(QStringLiteral("moveUp"), ui("MoveUp"));
    down_ = makeListAction(QStringLiteral("moveDown"), ui("MoveDown"));
    top_ = makeListAction(QStringLiteral("moveTop"), ui("MoveTop"));
    bottom_ = makeListAction(QStringLiteral("moveBottom"), ui("MoveBottom"));
    listMenu->addSeparator();
    remove_ = makeListAction(QStringLiteral("remove"), ui("RemoveTrack"));
    remove_->setShortcut(QKeySequence(Qt::Key_Delete));
    remove_->setShortcutContext(Qt::WidgetWithChildrenShortcut);
    list_->addAction(remove_);
    undo_ = makeListAction(QStringLiteral("undoRemove"), ui("UndoRemove"));
    undo_->setShortcut(QKeySequence(QKeySequence::Undo));
    undo_->setShortcutContext(Qt::WidgetWithChildrenShortcut);
    list_->addAction(undo_);

    listMenu->addSeparator();
    locate_ = makeListAction(QStringLiteral("locateCurrent"), ui("LocateCurrent"));
    openFolder_ = makeListAction(QStringLiteral("openFolder"), ui("OpenFolder"));
    restore_ = listMenu->addAction(ui("RestorePlaylist"));
    restore_->setObjectName(QStringLiteral("restorePlaylist"));
    listMenu->addSeparator();
    auto *about = listMenu->addAction(ui("AboutMenu"));
    about->setObjectName(QStringLiteral("aboutPlugin"));
    connect(about, &QAction::triggered, this, [this] {
        QMessageBox::about(this, ui("AboutTitle"),
            ui("AboutText")
                .arg(ui("DockName"), QStringLiteral(PLAYLISTDOCK_VERSION)));
    });

    sources_ = new QComboBox(this);
    sources_->setObjectName(QStringLiteral("sourceSelector"));
    sources_->addItem(ui("SelectSource"), QString());
    sources_->setMinimumContentsLength(8);
    sources_->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
    sources_->setFixedHeight(26);
    sources_->setStyleSheet(QStringLiteral("QComboBox { margin-top: 0; margin-bottom: 0; }"));
    auto *refresh = makeIconButton(QStringLiteral("refresh"), ui("Refresh"), QStringLiteral("icon-refresh"));
    sourceTools->addWidget(sources_, 1);
    sourceTools->addWidget(refresh);
    message_ = new QLabel(this);
    message_->setObjectName(QStringLiteral("message"));
    message_->setTextFormat(Qt::PlainText);
    message_->setSizePolicy(QSizePolicy::Maximum, QSizePolicy::Fixed);
    message_->setMinimumWidth(0);
    message_->setMaximumWidth(160);
    message_->hide();
    auto *transport = new QHBoxLayout;
    transport->setSpacing(2);
    previous_ = makeIconButton(QStringLiteral("previous"), ui("Previous"), QStringLiteral("icon-media-prev"));
    play_ = makeIconButton(QStringLiteral("playPause"), ui("PlaySelected"), QStringLiteral("icon-media-play"));
    stop_ = makeIconButton(QStringLiteral("stop"), ui("Stop"), QStringLiteral("icon-media-stop"));
    next_ = makeIconButton(QStringLiteral("next"), ui("Next"), QStringLiteral("icon-media-next"));
    for (auto *button : {previous_, play_, stop_, next_}) {
        button->setIconSize(QSize(14, 14));
        button->setFixedSize(22, 22);
    }
    transport->addWidget(previous_);
    transport->addWidget(play_);
    transport->addWidget(stop_);
    transport->addWidget(next_);
    transport->addWidget(message_);
    seek_ = new SeekSlider(Qt::Horizontal, this);
    seek_->setObjectName(QStringLiteral("seek"));
    seek_->setRange(0, 1000);
    seek_->setMinimumWidth(80);
    transport->addWidget(seek_, 2);
    time_ = new QLabel(QStringLiteral("0:00"), this);
    time_->setObjectName(QStringLiteral("playbackTime"));
    time_->setMinimumWidth(40);
    time_->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
    time_->setCursor(Qt::PointingHandCursor);
    time_->installEventFilter(this);
    transport->addWidget(time_);
    endModes_ = new QComboBox(this);
    endModes_->setObjectName(QStringLiteral("endMode"));
    endModes_->addItem(ui("EndStop"), Stop);
    endModes_->addItem(ui("EndNext"), Next);
    endModes_->addItem(ui("EndRepeat"), Repeat);
    endModes_->setAccessibleName(ui("EndMode"));
    endModes_->setSizeAdjustPolicy(QComboBox::AdjustToContents);
    endModes_->setFixedHeight(26);
    transport->addSpacing(4);
    transport->addWidget(endModes_);
    layout->addLayout(transport);

    connect(sources_, &QComboBox::currentIndexChanged, this, [this] {
        wantedUuid_ = sources_->currentData().toString();
        bindSource(wantedUuid_);
        saveSelection();
    });
    connect(refresh, &QToolButton::clicked, this, [this] {
        refreshSources();
        if (OBSSourceAutoRelease source = lockSource()) {
            loadQueue(source);
            clearError();
        }
    });
    connect(search_, &QLineEdit::textChanged, this, &PlaylistDock::filterList);
    connect(search_, &QLineEdit::returnPressed, this, &PlaylistDock::playSelected);
    connect(list_, &QWidget::customContextMenuRequested, this, [this, listMenu](const QPoint &position) {
        QPoint menuPosition = position;
        if (list_->viewport()->rect().contains(position)) {
            auto *item = list_->itemAt(position);
            list_->setCurrentItem(item, item && item->isSelected()
                ? QItemSelectionModel::NoUpdate : QItemSelectionModel::ClearAndSelect);
        } else {
            menuPosition = list_->visualItemRect(list_->currentItem()).center();
        }
        updateButtons();
        listMenu->popup(list_->viewport()->mapToGlobal(menuPosition));
    });
    connect(list_, &QListWidget::itemDoubleClicked, this, [this](QListWidgetItem *item) {
        playTrack(item->data(IdRole).toString(), true);
    });
    for (const auto key : {Qt::Key_Return, Qt::Key_Enter}) {
        auto *shortcut = new QShortcut(QKeySequence(key), list_);
        shortcut->setContext(Qt::WidgetWithChildrenShortcut);
        connect(shortcut, &QShortcut::activated, this, &PlaylistDock::playSelected);
    }
    auto *find = new QShortcut(QKeySequence(QKeySequence::Find), this);
    find->setContext(Qt::WidgetWithChildrenShortcut);
    connect(find, &QShortcut::activated, this, [this] {
        search_->setFocus(Qt::ShortcutFocusReason);
        search_->selectAll();
    });
    connect(list_, &QListWidget::currentItemChanged, this, [this] { updateButtons(); });
    connect(list_, &QListWidget::itemSelectionChanged, this, [this] { updateButtons(); });
    connect(list_->model(), &QAbstractItemModel::rowsMoved, this, [this] { orderChanged(); });
    connect(add_, &QToolButton::clicked, this, &PlaylistDock::addFiles);
    connect(remove_, &QAction::triggered, this, &PlaylistDock::removeSelected);
    connect(undo_, &QAction::triggered, this, &PlaylistDock::undoRemove);
    connect(locate_, &QAction::triggered, this, &PlaylistDock::locateCurrent);
    connect(openFolder_, &QAction::triggered, this, &PlaylistDock::openFolder);
    connect(up_, &QAction::triggered, this, [this] { moveSelected(-1); });
    connect(down_, &QAction::triggered, this, [this] { moveSelected(1); });
    connect(top_, &QAction::triggered, this, [this] { moveSelected(-list_->currentRow()); });
    connect(bottom_, &QAction::triggered, this, [this] { moveSelected(list_->count() - 1 - list_->currentRow()); });
    connect(play_, &QToolButton::clicked, this, &PlaylistDock::togglePlay);
    connect(previous_, &QToolButton::clicked, this, [this] { playRelative(-1); });
    connect(next_, &QToolButton::clicked, this, [this] { playRelative(1); });
    connect(stop_, &QToolButton::clicked, this, &PlaylistDock::stopPlayback);
    connect(restore_, &QAction::triggered, this, &PlaylistDock::restoreSource);
    connect(endModes_, &QComboBox::currentIndexChanged, this, [this] {
        endMode_ = std::clamp(endModes_->currentData().toInt(), int(Stop), int(Repeat));
        storeQueue();
    });
    connect(seek_, &QSlider::sliderPressed, this, [this] {
        seekTarget_ = -1;
        updateTime(duration_ * seek_->value() / 1000);
    });
    connect(seek_, &QSlider::sliderMoved, this, [this](int position) {
        updateTime(duration_ * position / 1000);
    });
    connect(seek_, &QSlider::sliderReleased, this, [this] {
        if (OBSSourceAutoRelease source = lockSource(); source && duration_ > 0) {
            seekTarget_ = duration_ * seek_->value() / 1000;
            seekCommand_.restart();
            updateTime(seekTarget_);
            obs_source_media_set_time(source, seekTarget_);
        }
    });
    for (auto *widget : findChildren<QWidget *>())
        widget->setToolTip(QString());
}

void PlaylistDock::resizeEvent(QResizeEvent *event)
{
    QWidget::resizeEvent(event);
    const bool compact = event->size().width() < 600;
    if (!heading_ || compactHeader_ == compact)
        return;
    compactHeader_ = compact;
    heading_->addWidget(sourceBar_, 0, 0, 1, compact ? 3 : 1);
    heading_->addWidget(search_, compact ? 1 : 0, compact ? 0 : 1, 1, compact ? 2 : 1);
    heading_->addWidget(count_, compact ? 1 : 0, 2);
}

bool PlaylistDock::eventFilter(QObject *watched, QEvent *event)
{
    if (watched == list_->viewport() && event->type() == QEvent::ToolTip) {
        const auto *help = static_cast<QHelpEvent *>(event);
        if (const auto *item = list_->itemAt(help->pos())) {
            QStyleOptionViewItem option;
            option.initFrom(list_);
            option.widget = list_;
            option.font = list_->font();
            option.rect = list_->visualItemRect(item);
            const auto index = list_->model()->index(list_->row(item), 0);
            const QRect rect = static_cast<TrackDelegate *>(list_->itemDelegate())->sizeRect(option, index).intersected(option.rect);
            if (rect.contains(help->pos())) {
                const QString path = item->data(PathRole).toString();
                const QString local = localFilePath(path);
                QString display = local.isEmpty()
                    ? QUrl(path).adjusted(QUrl::RemoveFilename | QUrl::RemoveQuery | QUrl::RemoveFragment).toDisplayString()
                    : QDir::toNativeSeparators(QFileInfo(local).absolutePath());
                for (QChar &character : display) {
                    if (character.isSpace())
                        character = QLatin1Char(' ');
                }
                const QScreen *screen = QApplication::screenAt(help->globalPos());
                if (!screen)
                    screen = list_->screen();
                const int width = std::max(0, (screen ? screen->availableGeometry().width() : list_->width()) - 48);
                display = QFontMetrics(QToolTip::font()).elidedText(display, Qt::ElideMiddle, width);
                QToolTip::showText(help->globalPos(), QStringLiteral("<p style='white-space:pre'>%1</p>").arg(display.toHtmlEscaped()), list_->viewport(), rect);
                return true;
            }
        }
        QToolTip::hideText();
        event->ignore();
        return true;
    }
    if ((watched == search_ || watched == list_ || watched == list_->viewport()) && event->type() == QEvent::KeyPress) {
        const auto *key = static_cast<QKeyEvent *>(event);
        if (key->key() == Qt::Key_Escape && key->modifiers() == Qt::NoModifier) {
            bool reserved = false;
            obs_enum_hotkey_bindings([](void *context, size_t, obs_hotkey_binding_t *binding) {
                const auto combination = obs_hotkey_binding_get_key_combination(binding);
                if (combination.key == OBS_KEY_ESCAPE && combination.modifiers == 0)
                    *static_cast<bool *>(context) = true;
                return !*static_cast<bool *>(context);
            }, &reserved);
            if (!reserved) {
                search_->clear();
                list_->setFocus(Qt::ShortcutFocusReason);
                return true;
            }
        }
    }
    if (watched == time_ && event->type() == QEvent::MouseButtonRelease) {
        const auto *mouse = static_cast<QMouseEvent *>(event);
        if (mouse->button() == Qt::LeftButton && time_->rect().contains(mouse->position().toPoint())) {
            toggleTimeDisplay();
            return true;
        }
    }
    if (watched == list_->viewport() && (event->type() == QEvent::DragEnter ||
        event->type() == QEvent::DragMove || event->type() == QEvent::Drop)) {
        auto *drop = static_cast<QDropEvent *>(event);
        if (drop->source() != list_ && drop->source() != list_->viewport() && drop->mimeData()->hasUrls()) {
            OBSSourceAutoRelease source = lockSource();
            const auto urls = drop->mimeData()->urls();
            const bool local = std::any_of(urls.cbegin(), urls.cend(), [](const QUrl &url) { return url.isLocalFile(); });
            if (!source || suspended_ || closing_ || !local || !drop->possibleActions().testFlag(Qt::CopyAction)) {
                drop->ignore();
                return true;
            }
            if (event->type() == QEvent::Drop) {
                QStringList files;
                for (const auto &url : urls)
                    if (url.isLocalFile() && QFileInfo(url.toLocalFile()).isFile())
                        files.append(url.toLocalFile());
                if (files.isEmpty()) {
                    drop->ignore();
                    return true;
                }
                appendFiles(files);
            }
            drop->setDropAction(Qt::CopyAction);
            drop->accept();
            return true;
        }
    }
    return QWidget::eventFilter(watched, event);
}

obs_source_t *PlaylistDock::lockSource() const
{
    obs_source_t *source = obs_weak_source_get_source(source_);
    if (source && obs_source_removed(source)) {
        obs_source_release(source);
        return nullptr;
    }
    return source;
}

void PlaylistDock::resume()
{
    if (closing_)
        return;
    suspended_ = false;
    refreshSources();
}

void PlaylistDock::suspend()
{
    rememberView();
    suspended_ = true;
    cancelSwitch();
    stoppedSignal_.Disconnect();
    save_.stop();
    message_->hide();
    endArmed_ = false;
    source_ = nullptr;
    tracks_.clear();
    removedTracks_.clear();
    observed_.clear();
    currentId_.clear();
    managed_ = false;
    duration_ = 0;
    const QSignalBlocker block(sources_);
    sources_->clear();
    sources_->addItem(ui("SelectSource"), QString());
    rebuildList();
    updateTime(0);
    seek_->setValue(0);
}

void PlaylistDock::shutdown()
{
    if (closing_)
        return;
    poll_.stop();
    suspend();
    closing_ = true;
}

void PlaylistDock::refreshSources()
{
    if (suspended_ || closing_)
        return;
    QList<QPair<QString, QString>> sources;
    obs_enum_sources([](void *context, obs_source_t *source) {
        if (!obs_source_removed(source) && qstrcmp(obs_source_get_unversioned_id(source), "vlc_source") == 0)
            static_cast<QList<QPair<QString, QString>> *>(context)->append({text(obs_source_get_name(source)), text(obs_source_get_uuid(source))});
        return true;
    }, &sources);
    std::sort(sources.begin(), sources.end(), [](const auto &a, const auto &b) { return a.first.localeAwareCompare(b.first) < 0; });
    QHash<QString, QStringList> sceneNames;
    if (!sources.isEmpty()) {
        obs_enum_scenes([](void *context, obs_source_t *scene) {
            auto &names = *static_cast<QHash<QString, QStringList> *>(context);
            QSet<obs_source_t *> visited;
            collectSceneSources(scene, text(obs_source_get_name(scene)), names, visited);
            return true;
        }, &sceneNames);
        for (auto &source : sources) {
            auto names = sceneNames.value(source.second);
            if (!names.isEmpty()) {
                names.sort(Qt::CaseInsensitive);
                source.first = names.join(QStringLiteral(" / ")) + QStringLiteral(" · ") + source.first;
            }
        }
    }
    bool changed = sources_->count() != sources.size() + 1;
    for (qsizetype i = 0; !changed && i < sources.size(); ++i)
        changed = sources_->itemText(int(i) + 1) != sources[i].first || sources_->itemData(int(i) + 1).toString() != sources[i].second;
    if (changed) {
        const QSignalBlocker block(sources_);
        QSet<QString> live;
        sources_->clear();
        sources_->addItem(ui("SelectSource"), QString());
        for (const auto &item : sources) {
            sources_->addItem(item.first, item.second);
            live.insert(item.second);
        }
        for (auto it = views_.begin(); it != views_.end();) {
            if (live.contains(it.key()))
                ++it;
            else
                it = views_.erase(it);
        }
        sources_->setCurrentIndex(std::max(0, sources_->findData(wantedUuid_)));
    }
    OBSSourceAutoRelease bound = lockSource();
    const QString uuid = sources_->currentData().toString();
    if ((bound ? text(obs_source_get_uuid(bound)) : QString()) != uuid || (!bound && !tracks_.isEmpty()))
        bindSource(uuid);
}

void PlaylistDock::bindSource(const QString &uuid)
{
    rememberView();
    cancelSwitch();
    stoppedSignal_.Disconnect();
    message_->hide();
    endArmed_ = false;
    source_ = nullptr;
    tracks_.clear();
    removedTracks_.clear();
    observed_.clear();
    currentId_.clear();
    managed_ = false;
    duration_ = 0;
    OBSSourceAutoRelease source = uuid.isEmpty() ? nullptr : obs_get_source_by_uuid(uuid.toUtf8().constData());
    if (source && !obs_source_removed(source) && qstrcmp(obs_source_get_unversioned_id(source), "vlc_source") == 0) {
        source_ = OBSGetWeakRef(source);
        loadQueue(source);
        restoreView(uuid);
        stoppedSignal_.Connect(obs_source_get_signal_handler(source), "media_stopped", &PlaylistDock::mediaStopped, this);
        clearError();
    } else {
        rebuildList();
        updateTime(0);
    }
}

void PlaylistDock::rememberView()
{
    if (OBSSourceAutoRelease source = lockSource())
        views_.insert(text(obs_source_get_uuid(source)), {selectedId(), search_->text().trimmed(), list_->verticalScrollBar()->value()});
}

void PlaylistDock::restoreView(const QString &uuid)
{
    const auto saved = views_.constFind(uuid);
    if (saved == views_.cend() || saved->query != search_->text().trimmed())
        return;
    QListWidgetItem *selected = nullptr;
    for (int row = 0; row < list_->count(); ++row) {
        auto *item = list_->item(row);
        if (!item->isHidden() && item->data(IdRole).toString() == saved->selected) {
            selected = item;
            break;
        }
    }
    if (!selected && !saved->selected.isEmpty())
        return;
    const QSignalBlocker block(list_);
    list_->setCurrentItem(selected, QItemSelectionModel::ClearAndSelect);
    list_->doItemsLayout();
    list_->verticalScrollBar()->setValue(saved->scroll);
    updateButtons();
}

void PlaylistDock::loadQueue(obs_source_t *source, bool force)
{
    cancelSwitch();
    const QString selected = selectedId();
    OBSDataAutoRelease settings = obs_source_get_settings(source);
    const QStringList actual = pathsIn(settings, "playlist");
    OBSDataAutoRelease privateSettings = obs_source_get_private_settings(source);
    OBSDataAutoRelease saved = obs_data_get_obj(privateSettings, StateKey);
    const bool currentFormat = saved && obs_data_get_int(saved, "version") == 1;
    endMode_ = currentFormat ? std::clamp(int(obs_data_get_int(saved, "endMode")), int(Stop), int(Repeat)) : Stop;
    tracks_.clear();
    removedTracks_.clear();
    currentId_.clear();
    endArmed_ = false;
    bool useSaved = !force && currentFormat && pathsIn(saved, "observed") == actual;
    if (useSaved) {
        OBSDataArrayAutoRelease items = obs_data_get_array(saved, "tracks");
        const size_t count = obs_data_array_count(items);
        for (size_t i = 0; i < count; ++i) {
            OBSDataAutoRelease item = obs_data_array_item(items, i);
            const QString path = text(obs_data_get_string(item, "value"));
            const QString id = text(obs_data_get_string(item, "id"));
            if (path.isEmpty() || id.isEmpty()) {
                useSaved = false;
                break;
            }
            tracks_.append({id, path});
        }
    }
    if (useSaved) {
        managed_ = obs_data_get_bool(saved, "managed");
        originalLoop_ = obs_data_get_bool(saved, "originalLoop");
        originalShuffle_ = obs_data_get_bool(saved, "originalShuffle");
        originalBehavior_ = text(obs_data_get_string(saved, "originalBehavior"));
        currentId_ = text(obs_data_get_string(saved, "current"));
    } else {
        tracks_.clear();
        for (const QString &path : expandFolders(actual))
            tracks_.append({QUuid::createUuid().toString(QUuid::WithoutBraces), path});
        managed_ = false;
        originalLoop_ = obs_data_get_bool(settings, "loop");
        originalShuffle_ = obs_data_get_bool(settings, "shuffle");
        originalBehavior_ = text(obs_data_get_string(settings, "playback_behavior"));
    }
    if (originalBehavior_.isEmpty())
        originalBehavior_ = QStringLiteral("stop_restart");
    observed_ = actual;
    if (currentIndex() < 0)
        currentId_.clear();
    const QSignalBlocker block(endModes_);
    endModes_->setCurrentIndex(endMode_);
    rebuildList(selected);
    storeQueue();
}

void PlaylistDock::storeQueue()
{
    OBSSourceAutoRelease source = lockSource();
    if (!source || closing_)
        return;
    OBSDataAutoRelease root = obs_source_get_private_settings(source);
    OBSDataAutoRelease saved = obs_data_create();
    OBSDataArrayAutoRelease items = obs_data_array_create();
    for (const Track &track : tracks_) {
        OBSDataAutoRelease item = obs_data_create();
        obs_data_set_string(item, "value", track.path.toUtf8().constData());
        obs_data_set_string(item, "id", track.id.toUtf8().constData());
        obs_data_array_push_back(items, item);
    }
    OBSDataArrayAutoRelease observed = pathArray(observed_);
    obs_data_set_int(saved, "version", 1);
    obs_data_set_array(saved, "tracks", items);
    obs_data_set_array(saved, "observed", observed);
    obs_data_set_string(saved, "current", currentId_.toUtf8().constData());
    obs_data_set_bool(saved, "managed", managed_);
    obs_data_set_bool(saved, "originalLoop", originalLoop_);
    obs_data_set_bool(saved, "originalShuffle", originalShuffle_);
    obs_data_set_string(saved, "originalBehavior", originalBehavior_.toUtf8().constData());
    obs_data_set_int(saved, "endMode", endMode_);
    obs_data_set_obj(root, StateKey, saved);
    save_.start();
}

void PlaylistDock::rebuildList(const QString &selected)
{
    rebuilding_ = true;
    const QSignalBlocker block(list_);
    list_->setUpdatesEnabled(false);
    list_->clear();
    QHash<QString, qint64> sizes;
    for (const Track &track : tracks_)
        if (track.bytes != -2)
            sizes.insert(track.path, track.bytes);
    for (Track &track : tracks_) {
        if (track.bytes == -2) {
            auto size = sizes.constFind(track.path);
            if (size == sizes.cend()) {
                const QString local = localFilePath(track.path);
                const QFileInfo file(local);
                const qint64 bytes = local.isEmpty() ? -1 : file.isFile() ? file.size() : MissingFileSize;
                size = sizes.insert(track.path, bytes);
            }
            track.bytes = *size;
        }
        auto *item = new QListWidgetItem(labelFor(track.path, track.id == currentId_), list_);
        item->setData(IdRole, track.id);
        item->setData(PathRole, track.path);
        item->setData(SizeRole, track.bytes);
        if (track.bytes == MissingFileSize)
            item->setData(Qt::AccessibleDescriptionRole, ui("FileMissing").arg(track.path));
        if (track.id == selected)
            list_->setCurrentItem(item, QItemSelectionModel::ClearAndSelect);
    }
    list_->setUpdatesEnabled(true);
    rebuilding_ = false;
    filterList();
}

void PlaylistDock::filterList()
{
    QToolTip::hideText();
    const QSignalBlocker block(list_);
    int visible = 0;
    const QString query = search_->text().trimmed();
    const bool byNumber = query.startsWith(QLatin1Char('#'));
    bool numberValid = false;
    const int number = byNumber ? query.mid(1).toInt(&numberValid) : 0;
    QListWidgetItem *firstMatch = nullptr;
    for (int row = 0; row < list_->count(); ++row) {
        auto *item = list_->item(row);
        const bool match = byNumber ? numberValid && row + 1 == number
            : item->data(PathRole).toString().contains(query, Qt::CaseInsensitive);
        item->setHidden(!match);
        visible += match;
        if (match && !firstMatch)
            firstMatch = item;
    }
    if (!query.isEmpty() || !list_->currentItem()) {
        list_->setCurrentItem(firstMatch, QItemSelectionModel::ClearAndSelect);
        if (firstMatch)
            list_->scrollToItem(firstMatch);
    }
    list_->setDragEnabled(query.isEmpty());
    count_->setText(query.isEmpty() ? ui("TrackCount").arg(tracks_.size()) : ui("FilteredCount").arg(visible).arg(tracks_.size()));
    updateButtons();
}

QString PlaylistDock::selectedId() const
{
    const auto *item = list_->currentItem();
    return item && !item->isHidden() ? item->data(IdRole).toString() : QString();
}

int PlaylistDock::currentIndex() const
{
    if (currentId_.isEmpty())
        return -1;
    for (qsizetype i = 0; i < tracks_.size(); ++i)
        if (tracks_[i].id == currentId_)
            return int(i);
    return -1;
}

void PlaylistDock::orderChanged()
{
    if (rebuilding_)
        return;
    tracks_.clear();
    for (int i = 0; i < list_->count(); ++i) {
        const auto *item = list_->item(i);
        tracks_.append({item->data(IdRole).toString(), item->data(PathRole).toString(), item->data(SizeRole).toLongLong()});
    }
    storeQueue();
    updateButtons();
    clearError();
}

void PlaylistDock::moveSelected(int offset)
{
    const int row = list_->currentRow();
    if (list_->selectedItems().size() != 1 || !search_->text().trimmed().isEmpty() ||
        row < 0 || row + offset < 0 || row + offset >= list_->count())
        return;
    rebuilding_ = true;
    auto *item = list_->takeItem(row);
    list_->insertItem(row + offset, item);
    list_->setCurrentItem(item, QItemSelectionModel::ClearAndSelect);
    rebuilding_ = false;
    orderChanged();
}

void PlaylistDock::addFiles()
{
    OBSSourceAutoRelease original = lockSource();
    if (!original)
        return;
    const auto files = QFileDialog::getOpenFileNames(this, ui("AddMediaTitle"), QString(), ui("MediaFileFilter"));
    OBSSourceAutoRelease current = lockSource();
    if (!current || original != current)
        return;
    appendFiles(files);
}

void PlaylistDock::appendFiles(const QStringList &files)
{
    OBSSourceAutoRelease source = lockSource();
    if (!source || suspended_ || closing_ || files.isEmpty())
        return;
    for (const QString &path : files)
        tracks_.append({QUuid::createUuid().toString(QUuid::WithoutBraces), path});
    {
        const QSignalBlocker block(search_);
        search_->clear();
    }
    rebuildList(tracks_.last().id);
    list_->scrollToItem(list_->currentItem());
    storeQueue();
    clearError();
}

void PlaylistDock::removeSelected()
{
    OBSSourceAutoRelease source = lockSource();
    if (!source || suspended_ || closing_)
        return;
    QSet<QString> ids;
    for (const auto *item : list_->selectedItems())
        if (!item->isHidden())
            ids.insert(item->data(IdRole).toString());
    if (ids.isEmpty())
        return;
    QList<Track> remaining;
    remaining.reserve(tracks_.size() - ids.size());
    removedTracks_.clear();
    for (int row = 0; row < tracks_.size(); ++row) {
        if (ids.contains(tracks_[row].id))
            removedTracks_.append({tracks_[row], row});
        else
            remaining.append(tracks_[row]);
    }
    if (removedTracks_.isEmpty())
        return;
    const int row = removedTracks_.first().row;
    tracks_ = std::move(remaining);
    const bool clearPlayback = ids.contains(currentId_) || tracks_.isEmpty();
    if (clearPlayback) {
        managed_ = true;
        currentId_.clear();
        stopPlayback();
    }
    rebuildList(tracks_.isEmpty() ? QString() : tracks_[std::min(row, int(tracks_.size()) - 1)].id);
    if (!clearPlayback)
        storeQueue();
    clearError();
}

void PlaylistDock::undoRemove()
{
    OBSSourceAutoRelease source = lockSource();
    if (!source || suspended_ || closing_ || removedTracks_.isEmpty())
        return;
    QList<Track> restored;
    restored.reserve(tracks_.size() + removedTracks_.size());
    int next = 0;
    for (const auto &entry : removedTracks_) {
        while (next < tracks_.size() && restored.size() < entry.row)
            restored.append(tracks_[next++]);
        restored.append(entry.track);
    }
    while (next < tracks_.size())
        restored.append(tracks_[next++]);
    const QString selected = removedTracks_.first().track.id;
    removedTracks_.clear();
    tracks_ = std::move(restored);
    rebuildList(selected);
    storeQueue();
    clearError();
}

void PlaylistDock::locateCurrent()
{
    if (currentIndex() < 0)
        return;
    search_->clear();
    for (int row = 0; row < list_->count(); ++row) {
        auto *item = list_->item(row);
        if (item->data(IdRole).toString() == currentId_) {
            list_->setCurrentItem(item, QItemSelectionModel::ClearAndSelect);
            list_->scrollToItem(item, QAbstractItemView::PositionAtCenter);
            list_->setFocus(Qt::OtherFocusReason);
            break;
        }
    }
    clearError();
}

void PlaylistDock::openFolder()
{
    OBSSourceAutoRelease source = lockSource();
    const auto *item = list_->currentItem();
    if (!source || suspended_ || closing_ || !item || item->isHidden() || !item->isSelected() || list_->selectedItems().size() != 1)
        return;
    const QString local = localFilePath(item->data(PathRole).toString());
    if (local.isEmpty())
        return;
    const QFileInfo file(local);
    const QString folder = file.absolutePath();
    if (file.isFile() && revealFile(file.absoluteFilePath())) {
        clearError();
        return;
    }
    if (!QDir(folder).exists() || !QDesktopServices::openUrl(QUrl::fromLocalFile(folder)))
        showError(ui("OpenFolderError").arg(folder));
    else
        clearError();
}

void PlaylistDock::playTrack(const QString &id, bool switchScene)
{
    OBSSourceAutoRelease source = lockSource();
    const auto it = std::find_if(tracks_.cbegin(), tracks_.cend(), [&id](const Track &track) { return track.id == id; });
    if (!source || it == tracks_.cend())
        return;
    const QString path = it->path;
    const QString local = localFilePath(path);
    if (!local.isEmpty() && !QFileInfo(local).isFile()) {
        endArmed_ = false;
        showError(ui("FileMissing").arg(path));
        return;
    }
    endArmed_ = false;
    seekTarget_ = -1;
    switchSceneOnStart_ = switchScene;
    const QString previousId = currentId_;
    currentId_ = id;
    managed_ = true;
    observed_ = {path};
    lastCommand_.restart();
    storeQueue();
    OBSDataAutoRelease settings = obs_data_create();
    OBSDataArrayAutoRelease single = pathArray(observed_);
    obs_data_set_array(settings, "playlist", single);
    obs_data_set_bool(settings, "loop", false);
    obs_data_set_bool(settings, "shuffle", false);
    obs_data_set_string(settings, "playback_behavior", "always_play");
    switching_ = true;
    {
        std::lock_guard<std::mutex> lock(switchMutex_);
        ++switchGeneration_;
        pendingPlayback_ = settings;
        // Publish the target and stop as one handoff.
        obs_source_media_stop(source);
    }
    for (int row = 0; row < list_->count(); ++row) {
        auto *item = list_->item(row);
        if (item->data(IdRole).toString() == id) {
            item->setText(labelFor(path, true));
            list_->setCurrentItem(item, QItemSelectionModel::ClearAndSelect);
        } else if (item->data(IdRole).toString() == previousId) {
            item->setText(labelFor(item->data(PathRole).toString()));
        }
    }
    updateButtons();
    clearError();
}

void PlaylistDock::mediaStopped(void *context, calldata_t *data)
{
    auto *dock = static_cast<PlaylistDock *>(context);
    auto *source = static_cast<obs_source_t *>(calldata_ptr(data, "source"));
    quint64 generation;
    {
        std::lock_guard<std::mutex> lock(dock->switchMutex_);
        if (!dock->pendingPlayback_ || !source || obs_source_removed(source))
            return;
        generation = dock->switchGeneration_;
        // Clear the audio clock after the old decoder stops.
        const bool decoupled = obs_source_async_decoupled(source);
        obs_source_set_async_decoupled(source, true);
        if (!decoupled)
            obs_source_set_async_decoupled(source, false);
        // Apply after VLC stops, on the OBS media thread.
        obs_source_update(source, dock->pendingPlayback_);
        dock->pendingPlayback_ = nullptr;
    }
    QMetaObject::invokeMethod(dock, [dock, generation] {
        if (generation != dock->switchGeneration_)
            return;
        dock->switching_ = false;
#ifdef _WIN32
        if (OBSSourceAutoRelease source = dock->lockSource()) {
            const auto monitoring = obs_source_get_monitoring_type(source);
            if (monitoring != OBS_MONITORING_TYPE_NONE) {
                // Start monitoring with an empty delay queue on the UI thread.
                obs_source_set_monitoring_type(source, OBS_MONITORING_TYPE_NONE);
                obs_source_set_monitoring_type(source, monitoring);
            }
        }
#endif
        dock->lastCommand_.restart();
        dock->updateButtons();
        if (dock->switchSceneOnStart_)
            emit dock->sceneSwitchRequested(dock->wantedUuid_);
    }, Qt::QueuedConnection);
}

void PlaylistDock::cancelSwitch()
{
    seekTarget_ = -1;
    const bool wasSwitching = switching_;
    {
        std::lock_guard<std::mutex> lock(switchMutex_);
        ++switchGeneration_;
        pendingPlayback_ = nullptr;
    }
    switching_ = false;
    switchSceneOnStart_ = false;
    if (wasSwitching) {
        if (OBSSourceAutoRelease source = lockSource()) {
            OBSDataAutoRelease settings = obs_source_get_settings(source);
            const QStringList actual = pathsIn(settings, "playlist");
            if (actual != observed_)
                currentId_.clear();
            observed_ = actual;
            storeQueue();
        }
    }
}

void PlaylistDock::playSelected()
{
    const QString id = selectedId();
    if (!id.isEmpty())
        playTrack(id, true);
}

void PlaylistDock::playRelative(int offset)
{
    if (tracks_.isEmpty())
        return;
    const int current = currentIndex();
    const int next = current < 0 ? (offset > 0 ? 0 : int(tracks_.size()) - 1) : (current + offset + int(tracks_.size())) % int(tracks_.size());
    playTrack(tracks_[next].id);
}

void PlaylistDock::togglePlay()
{
    OBSSourceAutoRelease source = lockSource();
    if (!source)
        return;
    const QString selected = selectedId();
    if (!selected.isEmpty() && selected != currentId_) {
        playTrack(selected);
        return;
    }
    const auto state = obs_source_media_get_state(source);
    if (state == OBS_MEDIA_STATE_PLAYING)
        obs_source_media_play_pause(source, true);
    else if (state == OBS_MEDIA_STATE_PAUSED)
        obs_source_media_play_pause(source, false);
    else if (!currentId_.isEmpty())
        playTrack(currentId_);
    updateButtons();
}

void PlaylistDock::stopPlayback()
{
    cancelSwitch();
    endArmed_ = false;
    if (OBSSourceAutoRelease source = lockSource()) {
        obs_source_media_stop(source);
        if (managed_) {
            // Cancel a playlist still waiting for its deferred update.
            observed_.clear();
            storeQueue();
            OBSDataAutoRelease settings = obs_data_create();
            OBSDataArrayAutoRelease empty = obs_data_array_create();
            obs_data_set_array(settings, "playlist", empty);
            obs_source_update(source, settings);
        }
    }
    clearError();
    updateButtons();
}

void PlaylistDock::restoreSource()
{
    cancelSwitch();
    OBSSourceAutoRelease source = lockSource();
    if (!source)
        return;
    QStringList paths;
    for (const Track &track : tracks_)
        paths.append(track.path);
    endArmed_ = false;
    managed_ = false;
    currentId_.clear();
    observed_ = paths;
    storeQueue();
    OBSDataAutoRelease settings = obs_source_get_settings(source);
    OBSDataArrayAutoRelease array = pathArray(paths);
    obs_data_set_array(settings, "playlist", array);
    obs_data_set_bool(settings, "loop", originalLoop_);
    obs_data_set_bool(settings, "shuffle", originalShuffle_);
    obs_data_set_string(settings, "playback_behavior", originalBehavior_.toUtf8().constData());
    obs_source_update(source, settings);
    rebuildList(selectedId());
    clearError();
}

void PlaylistDock::tick()
{
    if (suspended_ || closing_)
        return;
    if (++pollCount_ % 4 == 0)
        refreshSources();
    OBSSourceAutoRelease source = lockSource();
    if (!source)
        return;
    if (switching_) {
        updateButtons();
        return;
    }
    if (pollCount_ % 4 == 0) {
        OBSDataAutoRelease settings = obs_source_get_settings(source);
        if (pathsIn(settings, "playlist") != observed_) {
            loadQueue(source, true);
            clearError();
        }
    }
    const auto playback = obs_source_media_get_state(source);
    duration_ = std::max<qint64>(0, obs_source_media_get_duration(source));
    const qint64 position = std::max<qint64>(0, obs_source_media_get_time(source));
    if (seekTarget_ >= 0 && (seekCommand_.elapsed() >= 1000 ||
        qAbs(position - seekTarget_) <= std::max<qint64>(100, duration_ / 1000)))
        seekTarget_ = -1;
    if (!seek_->isSliderDown() && seekTarget_ < 0) {
        const QSignalBlocker block(seek_);
        seek_->setValue(duration_ > 0 ? int(std::clamp<qint64>(position * 1000 / duration_, 0, 1000)) : 0);
        updateTime(position);
    }
    const int current = currentIndex();
    QString status;
    switch (playback) {
    case OBS_MEDIA_STATE_PLAYING: status = ui("StatusPlaying"); break;
    case OBS_MEDIA_STATE_PAUSED: status = ui("StatusPaused"); break;
    case OBS_MEDIA_STATE_OPENING:
    case OBS_MEDIA_STATE_BUFFERING: status = ui("StatusLoading"); break;
    case OBS_MEDIA_STATE_ENDED: status = ui("StatusEnded"); break;
    case OBS_MEDIA_STATE_ERROR: status = ui("StatusError"); break;
    default: status = ui("StatusStopped"); break;
    }
    const QString progress = status + QStringLiteral(" · ") + clockText(position) + QStringLiteral(" / ") + (duration_ > 0 ? clockText(duration_) : QStringLiteral("—:—"));
    time_->setAccessibleDescription(progress);
    updateButtons();
    if (!managed_ || current < 0)
        return;
    if (playback == OBS_MEDIA_STATE_PLAYING && (!lastCommand_.isValid() || lastCommand_.elapsed() >= 100))
        endArmed_ = true;
    else if (playback == OBS_MEDIA_STATE_STOPPED || playback == OBS_MEDIA_STATE_ERROR)
        endArmed_ = false;
    else if (playback == OBS_MEDIA_STATE_ENDED && endArmed_) {
        endArmed_ = false;
        if (endMode_ == Repeat)
            playTrack(currentId_);
        else if (endMode_ == Next && current + 1 < tracks_.size())
            playTrack(tracks_[current + 1].id);
        else
            stopPlayback();
    }
}

void PlaylistDock::updateButtons()
{
    OBSSourceAutoRelease source = lockSource();
    const bool bound = source && !suspended_ && !closing_;
    const int row = list_->currentRow();
    const QString selection = selectedId();
    const bool selected = !selection.isEmpty();
    const auto items = list_->selectedItems();
    const int selections = int(std::count_if(items.cbegin(), items.cend(),
        [](const QListWidgetItem *item) { return !item->isHidden(); }));
    const bool unfiltered = search_->text().trimmed().isEmpty();
    const auto state = bound ? obs_source_media_get_state(source) : OBS_MEDIA_STATE_NONE;
    play_->setEnabled(bound && !switching_ && (selected || !currentId_.isEmpty()));
    stop_->setEnabled(bound);
    previous_->setEnabled(bound && !tracks_.isEmpty());
    next_->setEnabled(bound && !tracks_.isEmpty());
    add_->setEnabled(bound);
    remove_->setEnabled(bound && selections > 0);
    undo_->setEnabled(bound && !removedTracks_.isEmpty());
    locate_->setEnabled(bound && currentIndex() >= 0);
    openFolder_->setEnabled(bound && selected && selections == 1 &&
        list_->currentItem()->isSelected() && !localFilePath(list_->currentItem()->data(PathRole).toString()).isEmpty());
    up_->setEnabled(bound && selections == 1 && unfiltered && row > 0);
    down_->setEnabled(bound && selections == 1 && unfiltered && row + 1 < list_->count());
    top_->setEnabled(up_->isEnabled());
    bottom_->setEnabled(down_->isEnabled());
    restore_->setEnabled(bound && (managed_ || !tracks_.isEmpty()));
    endModes_->setEnabled(bound);
    seek_->setEnabled(bound && !switching_ && duration_ > 0);
    const bool selectedOther = selected && selection != currentId_;
    const bool pause = !selectedOther && state == OBS_MEDIA_STATE_PLAYING;
    const QString caption = pause ? ui("Pause") : selectedOther ? ui("PlaySelected") : state == OBS_MEDIA_STATE_PAUSED ? ui("Resume") : ui("Play");
    play_->setAccessibleName(caption);
    const QString iconClass = pause ? QStringLiteral("icon-media-pause") : QStringLiteral("icon-media-play");
    if (play_->property("class").toString() != iconClass) {
        play_->setProperty("class", iconClass);
        play_->style()->unpolish(play_);
        play_->style()->polish(play_);
        play_->update();
    }
}

void PlaylistDock::updateTime(qint64 position)
{
    const QString stamp = remainingTime_ ? QStringLiteral("−") +
        (duration_ > 0 ? clockText(std::max<qint64>(0, duration_ - position)) : QStringLiteral("—:—"))
        : clockText(position);
    time_->setText(stamp);
    time_->setAccessibleName((remainingTime_ ? ui("RemainingTime") : ui("ElapsedTime")) + QLatin1Char(' ') + stamp);
}

void PlaylistDock::toggleTimeDisplay()
{
    remainingTime_ = !remainingTime_;
    OBSSourceAutoRelease source = lockSource();
    const qint64 position = seek_->isSliderDown() ? duration_ * seek_->value() / 1000
        : seekTarget_ >= 0 ? seekTarget_ : source ? obs_source_media_get_time(source) : 0;
    updateTime(position);
    saveSelection();
}

void PlaylistDock::saveSelection()
{
    if (settingsFile_.isEmpty())
        return;
    if (!QDir().mkpath(QFileInfo(settingsFile_).absolutePath())) {
        showError(ui("CreateSettingsError"));
        return;
    }
    QSaveFile file(settingsFile_);
    if (!file.open(QIODevice::WriteOnly)) {
        showError(ui("SaveSettingsError").arg(file.errorString()));
        return;
    }
    const QByteArray contents = QJsonDocument(QJsonObject{
        {QStringLiteral("sourceUuid"), wantedUuid_}, {QStringLiteral("remainingTime"), remainingTime_}
    }).toJson(QJsonDocument::Compact);
    if (file.write(contents) != contents.size() || !file.commit())
        showError(ui("SaveSettingsError").arg(file.errorString()));
}

void PlaylistDock::clearError()
{
    message_->clear();
    message_->hide();
}

void PlaylistDock::showError(const QString &value)
{
    message_->setText(QStringLiteral("⚠ ") + value);
    message_->show();
    blog(LOG_WARNING, "[OBS-PlaylistDock] %s", value.toUtf8().constData());
}
