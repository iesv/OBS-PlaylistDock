// SPDX-License-Identifier: GPL-2.0-or-later
#include <obs-module.h>
#include <obs-frontend-api.h>
#include <obs.hpp>
#include <QAction>
#include <QApplication>
#include <QComboBox>
#include <QDir>
#include <QDockWidget>
#include <QElapsedTimer>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMenu>
#include <QMessageBox>
#include <QPixmap>
#include <QResizeEvent>
#include <QThread>
#include <QTimer>
#include <QVBoxLayout>
OBS_DECLARE_MODULE()
MODULE_EXPORT const char *obs_module_name() { return "PlaylistDock smoke-test helper"; }
namespace {
int failures = 0;
void check(bool value, const char *name)
{
    blog(value ? LOG_INFO : LOG_ERROR, "OBS_UI_CHECK %s=%s", name, value ? "PASS" : "FAIL");
    failures += !value;
}
void pump(int ms)
{
    QElapsedTimer timer;
    timer.start();
    do { QApplication::processEvents(); QThread::msleep(5); } while (timer.elapsed() < ms);
}
bool isScene(obs_source_t *target, bool preview)
{
    OBSSourceAutoRelease current = preview ? obs_frontend_get_current_preview_scene() : obs_frontend_get_current_scene();
    return current == target;
}
void waitScene(obs_source_t *target, bool preview)
{
    QElapsedTimer timer;
    timer.start();
    while (!isScene(target, preview) && timer.elapsed() < 5000) pump(20);
    check(isScene(target, preview), preview ? "preview_target" : "program_target");
}
void snapshot(QWidget *widget, const QString &name, const QSize &size = QSize(720, 230))
{
    const QString directory = qEnvironmentVariable("PLAYLISTDOCK_SMOKE_OUTPUT");
    if (directory.isEmpty()) return;
    QDir().mkpath(directory);
    auto *owner = qobject_cast<QDockWidget *>(widget->parentWidget());
    if (!owner) return;
    widget->setParent(nullptr);
    const QSize previousSize = widget->size();
    widget->resize(size);
    QResizeEvent resize(size, previousSize);
    QApplication::sendEvent(widget, &resize);
    widget->ensurePolished();
    widget->layout()->setGeometry(widget->rect());
    QPixmap image(widget->size());
    image.fill(widget->palette().color(QPalette::Window));
    widget->render(&image);
    auto *search = widget->findChild<QLineEdit *>(QStringLiteral("search"));
    auto *selector = widget->findChild<QComboBox *>(QStringLiteral("sourceSelector"));
    check(search && selector && (size.width() < 600
        ? search->y() >= selector->mapTo(widget, QPoint(0, selector->height())).y()
        : qAbs(search->geometry().center().y() - selector->mapTo(widget, selector->rect().center()).y()) <= 2), "responsive_header");
    check(image.save(QDir(directory).filePath(name)), "snapshot");
    owner->setWidget(widget);
}
void verify(QWidget *window)
{
    auto *widget = window->findChild<QWidget *>(QStringLiteral("OBS-PlaylistDock-content"));
    check(widget != nullptr, "dock_created");
    if (!widget) return;
    auto *search = widget->findChild<QLineEdit *>(QStringLiteral("search"));
    auto *list = widget->findChild<QListWidget *>(QStringLiteral("playlist"));
    auto *selector = widget->findChild<QComboBox *>(QStringLiteral("sourceSelector"));
    auto *mode = widget->findChild<QComboBox *>(QStringLiteral("endMode"));
    auto *status = widget->findChild<QLabel *>(QStringLiteral("message"));
    auto *sceneMenu = widget->findChild<QMenu *>(QStringLiteral("sceneSwitchMenu"));
    auto *about = widget->findChild<QAction *>(QStringLiteral("aboutPlugin"));
    check(search && list && selector && mode && status && about, "controls_created");
    if (!search || !list || !selector || !mode || !status || !about) return;
    check(!sceneMenu, "automatic_scene_switch_has_no_menu");
    QWidget *container = widget->parentWidget();
    while (container && !qobject_cast<QDockWidget *>(container)) container = container->parentWidget();
    const QString locale = QString::fromUtf8(obs_get_locale());
    const QString expected = locale == QStringLiteral("zh-CN") ? QStringLiteral("播放列表")
        : locale == QStringLiteral("zh-TW") ? QStringLiteral("播放清單") : QStringLiteral("Playlist");
    check(container && container->windowTitle() == expected, "localized_dock_title");
    check(mode->count() == 3, "three_end_modes");
    OBSDataAutoRelease settings = obs_data_create();
    OBSDataArrayAutoRelease queue = obs_data_array_create();
    const QByteArray media = qEnvironmentVariable("PLAYLISTDOCK_SMOKE_MEDIA").toUtf8();
    for (int i = 0; i < 12; ++i) {
        OBSDataAutoRelease item = obs_data_create();
        obs_data_set_string(item, "value", media.constData());
        obs_data_array_push_back(queue, item);
    }
    obs_data_set_array(settings, "playlist", queue);
    obs_data_set_bool(settings, "loop", false);
    OBSSourceAutoRelease first = obs_source_create("vlc_source", "UI VLC A", settings, nullptr);
    OBSSourceAutoRelease second = obs_source_create("vlc_source", "UI VLC B", settings, nullptr);
    check(first && second, "real_vlc_sources");
    if (!first || !second) return;
    OBSSceneAutoRelease a = obs_scene_create("UI Scene A");
    OBSSceneAutoRelease b = obs_scene_create("UI Scene B");
    OBSSceneAutoRelease c = obs_scene_create("UI Scene C");
    auto *firstItem = obs_scene_add(a, first);
    auto *secondItem = obs_scene_add(b, second);
    auto *sharedItem = obs_scene_add(c, first);
    obs_sceneitem_set_visible(secondItem, false);
    OBSSourceAutoRelease original = obs_frontend_get_current_scene();
    obs_frontend_set_current_scene(obs_scene_get_source(a));
    pump(1100);
    auto select = [&](obs_source_t *source) {
        const int index = selector->findData(QString::fromUtf8(obs_source_get_uuid(source)));
        check(index > 0, "source_selectable");
        const QString caption = selector->itemText(index);
        check(source == second ? caption.contains(QStringLiteral("UI Scene B")) && caption.contains(QStringLiteral("UI VLC B"))
            : caption.contains(QStringLiteral("UI Scene A")) && caption.contains(QStringLiteral("UI Scene C")) && caption.contains(QStringLiteral("UI VLC A")), "source_caption_contains_scene_names");
        selector->setCurrentIndex(index);
        pump(50);
    };
    select(second);
    check(list->count() == 12, "complete_numbered_queue");
    search->setText(QStringLiteral("#12"));
    check(list->currentRow() == 11 && !list->item(11)->isHidden(), "number_search_selection");
    QMetaObject::invokeMethod(search, "returnPressed", Qt::DirectConnection);
    waitScene(obs_scene_get_source(b), false);
    check(!obs_sceneitem_visible(secondItem), "hidden_source_program_switch_preserves_visibility");
    pump(500);
    check(status->isHidden(), "no_success_notification");
    snapshot(widget, QStringLiteral("number-search.png"));
    search->clear();
    list->scrollToBottom();
    snapshot(widget, QStringLiteral("number-alignment.png"));
    snapshot(widget, QStringLiteral("narrow-layout.png"), QSize(480, 230));
    obs_frontend_set_current_scene(obs_scene_get_source(a));
    obs_frontend_set_preview_program_mode(true);
    obs_frontend_set_current_preview_scene(obs_scene_get_source(a));
    pump(150);
    search->setText(QStringLiteral("#2"));
    check(QMetaObject::invokeMethod(list, "itemDoubleClicked", Qt::DirectConnection,
        Q_ARG(QListWidgetItem *, list->currentItem())), "double_click_activation");
    waitScene(obs_scene_get_source(b), true);
    check(isScene(obs_scene_get_source(a), false), "studio_program_unchanged");
    check(!obs_sceneitem_visible(secondItem), "hidden_source_preview_switch_preserves_visibility");
    auto *group = obs_scene_add_group(b, "UI Hidden VLC Group");
    obs_sceneitem_group_add_item(group, secondItem);
    obs_sceneitem_set_visible(secondItem, true);
    obs_sceneitem_set_visible(group, false);
    for (int i = 0; i < 20; ++i) {
        obs_frontend_set_current_preview_scene(obs_scene_get_source(a));
        pump(30);
        search->setText(QStringLiteral("#%1").arg(i % 10 + 2));
        if (i % 2)
            QMetaObject::invokeMethod(search, "returnPressed", Qt::DirectConnection);
        else
            check(QMetaObject::invokeMethod(list, "itemDoubleClicked", Qt::DirectConnection,
                Q_ARG(QListWidgetItem *, list->currentItem())), "group_double_click_activation");
        waitScene(obs_scene_get_source(b), true);
        check(isScene(obs_scene_get_source(a), false), "hidden_group_studio_program_unchanged");
        check(!obs_sceneitem_visible(group) && obs_sceneitem_visible(secondItem),
            "hidden_group_switch_preserves_visibility");
    }
    blog(LOG_INFO, "OBS_UI_CHECK hidden_group_switch_cycles=20");
    obs_frontend_set_preview_program_mode(false);
    obs_frontend_set_current_scene(obs_scene_get_source(b));
    obs_sceneitem_set_visible(firstItem, false);
    select(first);
    search->setText(QStringLiteral("#1"));
    QMetaObject::invokeMethod(search, "returnPressed", Qt::DirectConnection);
    pump(400);
    check(isScene(obs_scene_get_source(b), false) && status->isHidden(), "ambiguous_scene_not_guessed");
    obs_sceneitem_set_visible(sharedItem, false);
    obs_frontend_set_current_scene(obs_scene_get_source(c));
    QMetaObject::invokeMethod(search, "returnPressed", Qt::DirectConnection);
    pump(400);
    waitScene(obs_scene_get_source(c), false);
    check(status->isHidden(), "matching_scene_playback_has_no_notification");
    QTimer::singleShot(50, window, [] {
        auto *dialog = qobject_cast<QMessageBox *>(QApplication::activeModalWidget());
        check(dialog && dialog->text().contains(QStringLiteral("https://haliluya.net")), "about_author_url");
        bool link = false;
        if (dialog) {
            for (auto *label : dialog->findChildren<QLabel *>())
                link |= label->openExternalLinks() && label->text().contains(QStringLiteral("haliluya.net"));
            dialog->accept();
        }
        check(link, "author_hyperlink_enabled");
    });
    about->trigger();
    auto *folder = widget->findChild<QAction *>(QStringLiteral("openFolder"));
    check(folder && folder->isEnabled(), "reveal_file_action");
    if (folder && folder->isEnabled()) folder->trigger();
    selector->setCurrentIndex(0);
    obs_source_media_stop(first);
    obs_source_media_stop(second);
    if (original) obs_frontend_set_current_scene(original);
    obs_source_remove(obs_scene_get_source(a));
    obs_source_remove(obs_scene_get_source(b));
    obs_source_remove(obs_scene_get_source(c));
    obs_source_remove(first);
    obs_source_remove(second);
    pump(100);
    blog(failures ? LOG_ERROR : LOG_INFO, "OBS_UI_CHECK all=%s locale=%s", failures ? "FAIL" : "PASS", obs_get_locale());
}
void event(obs_frontend_event event, void *)
{
    if (event != OBS_FRONTEND_EVENT_FINISHED_LOADING) return;
    auto *window = static_cast<QWidget *>(obs_frontend_get_main_window());
    QTimer::singleShot(1000, window, [window] {
        if (!qEnvironmentVariable("PLAYLISTDOCK_SMOKE_MEDIA").isEmpty()) verify(window);
        QTimer::singleShot(300, window, [window] { window->close(); });
    });
}
}
bool obs_module_load()
{
    obs_frontend_add_event_callback(event, nullptr);
    return true;
}
void obs_module_unload() { obs_frontend_remove_event_callback(event, nullptr); }
