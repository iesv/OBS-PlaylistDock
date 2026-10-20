// SPDX-License-Identifier: GPL-2.0-or-later
#include "PlaylistDock.hpp"
#include "RevealFile.hpp"
#include <obs-module.h>
#include <obs-hotkey.h>
#include <QAction>
#include <QApplication>
#include <QComboBox>
#include <QCoreApplication>
#include <QDesktopServices>
#include <QDir>
#include <QDropEvent>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QHelpEvent>
#include <QLabel>
#include <QLineEdit>
#include <QMouseEvent>
#include <QKeyEvent>
#include <QListWidget>
#include <QMenu>
#include <QMimeData>
#include <QSignalBlocker>
#include <QAbstractButton>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSlider>
#include <QScreen>
#include <QScrollBar>
#include <QTemporaryDir>
#include <QTextDocument>
#include <QThread>
#include <QToolTip>
#include <QUuid>
#ifdef _WIN32
#include <windows.h>
#include <psapi.h>
#include <objbase.h>
#else
#include <mach/mach.h>
#endif
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <atomic>
#include <array>
#include <vector>
#include <mutex>

static lookup_t *testLocale = nullptr;
static QString revealedPath;
static int revealCalls = 0;
static bool revealSuccess = true;

bool revealFile(const QString &path)
{
    revealedPath = path;
    ++revealCalls;
    return revealSuccess;
}

class FolderHandler final : public QObject {
    Q_OBJECT
public:
    QUrl url;
public slots:
    void open(const QUrl &value) { url = value; }
};

const char *obs_module_text(const char *key)
{
    const char *value = key;
    text_lookup_getstr(testLocale, key, &value);
    return value;
}

namespace {
struct FakeMedia {
    std::atomic<obs_media_state> state{OBS_MEDIA_STATE_STOPPED};
    std::atomic<qint64> position{0};
    std::atomic<qint64> duration{60000};
    std::atomic<qint64> started{0};
    std::atomic<bool> stoppedBeforeUpdate{false};
};
qint64 nowMs() { return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count(); }
std::vector<FakeMedia *> fakeMedia;
std::mutex fakeMediaMutex;
struct ProgramAudio {
    std::atomic<uint64_t> nonSilent{0};
    std::atomic<bool> measuring{false};
    std::atomic<uint64_t> measured{0};
    std::atomic<uint64_t> silent{0};
    std::atomic<float> maxStep{0};
    float previous = 0;
    bool havePrevious = false;

    static void capture(void *context, size_t, audio_data *audio)
    {
        auto &probe = *static_cast<ProgramAudio *>(context);
        const auto *samples = reinterpret_cast<const float *>(audio->data[0]);
        if (!samples)
            return;
        bool audible = false;
        for (uint32_t i = 0; i < audio->frames; ++i)
            audible |= std::isfinite(samples[i]) && std::abs(samples[i]) > 0.00001f;
        if (audible)
            probe.nonSilent.fetch_add(audio->frames, std::memory_order_relaxed);
        if (!probe.measuring.load()) {
            probe.havePrevious = false;
            return;
        }
        probe.measured.fetch_add(audio->frames);
        if (!audible)
            probe.silent.fetch_add(audio->frames);
        float step = probe.maxStep.load();
        for (uint32_t i = 0; i < audio->frames; ++i) {
            if (probe.havePrevious)
                step = (std::max)(step, std::abs(samples[i] - probe.previous));
            probe.previous = samples[i];
            probe.havePrevious = true;
        }
        probe.maxStep.store(step);
    }
};
void check(bool condition, const char *message)
{
    if (!condition) {
        std::fprintf(stderr, "FAIL: %s\n", message);
        std::exit(1);
    }
}
void pump(int milliseconds = 0)
{
    QElapsedTimer elapsed;
    elapsed.start();
    do {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
        if (milliseconds)
            QThread::msleep(5);
    } while (elapsed.elapsed() < milliseconds);
    QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
}
template<class Predicate> void waitFor(Predicate ready, const char *message)
{
    QElapsedTimer elapsed;
    elapsed.start();
    while (!ready() && elapsed.elapsed() < 5000)
        pump(20);
    check(ready(), message);
}
template<class T> T *control(PlaylistDock &dock, const char *name)
{
    auto *value = dock.findChild<T *>(QString::fromLatin1(name));
    check(value != nullptr, name);
    return value;
}
void activate(PlaylistDock &dock, const char *name)
{
    if (auto *action = dock.findChild<QAction *>(QString::fromLatin1(name))) {
        if (action->isEnabled())
            action->trigger();
    } else {
        control<QAbstractButton>(dock, name)->click();
    }
}
quint64 memoryBytes()
{
#ifdef _WIN32
    PROCESS_MEMORY_COUNTERS_EX counters{};
    counters.cb = sizeof(counters);
    GetProcessMemoryInfo(GetCurrentProcess(), reinterpret_cast<PROCESS_MEMORY_COUNTERS *>(&counters), sizeof(counters));
    return counters.PrivateUsage;
#else
    mach_task_basic_info_data_t info{};
    mach_msg_type_number_t count = MACH_TASK_BASIC_INFO_COUNT;
    task_info(mach_task_self(), MACH_TASK_BASIC_INFO, reinterpret_cast<task_info_t>(&info), &count);
    return info.resident_size;
#endif
}
void setPlaylist(obs_source_t *source, const QStringList &paths)
{
    OBSDataAutoRelease settings = obs_source_get_settings(source);
    OBSDataArrayAutoRelease array = obs_data_array_create();
    for (const QString &path : paths) {
        OBSDataAutoRelease item = obs_data_create();
        obs_data_set_string(item, "value", path.toUtf8().constData());
        obs_data_array_push_back(array, item);
    }
    obs_data_set_array(settings, "playlist", array);
    obs_source_update(source, settings);
}
QStringList playlist(obs_source_t *source)
{
    QStringList result;
    OBSDataAutoRelease settings = obs_source_get_settings(source);
    OBSDataArrayAutoRelease array = obs_data_get_array(settings, "playlist");
    for (size_t i = 0; i < obs_data_array_count(array); ++i) {
        OBSDataAutoRelease item = obs_data_array_item(array, i);
        result.append(QString::fromUtf8(obs_data_get_string(item, "value")));
    }
    return result;
}
size_t savedCount(obs_source_t *source)
{
    OBSDataAutoRelease root = obs_source_get_private_settings(source);
    OBSDataAutoRelease saved = obs_data_get_obj(root, "OBS-PlaylistDock");
    OBSDataArrayAutoRelease array = obs_data_get_array(saved, "tracks");
    return obs_data_array_count(array);
}
void bind(PlaylistDock &dock, obs_source_t *source)
{
    dock.resume();
    dock.refreshSources();
    auto *combo = control<QComboBox>(dock, "sourceSelector");
    const int index = combo->findData(QString::fromUtf8(obs_source_get_uuid(source)));
    check(index > 0, "source must appear in the selector");
    combo->setCurrentIndex(index);
    pump();
}
void doubleClick(PlaylistDock &dock, int row)
{
    auto *list = control<QListWidget>(dock, "playlist");
    check(row >= 0 && row < list->count(), "double click row exists");
    list->setCurrentRow(row);
    check(QMetaObject::invokeMethod(list, "itemDoubleClicked", Qt::DirectConnection, Q_ARG(QListWidgetItem *, list->item(row))), "double click signal");
    pump(75);
    waitFor([&] { return control<QAbstractButton>(dock, "playPause")->isEnabled(); }, "functional double click completes its media handoff");
}
void doubleClickPath(PlaylistDock &dock, const QString &path)
{
    auto *list = control<QListWidget>(dock, "playlist");
    for (int row = 0; row < list->count(); ++row) {
        if (list->item(row)->data(Qt::UserRole + 1).toString() == path) {
            doubleClick(dock, row);
            return;
        }
    }
    check(false, "requested playback file exists in the dock");
}
QString mediaTitle(obs_source_t *source)
{
    calldata_t data;
    calldata_init(&data);
    calldata_set_string(&data, "tag_id", "title");
    proc_handler_call(obs_source_get_proc_handler(source), "get_metadata", &data);
    const QString title = QString::fromUtf8(calldata_string(&data, "tag_data"));
    calldata_free(&data);
    return title;
}

void keyPress(QWidget *widget, int key, Qt::KeyboardModifiers modifiers = Qt::NoModifier)
{
    QKeyEvent press(QEvent::KeyPress, key, modifiers);
    QApplication::sendEvent(widget, &press);
    QKeyEvent release(QEvent::KeyRelease, key, modifiers);
    QApplication::sendEvent(widget, &release);
    pump(100);
}
void registerFake()
{
    obs_source_info info{};
    info.id = "vlc_source";
    info.type = OBS_SOURCE_TYPE_INPUT;
    info.output_flags = OBS_SOURCE_ASYNC_VIDEO | OBS_SOURCE_CONTROLLABLE_MEDIA;
    info.get_name = [](void *) { return "Stress VLC"; };
    info.create = [](obs_data_t *, obs_source_t *) -> void * {
        auto *media = new FakeMedia;
        std::lock_guard<std::mutex> lock(fakeMediaMutex);
        fakeMedia.push_back(media);
        return media;
    };
    info.destroy = [](void *data) {
        auto *media = static_cast<FakeMedia *>(data);
        std::lock_guard<std::mutex> lock(fakeMediaMutex);
        fakeMedia.erase(std::find(fakeMedia.begin(), fakeMedia.end(), media));
        delete media;
    };
    info.update = [](void *data, obs_data_t *settings) {
        auto *media = static_cast<FakeMedia *>(data);
        media->stoppedBeforeUpdate = media->state == OBS_MEDIA_STATE_STOPPED;
        OBSDataArrayAutoRelease files = obs_data_get_array(settings, "playlist");
        const bool play = obs_data_array_count(files) > 0 && qstrcmp(obs_data_get_string(settings, "playback_behavior"), "always_play") == 0;
        media->state = play ? OBS_MEDIA_STATE_PLAYING : OBS_MEDIA_STATE_STOPPED;
        media->position = 0;
        media->started = nowMs();
    };
    info.media_get_state = [](void *data) { return static_cast<FakeMedia *>(data)->state.load(); };
    info.media_get_duration = [](void *data) -> int64_t { return static_cast<FakeMedia *>(data)->duration.load(); };
    info.media_get_time = [](void *data) -> int64_t {
        const auto *media = static_cast<FakeMedia *>(data);
        return media->position + (media->state == OBS_MEDIA_STATE_PLAYING ? nowMs() - media->started : 0);
    };
    info.media_set_time = [](void *data, int64_t position) {
        auto *media = static_cast<FakeMedia *>(data);
        media->position = position;
        media->started = nowMs();
    };
    info.media_restart = [](void *data) {
        auto *media = static_cast<FakeMedia *>(data);
        media->position = 0;
        media->started = nowMs();
        media->state = OBS_MEDIA_STATE_PLAYING;
    };
    info.media_play_pause = [](void *data, bool pause) {
        auto *media = static_cast<FakeMedia *>(data);
        if (pause && media->state == OBS_MEDIA_STATE_PLAYING)
            media->position += nowMs() - media->started;
        media->started = nowMs();
        media->state = pause ? OBS_MEDIA_STATE_PAUSED : OBS_MEDIA_STATE_PLAYING;
    };
    info.media_stop = [](void *data) {
        auto *media = static_cast<FakeMedia *>(data);
        media->state = OBS_MEDIA_STATE_STOPPED;
        media->position = 0;
    };
    obs_register_source(&info);
}
void featureSuite(const QStringList &files)
{
    OBSSourceAutoRelease source = obs_source_create("vlc_source", "Feature A", nullptr, nullptr);
    OBSSourceAutoRelease other = obs_source_create("vlc_source", "Feature B", nullptr, nullptr);
    const QStringList original{files[0], files[1], files[0], files[2]};
    setPlaylist(source, original);
    setPlaylist(other, {files[1]});
    QTemporaryDir temporary;
    check(temporary.isValid(), "create time-preference test directory");
    const QString settings = temporary.filePath(QStringLiteral("settings.json"));
    PlaylistDock dock{settings};
    bind(dock, source);
    dock.show();
    dock.activateWindow();
    pump(50);
    auto *list = control<QListWidget>(dock, "playlist");
    auto *search = control<QLineEdit>(dock, "search");
    auto *time = control<QLabel>(dock, "playbackTime");
    auto *count = control<QLabel>(dock, "trackCount");
    check(list->item(0)->data(Qt::UserRole + 2).toLongLong() == QFileInfo(files[0]).size(), "list items contain the local file size");
    check(count->geometry().left() >= search->geometry().right() &&
        qAbs(count->geometry().center().y() - search->geometry().center().y()) <= 2, "track count is after search at the far right of the top row");
    auto *sourceSelector = control<QComboBox>(dock, "sourceSelector");
    dock.resize(480, 200);
    pump(30);
    check(search->y() >= sourceSelector->mapTo(&dock, QPoint(0, sourceSelector->height())).y() &&
        count->x() >= search->geometry().right(), "narrow docks put search and count on a second row");
    dock.resize(920, 200);
    pump(30);
    check(qAbs(search->geometry().center().y() - sourceSelector->mapTo(&dock, sourceSelector->rect().center()).y()) <= 2,
        "widening restores a single header row");
    search->setText(QStringLiteral("#3"));
    list->setFocus();
    const auto find = QKeySequence(QKeySequence::Find)[0];
    keyPress(list, find.key(), find.keyboardModifiers());
    check(search->hasFocus() && search->selectedText() == search->text(), "platform Find shortcut focuses and selects the search text");
    const auto hotkey = obs_hotkey_register_frontend("test.escape", "Test Escape", [](void *, obs_hotkey_id, obs_hotkey_t *, bool) {}, nullptr);
    obs_key_combination_t escape{0, OBS_KEY_ESCAPE};
    obs_hotkey_load_bindings(hotkey, &escape, 1);
    keyPress(search, Qt::Key_Escape);
    check(search->text() == QStringLiteral("#3"), "an existing OBS Escape binding takes precedence");
    obs_hotkey_unregister(hotkey);
    keyPress(search, Qt::Key_Escape);
    check(search->text().isEmpty() && list->hasFocus(), "unbound local Escape clears search and returns to the list");

    auto drop = [&](const QList<QUrl> &urls) {
        QMimeData mime;
        mime.setUrls(urls);
        QDragEnterEvent enter(QPoint(10, 10), Qt::CopyAction | Qt::MoveAction, &mime, Qt::LeftButton, Qt::NoModifier);
        QApplication::sendEvent(list->viewport(), &enter);
        QDropEvent event(QPointF(10, 10), Qt::CopyAction | Qt::MoveAction, &mime, Qt::LeftButton, Qt::NoModifier);
        QApplication::sendEvent(list->viewport(), &event);
        return event.isAccepted() && event.dropAction() == Qt::CopyAction;
    };
    search->setText(QStringLiteral("#1"));
    check(drop({QUrl::fromLocalFile(files[2]), QUrl::fromLocalFile(files[0]), QUrl::fromLocalFile(files[2])}), "external file drops use copy semantics");
    check(list->count() == 7 && search->text().isEmpty() && list->currentRow() == 6, "dropped files are appended and made visible");
    for (int i = 0; i < 3; ++i)
        check(list->item(i + 4)->data(Qt::UserRole + 1).toString() == (i == 1 ? files[0] : files[2]), "file drops preserve order and duplicates");
    check(QFileInfo::exists(files[2]) && !drop({QUrl::fromLocalFile(temporary.path())}), "dragging never moves original files and ignores directories");

    QStringList ids;
    for (int row = 0; row < list->count(); ++row)
        ids.append(list->item(row)->data(Qt::UserRole).toString());
    doubleClick(dock, 1);
    const QStringList playing = playlist(source);
    int sceneRequests = 0;
    QObject::connect(&dock, &PlaylistDock::sceneSwitchRequested, &dock, [&](const QString &) { ++sceneRequests; });
    list->setCurrentRow(0, QItemSelectionModel::ClearAndSelect);
    list->item(2)->setSelected(true);
    QMetaObject::invokeMethod(list, "customContextMenuRequested", Qt::DirectConnection,
        Q_ARG(QPoint, list->visualItemRect(list->item(2)).center()));
    check(list->selectedItems().size() == 2, "right-clicking a selected entry preserves the batch selection");
    control<QMenu>(dock, "playlistMenu")->hide();
    activate(dock, "remove");
    check(list->count() == 5 && playlist(source) == playing, "batch removal of other entries preserves playback");
    list->setFocus();
    const auto undo = QKeySequence(QKeySequence::Undo)[0];
    keyPress(list, undo.key(), undo.keyboardModifiers());
    check(list->count() == ids.size() && playlist(source) == playing && !control<QAction>(dock, "undoRemove")->isEnabled(), "one-level Undo restores the batch without restarting playback");
    for (int row = 0; row < list->count(); ++row)
        check(list->item(row)->data(Qt::UserRole).toString() == ids[row], "Undo restores positions and distinct identities of duplicate paths");
    search->setText(QStringLiteral("#7"));
    activate(dock, "locateCurrent");
    check(search->text().isEmpty() && list->currentRow() == 1, "locating playback clears filtering and selects the playing entry");
    check(list->hasFocus(), "locating playback returns focus to the list");
    check(sceneRequests == 0 && playlist(source) == playing, "locating playback does not replay or switch scenes");
    list->item(2)->setSelected(true);
    activate(dock, "remove");
    waitFor([&] { return playlist(source).isEmpty() && obs_source_media_get_state(source) == OBS_MEDIA_STATE_STOPPED; },
        "removing the playing entry in a batch stops playback");
    activate(dock, "undoRemove");
    check(list->count() == 7 && playlist(source).isEmpty(), "Undo after removing playback does not resume it");

    pump(300);
    auto clickTime = [&] {
        const QPointF position(time->rect().center());
        for (const auto type : {QEvent::MouseButtonPress, QEvent::MouseButtonRelease}) {
            QMouseEvent event(type, position, QPointF(time->mapToGlobal(position.toPoint())), Qt::LeftButton,
                type == QEvent::MouseButtonPress ? Qt::LeftButton : Qt::NoButton, Qt::NoModifier);
            QApplication::sendEvent(time, &event);
        }
    };
    check(!time->text().startsWith(QStringLiteral("−")), "time defaults to elapsed");
    clickTime();
    check(time->text().startsWith(QStringLiteral("−")), "clicking the time selects remaining duration");
    {
        PlaylistDock restored{settings};
        check(control<QLabel>(restored, "playbackTime")->text().startsWith(QStringLiteral("−")), "time display preference survives reopening");
    }
    clickTime();
    check(!time->text().startsWith(QStringLiteral("−")), "clicking again returns to elapsed");
    auto *endMode = control<QComboBox>(dock, "endMode");
    endMode->setCurrentIndex(2);
    bind(dock, other);
    check(endMode->currentIndex() == 0, "new VLC sources default to stop-at-end");
    endMode->setCurrentIndex(0);
    bind(dock, source);
    check(endMode->currentIndex() == 2, "each VLC source retains its own playback mode");
    dock.suspend();
    bind(dock, source);
    check(endMode->currentIndex() == 2, "playback mode survives reopening the source");
    setPlaylist(other, {files[2], files[0]});
    bind(dock, other);
    check(endMode->currentIndex() == 0, "another source keeps stop-at-end independently");
    endMode->setCurrentIndex(1);
    setPlaylist(other, {files[0]});
    activate(dock, "refresh");
    check(endMode->currentIndex() == 1, "external playlist edits preserve that source's playback mode");
    bind(dock, source);
    check(endMode->currentIndex() == 2, "other source edits do not alter repeat mode");
    {
        OBSDataAutoRelease serialized = obs_save_source(source);
        obs_data_set_string(serialized, "uuid", QUuid::createUuid().toString(QUuid::WithoutBraces).toUtf8().constData());
        obs_data_set_string(serialized, "name", "Playback mode roundtrip");
        OBSSourceAutoRelease loaded = obs_load_source(serialized);
        check(loaded != nullptr, "reload source playback mode from scene data");
        PlaylistDock restoredMode{QString()};
        bind(restoredMode, loaded);
        check(control<QComboBox>(restoredMode, "endMode")->currentIndex() == 2, "scene serialization preserves the source's repeat setting");
        restoredMode.shutdown();
        obs_source_remove(loaded);
    }
    const QString sizePath = temporary.filePath(QStringLiteral("size.dat"));
    {
        QFile file(sizePath);
        check(file.open(QIODevice::WriteOnly) && file.resize(1536), "create a file-size fixture");
    }
    check(drop({QUrl::fromLocalFile(sizePath), QUrl::fromLocalFile(sizePath)}), "append duplicate paths for file-size caching");
    {
        QFile file(sizePath);
        check(file.open(QIODevice::WriteOnly) && file.resize(4096), "change the file size externally");
    }
    activate(dock, "moveUp");
    activate(dock, "remove");
    activate(dock, "undoRemove");
    for (int row = 7; row < 9; ++row)
        check(list->item(row)->data(Qt::UserRole + 2).toLongLong() == 1536, "reordering and Undo reuse cached sizes without querying files");
    activate(dock, "refresh");
    for (int row = 7; row < 9; ++row)
        check(list->item(row)->data(Qt::UserRole + 2).toLongLong() == 4096, "explicit Refresh updates cached sizes including duplicate paths");
    list->setCurrentRow(0, QItemSelectionModel::ClearAndSelect);
    activate(dock, "remove");
    bind(dock, other);
    check(!control<QAction>(dock, "undoRemove")->isEnabled() && !drop({QUrl(QStringLiteral("https://example.org/test.mp3"))}), "Undo cannot cross sources and remote URL drops are ignored");
    dock.suspend();
    check(!drop({QUrl::fromLocalFile(files[0])}), "file drops require a live bound source");
    dock.shutdown();
    obs_source_remove(source);
    obs_source_remove(other);
    std::printf("COMPACT_UI drop_order_duplicates_batch_remove_undo_find_escape_conflict_locate_time_preference_source_modes=PASS\n");
}

void fileSuite()
{
    QTemporaryDir temporary;
    check(temporary.isValid(), "create local-file feature fixtures");
    const QString directory = temporary.filePath(QStringLiteral("音 & 乐"));
    check(QDir().mkpath(directory), "create a Unicode folder with HTML-sensitive characters");
    const QString path = QDir(directory).filePath(QStringLiteral("测试 & file,#1.wav"));
    const QString missing = temporary.filePath(QStringLiteral("missing <track>.wav"));
    auto writeFile = [&] {
        QFile file(path);
        check(file.open(QIODevice::WriteOnly) && file.write("audio") == 5, "create a local-file fixture");
    };
    writeFile();
    OBSSourceAutoRelease source = obs_source_create("vlc_source", "File features", nullptr, nullptr);
    check(source != nullptr, "create a source for local-file checks");
    setPlaylist(source, {path, QUrl::fromLocalFile(path).toString(), missing, QStringLiteral("https://example.org/media.mp3")});
    PlaylistDock dock{QString()};
    bind(dock, source);
    dock.show();
    pump(30);
    FolderHandler handler;
    QDesktopServices::setUrlHandler(QStringLiteral("file"), &handler, "open");
    auto *list = control<QListWidget>(dock, "playlist");
    auto *folder = control<QAction>(dock, "openFolder");
    check(list->item(0)->data(Qt::UserRole + 2).toLongLong() == 5 && list->item(1)->data(Qt::UserRole + 2).toLongLong() == 5,
        "local paths and file URLs retain their file sizes");
    check(list->item(2)->data(Qt::AccessibleDescriptionRole).toString().contains(missing), "missing files have an accessible description");
    check(list->item(3)->data(Qt::UserRole + 2).toLongLong() == -1 && list->item(3)->data(Qt::AccessibleDescriptionRole).toString().isEmpty(),
        "remote media is not marked as a missing local file");
    list->setCurrentRow(0, QItemSelectionModel::ClearAndSelect);
    check(folder->isEnabled(), "one local file enables Open containing folder");
    activate(dock, "openFolder");
    check(revealedPath == QFileInfo(path).absoluteFilePath() && handler.url.isEmpty() && control<QLabel>(dock, "message")->isHidden(),
        "Open containing folder reveals the selected Unicode file without an ordinary notification");
    list->setCurrentRow(1, QItemSelectionModel::ClearAndSelect);
    activate(dock, "openFolder");
    check(revealedPath == QFileInfo(path).absoluteFilePath(), "file URLs reveal the decoded local file");
    revealSuccess = false;
    activate(dock, "openFolder");
    check(handler.url.toLocalFile() == QFileInfo(path).absolutePath() && control<QLabel>(dock, "message")->isHidden(),
        "a failed reveal launch falls back to the containing folder silently");
    revealSuccess = true;
    handler.url.clear();
    list->setCurrentRow(0, QItemSelectionModel::ClearAndSelect);
    const QRect row = list->visualItemRect(list->item(0));
    auto hover = [&](QPoint position) {
        QHelpEvent event(QEvent::ToolTip, position, list->viewport()->mapToGlobal(position));
        QApplication::sendEvent(list->viewport(), &event);
    };
    auto tooltipText = [] {
        QTextDocument document;
        document.setHtml(QToolTip::text());
        check(document.blockCount() == 1, "folder tooltip contains one text block");
        return document.toPlainText();
    };
    auto checkFolderTip = [&](const QString &folder, bool local = true) {
        const QString native = local ? QDir::toNativeSeparators(folder) : folder;
        const QString displayed = tooltipText();
        const int ellipsis = displayed.indexOf(QChar(0x2026));
        check(ellipsis < 0 ? displayed == native
            : ellipsis > 0 && ellipsis + 1 < displayed.size() &&
                native.startsWith(displayed.left(ellipsis)) && native.endsWith(displayed.mid(ellipsis + 1)),
            "folder tooltip preserves the directory or its prefix and suffix when shortened");
        check(!displayed.contains(QLatin1Char('\n')) && !displayed.contains(QLatin1Char('\r')) &&
            !displayed.contains(QChar::LineSeparator) && !displayed.contains(QChar::ParagraphSeparator),
            "folder tooltip never contains a line break");
        bool found = false;
        for (auto *widget : QApplication::allWidgets()) {
            const auto *label = qobject_cast<QLabel *>(widget);
            if (!label || label->objectName() != QStringLiteral("qtooltip_label") || label->text() != QToolTip::text())
                continue;
            found = true;
            check(widget->height() <= widget->fontMetrics().height() + 16,
                "rendered folder tooltip remains one line high");
            check(widget->width() <= widget->screen()->availableGeometry().width(),
                "rendered folder tooltip fits the available screen width");
        }
        check(found, "the native Qt tooltip label has been created and measured");
    };
    hover(QPoint(row.right() - 16, row.center().y()));
    checkFolderTip(directory);
    check(!tooltipText().contains(QFileInfo(path).fileName()), "size-column hover omits the file name");
    const QString localTip = QToolTip::text();
    const QRect urlRow = list->visualItemRect(list->item(1));
    hover(QPoint(urlRow.right() - 16, urlRow.center().y()));
    check(QToolTip::text() == localTip, "file URLs show the same decoded containing folder");
    hover(QPoint(row.left() + 16, row.center().y()));
    waitFor([] { return !QToolTip::isVisible(); }, "track-name hover hides the file-path tooltip");
    list->item(1)->setSelected(true);
    check(!folder->isEnabled(), "multiple selections disable Open containing folder");
    list->setCurrentRow(2, QItemSelectionModel::ClearAndSelect);
    check(folder->isEnabled(), "a missing local file still allows opening its containing folder");
    const int beforeMissing = revealCalls;
    activate(dock, "openFolder");
    check(revealCalls == beforeMissing && handler.url.toLocalFile() == temporary.path(), "missing files open their parent without a reveal request");
    list->setCurrentRow(3, QItemSelectionModel::ClearAndSelect);
    check(!folder->isEnabled(), "remote URLs cannot launch a local containing folder");
    check(QFile::remove(path), "remove the test file externally");
    control<QLineEdit>(dock, "search")->setText(QStringLiteral("#1"));
    check(list->item(0)->data(Qt::UserRole + 2).toLongLong() == 5, "search reuses cached availability and size");
    control<QLineEdit>(dock, "search")->clear();
    activate(dock, "refresh");
    check(!list->item(0)->data(Qt::AccessibleDescriptionRole).toString().isEmpty() &&
        !list->item(1)->data(Qt::AccessibleDescriptionRole).toString().isEmpty(), "Refresh updates missing status for paths and file URLs");
    writeFile();
    activate(dock, "refresh");
    check(list->item(0)->data(Qt::AccessibleDescriptionRole).toString().isEmpty() && list->item(0)->data(Qt::UserRole + 2).toLongLong() == 5,
        "Refresh clears the missing marker when a file returns");
    for (int cycle = 0; cycle < 100; ++cycle) {
        check(QFile::remove(path), "remove a fixture during repeated availability changes");
        activate(dock, "refresh");
        check(list->item(0)->data(Qt::UserRole + 2).toLongLong() == -3 &&
            list->item(1)->data(Qt::UserRole + 2).toLongLong() == -3, "repeated refresh marks missing paths and file URLs");
        hover(QPoint(row.right() - 16, row.center().y()));
        check(QToolTip::text() == localTip, "missing-file hover retains its cached containing folder");
        writeFile();
        activate(dock, "refresh");
        check(list->item(0)->data(Qt::UserRole + 2).toLongLong() == 5 &&
            list->item(1)->data(Qt::AccessibleDescriptionRole).toString().isEmpty(), "repeated refresh clears missing status after recovery");
        list->setCurrentRow(cycle % 2, QItemSelectionModel::ClearAndSelect);
        activate(dock, "openFolder");
        check(revealedPath == QFileInfo(path).absoluteFilePath(), "repeated path and file-URL selections reveal the intended file");
    }
    const QString longFolder = temporary.path() + QLatin1Char('/') + QStringLiteral("目录 & long ").repeated(160) + QStringLiteral("/尾部 folder");
    const QString multiline = temporary.filePath(QStringLiteral("with\nline\rbreak\u2028folder"));
    for (const auto &folder : {longFolder, multiline, QDir::rootPath()}) {
        setPlaylist(source, {QDir(folder).filePath(QStringLiteral("omit-this-file.flac"))});
        activate(dock, "refresh");
        for (int i = 0; i < 100; ++i)
            hover(QPoint(row.right() - 16, row.center().y()));
        checkFolderTip(folder == multiline ? temporary.filePath(QStringLiteral("with line break folder")) : folder);
        check(!tooltipText().contains(QStringLiteral("omit-this-file")), "folder-only hover omits every fixture's file name");
        if (folder == longFolder)
            check(tooltipText().contains(QChar(0x2026)), "an oversized directory is shortened in the middle");
    }
    setPlaylist(source, {QStringLiteral("https://example.org/media.mp3?token=hidden#position")});
    activate(dock, "refresh");
    hover(QPoint(row.right() - 16, row.center().y()));
    checkFolderTip(QStringLiteral("https://example.org/"), false);
    check(!tooltipText().contains(QStringLiteral("media.mp3")) && !tooltipText().contains(QLatin1Char('?')) &&
        !tooltipText().contains(QLatin1Char('#')), "remote tooltips omit filename, query and fragment");
    const QString absent = temporary.filePath(QStringLiteral("absent/file.wav"));
    setPlaylist(source, {absent});
    activate(dock, "refresh");
    activate(dock, "openFolder");
    check(control<QLabel>(dock, "message")->text().contains(QDir::fromNativeSeparators(QFileInfo(absent).absolutePath())),
        "opening a nonexistent folder reports an error without launching a file manager");
    dock.suspend();
    check(!folder->isEnabled(), "an unbound dock cannot open a containing folder");
    dock.shutdown();
    obs_source_remove(source);
    QDesktopServices::unsetUrlHandler(QStringLiteral("file"));
    std::printf("FILE_FEATURES recovery_cycles=100 reveal_file_url_unicode_fallback_cached_size_path=PASS\n");
    std::printf("FOLDER_TOOLTIP hover_cycles=300 local_url_unicode_escaped_missing_long_single_line_screen_bounds=PASS\n");
}

void viewSuite(const QStringList &files)
{
    OBSSourceAutoRelease first = obs_source_create("vlc_source", "View A", nullptr, nullptr);
    OBSSourceAutoRelease second = obs_source_create("vlc_source", "View B", nullptr, nullptr);
    check(first && second, "create sources for view-state checks");
    QStringList paths;
    for (int row = 0; row < 40; ++row)
        paths.append(files[row % files.size()]);
    setPlaylist(first, paths);
    setPlaylist(second, paths);
    PlaylistDock dock{QString()};
    dock.show();
    bind(dock, first);
    pump(30);
    auto *list = control<QListWidget>(dock, "playlist");
    list->setCurrentRow(27, QItemSelectionModel::ClearAndSelect);
    list->scrollToItem(list->currentItem(), QAbstractItemView::PositionAtCenter);
    pump(30);
    const QString firstId = list->currentItem()->data(Qt::UserRole).toString();
    const int firstScroll = list->verticalScrollBar()->value();
    check(firstScroll > 0, "view fixture has a scrolled long playlist");
    bind(dock, second);
    list->setCurrentRow(10, QItemSelectionModel::ClearAndSelect);
    list->scrollToItem(list->currentItem(), QAbstractItemView::PositionAtCenter);
    pump(30);
    const QString secondId = list->currentItem()->data(Qt::UserRole).toString();
    const int secondScroll = list->verticalScrollBar()->value();
    bind(dock, first);
    pump(30);
    check(list->currentItem()->data(Qt::UserRole).toString() == firstId && list->verticalScrollBar()->value() == firstScroll,
        "returning to a source restores its selected duplicate entry and scroll position");
    bind(dock, second);
    pump(30);
    check(list->currentItem()->data(Qt::UserRole).toString() == secondId && list->verticalScrollBar()->value() == secondScroll,
        "each source retains its own view state independently");
    for (int cycle = 0; cycle < 200; ++cycle) {
        bind(dock, first);
        check(list->currentItem() && list->currentItem()->data(Qt::UserRole).toString() == firstId &&
            list->verticalScrollBar()->value() == firstScroll, "repeated source changes retain the first source view");
        bind(dock, second);
        check(list->currentItem() && list->currentItem()->data(Qt::UserRole).toString() == secondId &&
            list->verticalScrollBar()->value() == secondScroll, "repeated source changes retain the second source view");
    }
    auto *search = control<QLineEdit>(dock, "search");
    search->setText(QStringLiteral("#2"));
    bind(dock, first);
    check(list->currentRow() == 1 && !list->currentItem()->isHidden(), "a changed filter selects its first match instead of a hidden cached item");
    search->clear();
    list->setCurrentRow(22, QItemSelectionModel::ClearAndSelect);
    list->scrollToItem(list->currentItem(), QAbstractItemView::PositionAtCenter);
    pump(30);
    const int suspendedScroll = list->verticalScrollBar()->value();
    dock.suspend();
    bind(dock, first);
    pump(30);
    check(list->currentRow() == 22 && list->verticalScrollBar()->value() == suspendedScroll, "suspend and resume preserve the current source view");
    check(playlist(first) == paths && playlist(second) == paths &&
        obs_source_media_get_state(first) == OBS_MEDIA_STATE_STOPPED && obs_source_media_get_state(second) == OBS_MEDIA_STATE_STOPPED,
        "restoring view state does not start playback or rewrite native playlists");
    bind(dock, second);
    setPlaylist(first, {files[2]});
    bind(dock, first);
    check(list->count() == 1 && list->currentRow() == 0, "a replaced playlist discards a stale selected identity");
    obs_source_remove(first);
    dock.refreshSources();
    check(list->count() == 0 && !control<QAction>(dock, "openFolder")->isEnabled(), "source removal clears the view and folder action safely");
    dock.shutdown();
    obs_source_remove(second);
    std::printf("SOURCE_VIEWS switches=400 selection_scroll_filters_suspend_removed_sources_stale_identity=PASS\n");
}

void formatSuite(const QStringList &files)
{
    const QStringList original{files[0], files[1], files[0], files[2]};
    for (int variant = 0; variant < 4; ++variant) {
        OBSSourceAutoRelease source = obs_source_create("vlc_source", "Queue format", nullptr, nullptr);
        check(source != nullptr, "create a source for queue format checks");
        setPlaylist(source, original);
        OBSDataAutoRelease root = obs_source_get_private_settings(source);
        OBSDataAutoRelease saved = obs_data_create();
        OBSDataArrayAutoRelease observed = obs_data_array_create();
        OBSDataArrayAutoRelease tracks = obs_data_array_create();
        for (int row = 0; row < original.size(); ++row) {
            OBSDataAutoRelease native = obs_data_create();
            obs_data_set_string(native, "value", original[row].toUtf8().constData());
            obs_data_array_push_back(observed, native);
            OBSDataAutoRelease track = obs_data_create();
            obs_data_set_string(track, "value", original[original.size() - 1 - row].toUtf8().constData());
            if (variant != 2 || row != 1)
                obs_data_set_string(track, "id", QStringLiteral("saved-%1").arg(row).toUtf8().constData());
            obs_data_array_push_back(tracks, track);
        }
        if (variant != 1)
            obs_data_set_int(saved, "version", variant == 0 ? 0 : 1);
        obs_data_set_int(saved, "endMode", 2);
        obs_data_set_array(saved, "observed", observed);
        obs_data_set_array(saved, "tracks", tracks);
        obs_data_set_obj(root, "OBS-PlaylistDock", saved);
        PlaylistDock dock{QString()};
        bind(dock, source);
        auto *list = control<QListWidget>(dock, "playlist");
        check(list->count() == original.size(), "queue format checks retain every native entry");
        for (int row = 0; row < list->count(); ++row) {
            check(list->item(row)->data(Qt::UserRole + 1).toString() == original[variant == 3 ? original.size() - 1 - row : row],
                "only complete current-format records restore custom order");
            const QString id = list->item(row)->data(Qt::UserRole).toString();
            check(!id.isEmpty() && (variant == 3 ? id == QStringLiteral("saved-%1").arg(row) : !id.startsWith(QStringLiteral("saved-"))),
                "invalid queue records are discarded as a whole without filling missing IDs");
        }
        check(control<QComboBox>(dock, "endMode")->currentIndex() == (variant < 2 ? 0 : 2),
            "only current-format source preferences are read");
        check(playlist(source) == original, "queue format validation does not modify the native VLC playlist");
        dock.shutdown();
        obs_source_remove(source);
    }
    std::printf("QUEUE_FORMAT current_roundtrip_unsupported_missing_version_missing_id=PASS\n");
}

void shutdownSuite(const QStringList &files)
{
    for (int round = 0; round < 30; ++round) {
        OBSSourceAutoRelease source = obs_source_create("vlc_source", "Pending close", nullptr, nullptr);
        check(source != nullptr, "create a source for pending-close checks");
        setPlaylist(source, {files[0], files[1], files[0], files[2]});
        PlaylistDock dock{QString()};
        bind(dock, source);
        auto *list = control<QListWidget>(dock, "playlist");
        list->setCurrentRow(0, QItemSelectionModel::ClearAndSelect);
        activate(dock, "moveBottom");
        QStringList expected;
        for (int row = 0; row < list->count(); ++row)
            expected.append(list->item(row)->data(Qt::UserRole + 1).toString());
        doubleClick(dock, 0);
        QMetaObject::invokeMethod(list, "itemDoubleClicked", Qt::DirectConnection, Q_ARG(QListWidgetItem *, list->item(1)));
        dock.shutdown();
        PlaylistDock reopened{QString()};
        bind(reopened, source);
        auto *restored = control<QListWidget>(reopened, "playlist");
        check(restored->count() == expected.size(), "closing a pending switch preserves the full queue");
        for (int row = 0; row < restored->count(); ++row)
            check(restored->item(row)->data(Qt::UserRole + 1).toString() == expected[row], "closing a pending switch preserves custom order and duplicates");
        reopened.shutdown();
        obs_source_remove(source);
    }
    std::printf("PENDING_CLOSE cycles=30 full_queue_order_duplicates=PASS\n");
}

void fakeSuite(const QStringList &files)
{
    registerFake();
    featureSuite(files);
    fileSuite();
    viewSuite(files);
    formatSuite(files);
    shutdownSuite(files);
    waitFor([] { std::lock_guard<std::mutex> lock(fakeMediaMutex); return fakeMedia.empty(); }, "feature sources finish deferred destruction");
    OBSSourceAutoRelease first = obs_source_create("vlc_source", "Stress A", nullptr, nullptr);
    OBSSourceAutoRelease second = obs_source_create("vlc_source", "Stress B", nullptr, nullptr);
    check(first && second, "create fake VLC sources");
    FakeMedia *firstMedia;
    {
        std::lock_guard<std::mutex> lock(fakeMediaMutex);
        firstMedia = fakeMedia.front();
    }
    const QStringList original{files[0], files[1], files[0], files[2]};
    setPlaylist(first, original);
    setPlaylist(second, {files[2], files[1]});
    PlaylistDock dock{QString()};
    bind(dock, first);
    auto *list = control<QListWidget>(dock, "playlist");
    check(list->count() == 4, "read original playlist including duplicates");
    check(list->item(0)->data(Qt::UserRole + 1).toString() == files[0] &&
          list->item(1)->data(Qt::UserRole + 1).toString() == files[1] &&
          list->item(2)->data(Qt::UserRole + 1).toString() == files[0] &&
          list->item(3)->data(Qt::UserRole + 1).toString() == files[2], "default order matches the native VLC playlist including duplicates");
    check(playlist(first) == original, "reading must not modify VLC playlist");
    auto *search = control<QLineEdit>(dock, "search");
    search->setText(QStringLiteral("测试音视频-1"));
    check(list->currentItem() && list->currentItem()->data(Qt::UserRole + 1).toString() == files[1], "search selects the first matching track");
    QMetaObject::invokeMethod(search, "returnPressed", Qt::DirectConnection);
    waitFor([&] { return playlist(first) == QStringList{files[1]}; }, "Enter in the search box plays the first match");
    search->setText(QStringLiteral("#4"));
    check(list->currentRow() == 3 && !list->item(3)->isHidden(), "number search matches the complete playlist position");
    QMetaObject::invokeMethod(search, "returnPressed", Qt::DirectConnection);
    waitFor([&] { return playlist(first) == QStringList{files[2]}; }, "number search Enter plays the numbered track");
    for (const QString &query : {QStringLiteral("#0"), QStringLiteral("#999999999999"), QStringLiteral("#bad")}) {
        search->setText(query);
        check(!list->currentItem(), "invalid number searches clear the previous selection");
        QMetaObject::invokeMethod(search, "returnPressed", Qt::DirectConnection);
        pump(50);
        check(playlist(first) == QStringList{files[2]}, "Enter with no match does not play a hidden selection");
    }
    search->clear();
    dock.show();
    dock.activateWindow();
    search->setText(QStringLiteral("测试"));
    search->setCursorPosition(0);
    search->setFocus();
    pump(50);
    keyPress(search, Qt::Key_Delete);
    check(list->count() == 4 && search->text() == QStringLiteral("试"), "Del in search edits text without removing a track");
    search->clear();
    list->setCurrentRow(1);
    list->setFocus();
    pump(50);
    keyPress(list, Qt::Key_Delete);
    check(list->count() == 3 && QFileInfo::exists(files[0]), "Del in the playlist removes only its entry and preserves the file");
    dock.hide();
    setPlaylist(first, original);
    pump(1100);
    activate(dock, "restorePlaylist");
    std::printf("SEARCH_KEYBOARD first_match_number_enter_delete_focus=PASS\n");
    check(list->model()->moveRow(QModelIndex(), 3, QModelIndex(), 0), "native QListWidget drag/drop move");
    check(list->item(0)->data(Qt::UserRole + 1).toString() == files[2], "drag/drop updates list order");
    dock.suspend();
    bind(dock, first);
    check(list->item(0)->data(Qt::UserRole + 1).toString() == files[2], "rebinding preserves a saved manual order");
    activate(dock, "restorePlaylist");
    check(playlist(first).first() == files[2], "drag/drop order is persisted and restored to VLC");
    setPlaylist(first, original);
    pump(1100);
    doubleClickPath(dock, files[1]);
    waitFor([&] { return playlist(first) == QStringList{files[1]}; }, "double click plays selected file");
    check(firstMedia->stoppedBeforeUpdate, "new media settings are applied only after the old media stop completes");
    check(savedCount(first) == 4, "complete queue survives single-file playback");
    auto *play = control<QAbstractButton>(dock, "playPause");
    waitFor([&] { return play->isEnabled(); }, "playback handoff completes");
    play->click();
    waitFor([&] { return obs_source_media_get_state(first) == OBS_MEDIA_STATE_PAUSED; }, "pause");
    play->click();
    waitFor([&] { return obs_source_media_get_state(first) == OBS_MEDIA_STATE_PLAYING; }, "resume");
    pump(300);
    auto *seek = control<QSlider>(dock, "seek");
    const QPointF right(seek->width() * 0.8, seek->height() / 2.0);
    const QPointF left(seek->width() * 0.25, seek->height() / 2.0);
    auto mouse = [seek](QEvent::Type type, const QPointF &position, Qt::MouseButton button, Qt::MouseButtons buttons) {
        QMouseEvent event(type, position, QPointF(seek->mapToGlobal(position.toPoint())), button, buttons, Qt::NoModifier);
        QApplication::sendEvent(seek, &event);
    };
    mouse(QEvent::MouseButtonPress, right, Qt::LeftButton, Qt::LeftButton);
    mouse(QEvent::MouseButtonRelease, right, Qt::LeftButton, Qt::NoButton);
    waitFor([&] { return obs_source_media_get_time(first) >= 40000; }, "clicking the seek groove jumps directly to the requested time");
    mouse(QEvent::MouseButtonPress, right, Qt::LeftButton, Qt::LeftButton);
    mouse(QEvent::MouseMove, left, Qt::NoButton, Qt::LeftButton);
    pump(300);
    check(seek->isSliderDown() && seek->value() > 150 && seek->value() < 350, "dragging follows the pointer without playback polling pulling the handle back");
    check(obs_source_media_get_time(first) >= 40000, "dragging does not flood VLC with intermediate seek requests");
    mouse(QEvent::MouseButtonRelease, left, Qt::LeftButton, Qt::NoButton);
    waitFor([&] { return obs_source_media_get_time(first) >= 10000 && obs_source_media_get_time(first) < 25000; }, "releasing the seek handle applies the final position");
    auto *mode = control<QComboBox>(dock, "endMode");
    mode->setCurrentIndex(1);
    control<QAbstractButton>(dock, "stop")->click();
    const QStringList stopped = playlist(first);
    pump(600);
    check(playlist(first) == stopped && obs_source_media_get_state(first) == OBS_MEDIA_STATE_STOPPED, "manual stop must not auto-advance");

    doubleClick(dock, 1);
    pump(350);
    firstMedia->state = OBS_MEDIA_STATE_ENDED;
    waitFor([&] { return playlist(first) == QStringList{files[0]}; }, "natural end advances once in VLC playlist order");
    mode->setCurrentIndex(2);
    pump(350);
    firstMedia->state = OBS_MEDIA_STATE_ENDED;
    waitFor([&] { return playlist(first) == QStringList{files[0]} && obs_source_media_get_state(first) == OBS_MEDIA_STATE_PLAYING; },
        "repeat restarts current track");
    mode->setCurrentIndex(0);
    pump(350);
    firstMedia->state = OBS_MEDIA_STATE_ENDED;
    waitFor([&] { return playlist(first).isEmpty() && obs_source_media_get_state(first) == OBS_MEDIA_STATE_STOPPED && savedCount(first) == 4; },
        "stop-at-end fully stops playback and preserves the complete queue");
    mode->setCurrentIndex(1);
    doubleClick(dock, list->count() - 1);
    pump(350);
    firstMedia->state = OBS_MEDIA_STATE_ENDED;
    waitFor([&] { return playlist(first).isEmpty() && obs_source_media_get_state(first) == OBS_MEDIA_STATE_STOPPED; },
        "play-next mode fully stops at the final playlist entry");
    mode->setCurrentIndex(0);
    control<QAbstractButton>(dock, "stop")->click();

    QElapsedTimer elapsed;
    elapsed.start();
    quint64 warmMemory = 0;
    for (int iteration = 0; iteration < 6000; ++iteration) {
        if (iteration % 97 == 0)
            bind(dock, iteration % 2 ? second : first);
        list = control<QListWidget>(dock, "playlist");
        if (list->count())
            list->setCurrentRow(iteration % list->count());
        const char *actions[] = {"playPause", "next", "previous", "stop", "moveUp", "moveDown", "moveTop", "stop"};
        activate(dock, actions[iteration % 8]);
        if (iteration % 20 == 0)
            pump();
        if (iteration == 2000)
            warmMemory = memoryBytes();
    }
    pump(250);
    const quint64 afterMemory = memoryBytes();
    check(afterMemory < warmMemory + 32ULL * 1024 * 1024, "private memory growth after warm-up must stay below 32 MiB");
    std::printf("COMMAND_STRESS operations=6000 elapsed_ms=%lld warm_private_bytes=%llu final_private_bytes=%llu\n", elapsed.elapsed(), warmMemory, afterMemory);

    bind(dock, first);
    QStringList large;
    large.reserve(10000);
    for (int i = 0; i < 10000; ++i)
        large.append(files[i % files.size()]);
    setPlaylist(first, large);
    pump(1100);
    check(list->count() == 10000, "synchronize external 10000-item playlist");
    for (int i = 0; i < 40; ++i) {
        list->setCurrentRow(5000);
        activate(dock, "moveUp");
        pump();
    }
    check(savedCount(first) == 10000, "large queue reordering preserves all items");
    QItemSelection batch(list->model()->index(2000, 0), list->model()->index(7999, 0));
    list->selectionModel()->select(batch, QItemSelectionModel::ClearAndSelect);
    activate(dock, "remove");
    check(list->count() == 4000 && savedCount(first) == 4000, "remove 6000 selected entries in one batch");
    activate(dock, "undoRemove");
    check(list->count() == 10000 && savedCount(first) == 10000, "Undo restores the complete large batch");
    activate(dock, "restorePlaylist");
    check(playlist(first).size() == 10000, "restore the complete queue");

    setPlaylist(first, original);
    pump(1100);
    check(list->count() == 4, "external playlist edits replace the managed queue");
    doubleClick(dock, 2);
    OBSDataAutoRelease serialized = obs_save_source(first);
    OBSDataAutoRelease savedPrivate = obs_data_get_obj(serialized, "private_settings");
    OBSDataAutoRelease savedQueue = obs_data_get_obj(savedPrivate, "OBS-PlaylistDock");
    OBSDataArrayAutoRelease savedItems = obs_data_get_array(savedQueue, "tracks");
    check(obs_data_array_count(savedItems) == 4, "complete queue is included in OBS scene serialization");
    obs_data_set_string(serialized, "uuid", QUuid::createUuid().toString(QUuid::WithoutBraces).toUtf8().constData());
    obs_data_set_string(serialized, "name", "Roundtrip VLC");
    {
        OBSSourceAutoRelease reloaded = obs_load_source(serialized);
        check(reloaded != nullptr, "reload a source from serialized OBS scene data");
        bind(dock, reloaded);
        check(list->count() == 4 && playlist(reloaded).size() == 1, "scene reload recovers the full managed queue");
        dock.suspend();
        obs_source_remove(reloaded);
    }
    dock.suspend();
    bind(dock, first);
    check(list->count() == 4 && playlist(first).size() == 1, "rebinding restores full queue without disturbing playback");
    for (int round = 0; round < 100; ++round) {
        const QStringList input = round % 2 ? original : QStringList{files[2], files[0], files[2]};
        OBSDataAutoRelease inputSettings = obs_source_get_settings(first);
        obs_data_set_bool(inputSettings, "loop", round % 2);
        obs_data_set_bool(inputSettings, "shuffle", round % 3 == 0);
        obs_data_set_string(inputSettings, "playback_behavior", "pause_unpause");
        obs_source_update(first, inputSettings);
        setPlaylist(first, input);
        activate(dock, "refresh");
        QStringList expected;
        for (int row = 0; row < list->count(); ++row)
            expected.append(list->item(row)->data(Qt::UserRole + 1).toString());
        check(expected.size() == input.size(), "playlist reads retain duplicates during repeated roundtrips");
        doubleClick(dock, round % list->count());
        dock.suspend();
        bind(dock, first);
        check(savedCount(first) == size_t(input.size()), "persistent full queue survives every roundtrip");
        activate(dock, "restorePlaylist");
        OBSDataAutoRelease outputSettings = obs_source_get_settings(first);
        check(playlist(first) == expected, "playlist writes restore the full ordered queue");
        check(obs_data_get_bool(outputSettings, "loop") == bool(round % 2) &&
              obs_data_get_bool(outputSettings, "shuffle") == (round % 3 == 0) &&
              qstrcmp(obs_data_get_string(outputSettings, "playback_behavior"), "pause_unpause") == 0,
              "roundtrips retain the original loop, shuffle and visibility behavior");
    }
    std::printf("PLAYLIST_ROUNDTRIP reads_persist_rebind_restore=100 duplicates_and_original_options=PASS\n");
    const QString missing = QFileInfo(files[0]).absolutePath() + QStringLiteral("/missing.mp4");
    setPlaylist(first, {missing});
    pump(1100);
    doubleClick(dock, 0);
    check(control<QLabel>(dock, "message")->text().contains(QStringLiteral("不存在")), "missing files produce a useful error");
    check(list->item(0)->data(Qt::UserRole + 2).toLongLong() == -3, "missing local files retain the missing status marker");

    setPlaylist(first, original);
    pump(1100);
    doubleClick(dock, 1);
    activate(dock, "remove");
    pump(100);
    check(playlist(first).isEmpty() && savedCount(first) == 3, "removing the playing track clears VLC playback but preserves remaining queue");
    activate(dock, "restorePlaylist");
    check(playlist(first).size() == 3, "restore remaining queue after deleting current track");
    setPlaylist(first, original);
    pump(1100);
    for (int iteration = 0; iteration < 300; ++iteration) {
        PlaylistDock transient{QString()};
        bind(transient, first);
        transient.suspend();
    }
    std::printf("LIFECYCLE_STRESS dock_create_destroy=300 large_queue=10000 large_reorders=40\n");
    bind(dock, first);
    obs_source_remove(first);
    dock.refreshSources();
    check(list->count() == 0, "removed source detaches safely");
    bind(dock, second);
    check(list->count() == 2, "other source remains usable after source removal");
    while (list->count()) {
        list->setCurrentRow(0);
        activate(dock, "remove");
    }
    check(playlist(second).isEmpty() && savedCount(second) == 0, "emptying the queue clears the VLC playback list");
    dock.shutdown();
}

void realSuite(const QStringList &files, const QString &modulePath, const QString &dataPath)
{
    obs_module_t *module = nullptr;
    check(obs_open_module(&module, modulePath.toUtf8().constData(), dataPath.toUtf8().constData()) == MODULE_SUCCESS, "open the installed VLC module");
    check(obs_init_module(module), "initialize the installed VLC module");
    OBSDataAutoRelease settings = obs_data_create();
    obs_data_set_string(settings, "playback_behavior", "always_play");
    obs_data_set_bool(settings, "loop", false);
    OBSSourceAutoRelease source = obs_source_create("vlc_source", "Real VLC", settings, nullptr);
    check(source != nullptr, "create a real VLC source");
    setPlaylist(source, files);
    PlaylistDock dock{QString()};
    bind(dock, source);
    QStringList ordered;
    auto *list = control<QListWidget>(dock, "playlist");
    for (int row = 0; row < list->count(); ++row)
        ordered.append(list->item(row)->data(Qt::UserRole + 1).toString());
    for (int iteration = 0; iteration < 150; ++iteration) {
        doubleClick(dock, iteration % files.size());
        pump(20);
        if (iteration % 3 == 0)
            control<QAbstractButton>(dock, "stop")->click();
    }
    doubleClickPath(dock, files[0]);
    pump(650);
    check(obs_source_media_get_state(source) == OBS_MEDIA_STATE_PLAYING, "real VLC audio playback starts after rapid switching");
    control<QAbstractButton>(dock, "playPause")->click();
    pump(300);
    check(obs_source_media_get_state(source) == OBS_MEDIA_STATE_PAUSED, "real VLC pause");
    control<QAbstractButton>(dock, "stop")->click();
    pump(300);
    check(obs_source_media_get_state(source) == OBS_MEDIA_STATE_STOPPED, "real VLC stop");
    if (files.size() > 1) {
        doubleClickPath(dock, files[1]);
        pump(900);
        check(obs_source_media_get_state(source) == OBS_MEDIA_STATE_PLAYING, "real VLC video playback");
        check(obs_source_get_width(source) > 0 && obs_source_get_height(source) > 0, "real VLC produces decoded video frames");
        std::printf("VIDEO_FRAMES width=%u height=%u\n", obs_source_get_width(source), obs_source_get_height(source));
        control<QAbstractButton>(dock, "stop")->click();
        pump(100);
    }
    activate(dock, "restorePlaylist");
    check(playlist(source) == ordered, "restore the full real VLC playlist in dock order");
    doubleClick(dock, 0);
    pump(200);
    activate(dock, "remove");
    pump(200);
    check(playlist(source).isEmpty(), "real VLC does not retain the removed playing file");
    check(savedCount(source) == size_t(files.size() - 1), "real VLC queue retains the other files after removal");
    obs_source_media_stop(source);
    dock.shutdown();
    std::printf("REAL_VLC_STRESS rapid_file_switches=150 pause_stop_restore=PASS\n");
}

void mixedSuite(const QStringList &files, const QString &modulePath, const QString &dataPath, bool userMedia, bool continuity, bool nativeControls)
{
    obs_module_t *module = nullptr;
    check(obs_open_module(&module, modulePath.toUtf8().constData(), dataPath.toUtf8().constData()) == MODULE_SUCCESS, "open VLC for mixed-media tests");
    check(obs_init_module(module), "initialize mixed-media VLC");
    QString monitorId;
    const QString monitorDevice = qEnvironmentVariable("PLAYLISTDOCK_MONITOR_DEVICE");
#ifdef _WIN32
    if (!monitorDevice.isEmpty()) {
        check(continuity || userMedia, "monitor loopback uses tone or original-media fixtures");
        struct Device { QString name; QString id; } device{monitorDevice, QString()};
        obs_enum_audio_monitoring_devices([](void *context, const char *name, const char *id) {
            auto &device = *static_cast<Device *>(context);
            if (QString::fromUtf8(name) == device.name)
                device.id = QString::fromUtf8(id);
            return true;
        }, &device);
        check(!device.id.isEmpty(), "requested virtual monitoring endpoint exists");
        monitorId = device.id;
        check(obs_set_audio_monitoring_device(monitorDevice.toUtf8().constData(), monitorId.toUtf8().constData()), "select virtual monitoring endpoint in this test process");
    }
#endif
    int switches = 0;
    const QJsonObject titles = QJsonDocument::fromJson(qEnvironmentVariable("PLAYLISTDOCK_EXPECTED_TITLES").toUtf8()).object();
    const int settled = userMedia ? 9 : continuity ? 10 : 30;
    const int rapid = userMedia || continuity ? 20 : 100;
    for (const char *behavior : {"stop_restart", "pause_unpause", "always_play"}) {
        if ((userMedia || continuity) && qstrcmp(behavior, "always_play") != 0)
            continue;
        for (bool active : {false, true}) {
            if ((userMedia || continuity) && !active)
                continue;
            OBSDataAutoRelease settings = obs_data_create();
            obs_data_set_string(settings, "playback_behavior", behavior);
            obs_data_set_bool(settings, "loop", false);
            OBSSourceAutoRelease source = obs_source_create("vlc_source", "Mixed media VLC", settings, nullptr);
            check(source != nullptr, "create mixed-media VLC source");
            std::atomic<uint64_t> audioFrames{0};
            obs_source_audio_capture_t captureAudio = [](void *context, obs_source_t *, const audio_data *audio, bool) {
                static_cast<std::atomic<uint64_t> *>(context)->fetch_add(audio->frames, std::memory_order_relaxed);
            };
            obs_source_add_audio_capture_callback(source, captureAudio, &audioFrames);
            ProgramAudio programAudio;
            if (!monitorId.isEmpty()) {
                obs_source_set_monitoring_type(source, OBS_MONITORING_TYPE_MONITOR_AND_OUTPUT);
                obs_source_set_volume(source, 0.02f);
                pump(1000);
            }
            auto &programFrames = programAudio.nonSilent;
            audio_output_callback_t captureProgram = &ProgramAudio::capture;
            audio_convert_info outputFormat{};
            outputFormat.samples_per_sec = 48000;
            outputFormat.format = AUDIO_FORMAT_FLOAT_PLANAR;
            outputFormat.speakers = SPEAKERS_STEREO;
            if (active)
                obs_source_inc_active(source);
            if (active) {
                obs_set_output_source(0, source);
                check(audio_output_connect(obs_get_audio(), 0, &outputFormat, captureProgram, &programAudio), "capture actual OBS program audio");
            }
            setPlaylist(source, files);
            pump(150);
            PlaylistDock dock{QString()};
            bind(dock, source);
            auto *list = control<QListWidget>(dock, "playlist");
            int nativeIndex = 0;
            auto request = [&](const QString &path, int delay) {
                for (int row = 0; row < list->count(); ++row) {
                    if (list->item(row)->data(Qt::UserRole + 1).toString() != path)
                        continue;
                    list->setCurrentRow(row);
                    if (nativeControls) {
                        const int target = int(files.indexOf(path));
                        while (nativeIndex < target) {
                            obs_source_media_next(source);
                            ++nativeIndex;
                        }
                        while (nativeIndex > target) {
                            obs_source_media_previous(source);
                            --nativeIndex;
                        }
                        obs_source_media_play_pause(source, false);
                    } else {
                        check(QMetaObject::invokeMethod(list, "itemDoubleClicked", Qt::DirectConnection, Q_ARG(QListWidgetItem *, list->item(row))), "mixed-media double click");
                    }
                    pump(delay);
                    ++switches;
                    return;
                }
                check(false, "mixed-media file exists in the dock");
            };
            auto waitForTrack = [&](const QString &path) {
                const QStringList expectedPlaylist = nativeControls ? files : QStringList{path};
                const QString expected = titles.value(QDir::fromNativeSeparators(path)).toString(QFileInfo(path).completeBaseName());
                const QString fileName = QFileInfo(path).fileName();
                auto matches = [&] {
                    const QString actual = mediaTitle(source);
                    return actual == expected || actual == fileName;
                };
                QElapsedTimer elapsed;
                elapsed.start();
                while (elapsed.elapsed() < 5000) {
                    if (obs_source_media_get_state(source) == OBS_MEDIA_STATE_PLAYING && matches() && playlist(source) == expectedPlaylist)
                        break;
                    pump(20);
                }
                if (obs_source_media_get_state(source) != OBS_MEDIA_STATE_PLAYING || !matches() || playlist(source) != expectedPlaylist) {
                    std::fprintf(stderr, "MIXED_FAILURE behavior=%s active=%d expected=%s actual=%s state=%d\n", behavior, active, expected.toUtf8().constData(), mediaTitle(source).toUtf8().constData(), int(obs_source_media_get_state(source)));
                    check(false, "the requested mixed-media file must actually play");
                }
                check(playlist(source) == expectedPlaylist, "VLC retains the expected runtime playlist");
                check(savedCount(source) == size_t(files.size()), "mixed-media switching preserves the full queue");
                const uint64_t beforeAudio = audioFrames.load(std::memory_order_relaxed);
                QElapsedTimer decodedAudio;
                decodedAudio.start();
                while (audioFrames.load(std::memory_order_relaxed) == beforeAudio && decodedAudio.elapsed() < 2000)
                    pump(20);
                check(audioFrames.load(std::memory_order_relaxed) > beforeAudio, "selected mixed-media file produces decoded audio");
                if (active && QFileInfo(path).suffix().compare(QStringLiteral("mov"), Qt::CaseInsensitive) != 0) {
                    check(!obs_source_async_unbuffered(source) && !obs_source_async_decoupled(source), "audio playback preserves the original source timing options");
                    pump(userMedia ? 1500 : 150);
                    const uint64_t beforeProgram = programFrames.load(std::memory_order_relaxed);
                    QElapsedTimer programWait;
                    programWait.start();
                    while (programFrames.load(std::memory_order_relaxed) == beforeProgram && programWait.elapsed() < 3000)
                        pump(20);
                    check(programFrames.load(std::memory_order_relaxed) > beforeProgram, "video-to-audio switching must produce non-silent OBS program audio");
                    if (userMedia) {
                        for (int interval = 0; interval < 6; ++interval) {
                            const uint64_t before = programFrames.load(std::memory_order_relaxed);
                            pump(500);
                            check(programFrames.load(std::memory_order_relaxed) > before, "FLAC program audio continues after the video-to-audio transition");
                        }
                        std::printf("MIXED_AUDIO file=%s video_width=%u non_silent_program_audio=PASS\n", fileName.toUtf8().constData(), obs_source_get_width(source));
                        std::fflush(stdout);
                    }
                    if (continuity) {
                        pump(1500);
                        programAudio.measured = 0;
                        programAudio.silent = 0;
                        programAudio.maxStep = 0;
                        programAudio.measuring = true;
                        QElapsedTimer window;
                        window.start();
                        pump(3000);
                        programAudio.measuring = false;
                        const double coverage = double(programAudio.measured.load()) / (window.elapsed() * 48.0);
                        const double silence = double(programAudio.silent.load()) / std::max<uint64_t>(1, programAudio.measured.load());
                        std::printf("AUDIO_CONTINUITY file=%s coverage=%.4f silent_ratio=%.4f max_sample_step=%.4f\n", fileName.toUtf8().constData(), coverage, silence, double(programAudio.maxStep.load()));
                        std::fflush(stdout);
                        check(coverage > 0.95 && silence < 0.01 && programAudio.maxStep.load() < 0.15f, "continuous test tone must have no sustained gaps or sample discontinuities after switching");
                    }
                    if (!nativeControls)
                        check(obs_source_get_width(source) == 0, "audio-only playback must clear the previous MOV video frame and clock queue");
                }
                if (QFileInfo(path).suffix().compare(QStringLiteral("mov"), Qt::CaseInsensitive) == 0) {
                    check(!obs_source_async_unbuffered(source) && !obs_source_async_decoupled(source), "video restores the source's audio/video timing");
                    QElapsedTimer frames;
                    frames.start();
                    while (obs_source_get_width(source) == 0 && frames.elapsed() < 2000)
                        pump(20);
                    check(obs_source_get_width(source) > 0 && obs_source_get_height(source) > 0, "selected MOV produces video frames");
                }
            };
            for (int i = 0; i < settled; ++i) {
                const QString &path = files[i % files.size()];
                request(path, 5);
                waitForTrack(path);
                if (userMedia && QFileInfo(path).suffix().compare(QStringLiteral("mov"), Qt::CaseInsensitive) == 0)
                    pump(10000);
                if (continuity && QFileInfo(path).suffix().compare(QStringLiteral("mov"), Qt::CaseInsensitive) == 0)
                    pump(2000);
            }
            for (int i = 0; i < rapid; ++i)
                request(files[i % files.size()], 5);
            waitForTrack(files[(rapid - 1) % files.size()]);
            request(files[0], 1);
            if (nativeControls)
                obs_source_media_stop(source);
            else
                control<QAbstractButton>(dock, "stop")->click();
            waitFor([&] {
                const auto state = obs_source_media_get_state(source);
                return (nativeControls || playlist(source).isEmpty()) && (state == OBS_MEDIA_STATE_STOPPED || state == OBS_MEDIA_STATE_NONE);
            }, "stop cancels a pending mixed-media switch");
            pump(200);
            check(obs_source_media_get_state(source) != OBS_MEDIA_STATE_PLAYING, "a cancelled mixed-media request cannot restart playback");
            if (nativeControls) {
                obs_source_media_restart(source);
                nativeIndex = 0;
                pump(150);
            }
            request(files[1], 5);
            waitForTrack(files[1]);
            if (nativeControls)
                obs_source_media_play_pause(source, true);
            else
                control<QAbstractButton>(dock, "playPause")->click();
            waitFor([&] { return obs_source_media_get_state(source) == OBS_MEDIA_STATE_PAUSED; }, "mixed-media pause");
            request(files[0], 5);
            waitForTrack(files[0]);
            request(files[0], 5);
            waitForTrack(files[0]);
            activate(dock, "restorePlaylist");
            OBSDataAutoRelease restored = obs_source_get_settings(source);
            check(qstrcmp(obs_data_get_string(restored, "playback_behavior"), behavior) == 0, "restore the original VLC visibility behavior");
            check(playlist(source).size() == files.size(), "restore the complete mixed-media queue");
            check(!obs_source_async_unbuffered(source) && !obs_source_async_decoupled(source), "restore the source's original timing flags");
            obs_source_media_stop(source);
            pump(100);
            dock.shutdown();
            obs_source_remove_audio_capture_callback(source, captureAudio, &audioFrames);
            if (!monitorId.isEmpty()) {
                obs_source_set_monitoring_type(source, OBS_MONITORING_TYPE_NONE);
            }
            if (active) {
                audio_output_disconnect(obs_get_audio(), 0, captureProgram, &programAudio);
                obs_set_output_source(0, nullptr);
            }
            if (active)
                obs_source_dec_active(source);
            obs_source_remove(source);
            std::printf("MIXED_CASE behavior=%s active=%d settled=%d rapid=%d pause_stop_repeat_restore=PASS\n", behavior, active, settled, rapid);
            std::fflush(stdout);
        }
    }
    std::printf("MIXED_MEDIA_STRESS requests=%d user_media=%d metadata_decoded_audio_program_audio_and_mov_frames=PASS\n", switches, userMedia);
}

void endSuite(const QStringList &files, const QString &modulePath, const QString &dataPath)
{
    obs_module_t *module = nullptr;
    check(obs_open_module(&module, modulePath.toUtf8().constData(), dataPath.toUtf8().constData()) == MODULE_SUCCESS && obs_init_module(module), "load real VLC for video completion tests");
    int cases = 0;
    for (const auto &path : files) {
        if (QFileInfo(path).suffix().compare(QStringLiteral("mov"), Qt::CaseInsensitive) != 0)
            continue;
        for (const bool active : {false, true}) {
            OBSDataAutoRelease settings = obs_data_create();
            obs_data_set_string(settings, "playback_behavior", "always_play");
            OBSSourceAutoRelease source = obs_source_create("vlc_source", "Video end", settings, nullptr);
            check(source != nullptr, "create video completion source");
            setPlaylist(source, {files[1], path});
            if (active) {
                obs_source_inc_active(source);
                obs_set_output_source(0, source);
            }
            PlaylistDock dock{QString()};
            bind(dock, source);
            auto *mode = control<QComboBox>(dock, "endMode");
            for (int endMode : {0, 1}) {
                mode->setCurrentIndex(endMode);
                doubleClick(dock, 1);
                waitFor([&] { return obs_source_media_get_state(source) == OBS_MEDIA_STATE_PLAYING && obs_source_get_width(source) > 0 && obs_source_media_get_duration(source) > 0; }, "MOV produces frames before natural completion");
                pump(350);
                obs_source_media_set_time(source, std::max<int64_t>(0, obs_source_media_get_duration(source) - 750));
                waitFor([&] {
                    const auto state = obs_source_media_get_state(source);
                    return (state == OBS_MEDIA_STATE_STOPPED || state == OBS_MEDIA_STATE_NONE) && playlist(source).isEmpty() && obs_source_get_width(source) == 0;
                }, "natural video completion stops VLC and clears its final frame");
                pump(350);
                check(obs_source_get_width(source) == 0 && playlist(source).isEmpty() && savedCount(source) == 2,
                    "the final video frame stays cleared while the full queue is retained");
                ++cases;
            }
            dock.shutdown();
            if (active) {
                obs_set_output_source(0, nullptr);
                obs_source_dec_active(source);
            }
            obs_source_remove(source);
        }
    }
    check(cases >= 8, "completion tests cover H.264 and ProRes, active and inactive sources, stop and final-next modes");
    std::printf("VIDEO_END cases=%d natural_stop_final_next_no_retained_frame=PASS\n", cases);
}

void sourceSwitchSuite(const QStringList &files, const QString &modulePath, const QString &dataPath)
{
    obs_module_t *module = nullptr;
    check(obs_open_module(&module, modulePath.toUtf8().constData(), dataPath.toUtf8().constData()) == MODULE_SUCCESS, "open real VLC for cross-source tests");
    check(obs_init_module(module), "initialize real VLC for cross-source tests");
    std::array<OBSSourceAutoRelease, 3> sources;
    std::array<QStringList, 3> queues;
    std::array<std::atomic<uint64_t>, 3> decoded{};
    const char *behaviors[] = {"stop_restart", "pause_unpause", "always_play"};
    obs_source_audio_capture_t capture = [](void *context, obs_source_t *, const audio_data *audio, bool) {
        static_cast<std::atomic<uint64_t> *>(context)->fetch_add(audio->frames, std::memory_order_relaxed);
    };
    std::atomic<uint64_t> programFrames{0};
    audio_output_callback_t program = [](void *context, size_t, audio_data *audio) {
        const auto *samples = reinterpret_cast<const float *>(audio->data[0]);
        if (!samples)
            return;
        for (uint32_t i = 0; i < audio->frames; ++i) {
            if (std::isfinite(samples[i]) && std::abs(samples[i]) > 0.00001f) {
                static_cast<std::atomic<uint64_t> *>(context)->fetch_add(audio->frames, std::memory_order_relaxed);
                break;
            }
        }
    };
    audio_convert_info format{};
    format.samples_per_sec = 48000;
    format.speakers = SPEAKERS_STEREO;
    format.format = AUDIO_FORMAT_FLOAT_PLANAR;
    check(audio_output_connect(obs_get_audio(), 0, &format, program, &programFrames), "capture cross-source program audio");
    for (int i = 0; i < 3; ++i) {
        OBSDataAutoRelease settings = obs_data_create();
        obs_data_set_string(settings, "playback_behavior", behaviors[i]);
        obs_data_set_bool(settings, "loop", false);
        sources[i] = obs_source_create("vlc_source", qPrintable(QStringLiteral("VLC source %1").arg(i)), settings, nullptr);
        check(sources[i] != nullptr, "create three independent real VLC sources");
        queues[i] = files;
        if (i == 1)
            std::reverse(queues[i].begin(), queues[i].end());
        if (i == 2)
            queues[i].append(files.front());
        setPlaylist(sources[i], queues[i]);
        obs_source_inc_active(sources[i]);
        obs_source_add_audio_capture_callback(sources[i], capture, &decoded[i]);
    }
    pump(200);
    PlaylistDock dock{QString()};
    for (int i = 0; i < 3; ++i)
        bind(dock, sources[i]);
    auto request = [&](int index, int track, int delay) {
        bind(dock, sources[index]);
        auto *list = control<QListWidget>(dock, "playlist");
        check(list->count() == queues[index].size(), "switching sources preserves each complete playlist");
        obs_set_output_source(0, sources[index]);
        const QString path = files[track % files.size()];
        for (int row = 0; row < list->count(); ++row) {
            if (list->item(row)->data(Qt::UserRole + 1).toString() == path) {
                list->setCurrentRow(row);
                QMetaObject::invokeMethod(list, "itemDoubleClicked", Qt::DirectConnection, Q_ARG(QListWidgetItem *, list->item(row)));
                pump(delay);
                return path;
            }
        }
        check(false, "requested cross-source track exists");
        return QString();
    };
    auto confirm = [&](int index, const QString &path) {
        auto *source = sources[index].Get();
        QElapsedTimer wait;
        wait.start();
        const QString title = QFileInfo(path).completeBaseName();
        while (wait.elapsed() < 5000 && (obs_source_media_get_state(source) != OBS_MEDIA_STATE_PLAYING || mediaTitle(source) != title || playlist(source) != QStringList{path}))
            pump(20);
        check(obs_source_media_get_state(source) == OBS_MEDIA_STATE_PLAYING && mediaTitle(source) == title, "the selected source plays the requested real media");
        check(playlist(source) == QStringList{path}, "a source never receives a different source's queued request");
        const uint64_t before = decoded[index].load();
        wait.restart();
        while (decoded[index].load() == before && wait.elapsed() < 2000)
            pump(20);
        check(decoded[index].load() > before, "selected source continues decoding audio after rebinding");
        if (QFileInfo(path).suffix().compare(QStringLiteral("mov"), Qt::CaseInsensitive) == 0) {
            wait.restart();
            while (obs_source_get_width(source) == 0 && wait.elapsed() < 2000)
                pump(20);
            check(obs_source_get_width(source) > 0, "cross-source MOV produces video frames");
        } else {
            pump(200);
            const uint64_t output = programFrames.load();
            wait.restart();
            while (programFrames.load() == output && wait.elapsed() < 3000)
                pump(20);
            check(programFrames.load() > output && obs_source_get_width(source) == 0, "cross-source audio reaches the actual program mix with no stale video");
        }
        for (int i = 0; i < 3; ++i)
            check(savedCount(sources[i]) == size_t(queues[i].size()), "all three source queues remain intact");
    };
    for (int i = 0; i < 30; ++i)
        confirm(i % 3, request(i % 3, i / 3, 5));
    const quint64 warm = memoryBytes();
    for (int i = 0; i < 600; ++i)
        request(i % 3, i / 3, 2);
    for (int i = 0; i < 60; ++i)
        confirm(i % 3, request(i % 3, i / 3, 5));
    for (int i = 0; i < 30; ++i) {
        request(i % 3, 0, 0);
        control<QAbstractButton>(dock, "stop")->click();
        auto *source = sources[i % 3].Get();
        QElapsedTimer stopped;
        stopped.start();
        auto settled = [&] {
            const auto state = obs_source_media_get_state(source);
            return playlist(source).isEmpty() && (state == OBS_MEDIA_STATE_STOPPED || state == OBS_MEDIA_STATE_NONE);
        };
        while (!settled() && stopped.elapsed() < 5000)
            pump(20);
        if (!settled())
            std::printf("STOP_STATE iteration=%d state=%d playlist_count=%lld title=%s elapsed_ms=%lld\n", i, int(obs_source_media_get_state(source)), qlonglong(playlist(source).size()), qPrintable(mediaTitle(source)), stopped.elapsed());
        check(settled(), "stop cancels a pending request during rapid source changes");
        pump(200);
        check(settled(), "a cancelled request cannot restart playback");
        const int next = (i + 1) % 3;
        confirm(next, request(next, 1, 5));
    }
    pump(500);
    const quint64 final = memoryBytes();
    check(final < warm + 64ULL * 1024 * 1024, "cross-source private memory growth remains bounded after decoder warm-up");
    for (int i = 0; i < 3; ++i) {
        bind(dock, sources[i]);
        activate(dock, "restorePlaylist");
        OBSDataAutoRelease settings = obs_source_get_settings(sources[i]);
        check(playlist(sources[i]).size() == queues[i].size() && qstrcmp(obs_data_get_string(settings, "playback_behavior"), behaviors[i]) == 0, "each source restores its own playlist and visibility behavior");
        obs_source_media_stop(sources[i]);
    }
    pump(150);
    dock.shutdown();
    obs_set_output_source(0, nullptr);
    audio_output_disconnect(obs_get_audio(), 0, program, &programFrames);
    for (int i = 0; i < 3; ++i) {
        obs_source_remove_audio_capture_callback(sources[i], capture, &decoded[i]);
        obs_source_dec_active(sources[i]);
        obs_source_remove(sources[i]);
    }
    std::printf("SOURCE_SWITCH_STRESS sources=3 rapid=600 confirmed=90 cancel_and_rebind=30 warm_private_bytes=%llu final_private_bytes=%llu audio_video_queues_restore=PASS\n", warm, final);
}
}

int main(int argc, char **argv)
{
    setvbuf(stdout, nullptr, _IONBF, 0);
    QApplication app(argc, argv);
    QApplication::setEffectEnabled(Qt::UI_FadeTooltip, false);
    QApplication::setEffectEnabled(Qt::UI_AnimateTooltip, false);
#ifdef _WIN32
    const HRESULT com = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    check(SUCCEEDED(com) || com == RPC_E_CHANGED_MODE, "initialize COM for native audio monitoring tests");
#endif
    testLocale = text_lookup_create(PLAYLISTDOCK_TEST_LOCALE);
    check(testLocale != nullptr, "load the same locale resources used by the plugin");
    QTemporaryDir temporary;
    check(temporary.isValid(), "create temporary test directory");
    const bool real = argc >= 4 && QString::fromLocal8Bit(argv[1]) == QStringLiteral("--vlc");
    const bool userMedia = argc >= 4 && QString::fromLocal8Bit(argv[1]) == QStringLiteral("--mixed-real");
    const bool nativeControls = argc >= 4 && QString::fromLocal8Bit(argv[1]) == QStringLiteral("--native-continuity");
    const bool continuity = nativeControls || (argc >= 4 && QString::fromLocal8Bit(argv[1]) == QStringLiteral("--continuity"));
    const bool mixed = userMedia || continuity || (argc >= 4 && QString::fromLocal8Bit(argv[1]) == QStringLiteral("--mixed"));
    const bool sources = argc >= 4 && QString::fromLocal8Bit(argv[1]) == QStringLiteral("--sources");
    const bool ended = argc >= 4 && QString::fromLocal8Bit(argv[1]) == QStringLiteral("--end");
    QStringList files;
    if (real || mixed || sources || ended) {
        check(argc >= 5, "real VLC mode needs a media file");
        for (int i = 4; i < argc; ++i)
            files.append(QString::fromLocal8Bit(argv[i]));
    } else {
        for (int i = 0; i < 3; ++i) {
            const QString path = temporary.filePath(QStringLiteral("测试音视频-%1.mp4").arg(i));
            QFile file(path);
            check(file.open(QIODevice::WriteOnly), "create a test file");
            file.write("test");
            files.append(path);
        }
    }
    check(obs_startup("zh-CN", temporary.path().toUtf8().constData(), nullptr), "start libobs");
    obs_audio_info audio{};
    audio.samples_per_sec = 48000;
    audio.speakers = SPEAKERS_STEREO;
    check(obs_reset_audio(&audio), "initialize the OBS audio clock");
#ifdef _WIN32
    const QByteArray obsData = qgetenv("PLAYLISTDOCK_OBS_DATA");
    check(!obsData.isEmpty(), "PLAYLISTDOCK_OBS_DATA must point to OBS data/libobs");
    const QByteArray dataPrefix = QDir::fromNativeSeparators(QString::fromLocal8Bit(obsData)).toUtf8() + '/';
    obs_add_data_path(dataPrefix.constData());
#endif
    obs_video_info video{};
#ifdef _WIN32
    video.graphics_module = "libobs-d3d11";
#else
    const QByteArray graphics = qgetenv("PLAYLISTDOCK_GRAPHICS_MODULE");
    check(!graphics.isEmpty(), "PLAYLISTDOCK_GRAPHICS_MODULE must point to the OBS graphics library");
    video.graphics_module = graphics.constData();
#endif
    video.fps_num = 30;
    video.fps_den = 1;
    video.base_width = 128;
    video.base_height = 128;
    video.output_width = 128;
    video.output_height = 128;
    video.output_format = VIDEO_FORMAT_NV12;
    video.colorspace = VIDEO_CS_709;
    video.range = VIDEO_RANGE_PARTIAL;
    video.scale_type = OBS_SCALE_BILINEAR;
    check(obs_reset_video(&video) == OBS_VIDEO_SUCCESS, "start the real OBS video/media thread");
    const quint64 before = memoryBytes();
    if (ended)
        endSuite(files, QString::fromLocal8Bit(argv[2]), QString::fromLocal8Bit(argv[3]));
    else if (sources)
        sourceSwitchSuite(files, QString::fromLocal8Bit(argv[2]), QString::fromLocal8Bit(argv[3]));
    else if (mixed)
        mixedSuite(files, QString::fromLocal8Bit(argv[2]), QString::fromLocal8Bit(argv[3]), userMedia, continuity, nativeControls);
    else if (real)
        realSuite(files, QString::fromLocal8Bit(argv[2]), QString::fromLocal8Bit(argv[3]));
    else
        fakeSuite(files);
    pump();
    obs_shutdown();
    text_lookup_destroy(testLocale);
    testLocale = nullptr;
    check(fakeMedia.empty(), "all fake source instances must be destroyed");
    std::printf("PASS initial_private_bytes=%llu final_private_bytes=%llu\n", before, memoryBytes());
#ifdef _WIN32
    if (SUCCEEDED(com))
        CoUninitialize();
#endif
    return 0;
}

#include "stress.moc"
