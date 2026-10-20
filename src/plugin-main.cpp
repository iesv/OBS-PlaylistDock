// SPDX-License-Identifier: GPL-2.0-or-later
#include "PlaylistDock.hpp"
#include <obs-module.h>
#include <obs-frontend-api.h>
#include <QPointer>
#include <QSet>

#if LIBOBS_API_VER != MAKE_SEMANTIC_VERSION(32, 0, 4)
#error Build OBS-PlaylistDock against the OBS 32.0.4 headers.
#endif

OBS_DECLARE_MODULE()
OBS_MODULE_USE_DEFAULT_LOCALE("obs-playlistdock", "en-US")
MODULE_EXPORT const char *obs_module_name() { return obs_module_text("DockName"); }
MODULE_EXPORT const char *obs_module_description() { return obs_module_text("PluginDescription"); }
MODULE_EXPORT const char *obs_module_author() { return "haliluya.net"; }

namespace {
constexpr const char *DockId = "OBS-PlaylistDock";
QPointer<PlaylistDock> dock;
bool exiting = false;

bool containsSource(obs_source_t *container, obs_source_t *source, QSet<obs_source_t *> &visited)
{
    if (container == source)
        return true;
    obs_scene_t *scene = obs_group_or_scene_from_source(container);
    if (!scene || visited.contains(container))
        return false;
    visited.insert(container);
    struct Search {
        obs_source_t *source;
        QSet<obs_source_t *> *visited;
        bool found = false;
    } search{source, &visited};
    obs_scene_enum_items(scene, [](obs_scene_t *, obs_sceneitem_t *item, void *context) {
        auto &search = *static_cast<Search *>(context);
        if (containsSource(obs_sceneitem_get_source(item), search.source, *search.visited))
            search.found = true;
        return !search.found;
    }, &search);
    return search.found;
}

QList<OBSSource> matchingScenes(obs_source_t *source)
{
    obs_frontend_source_list scenes{};
    obs_frontend_get_scenes(&scenes);
    QList<OBSSource> matches;
    for (size_t i = 0; i < scenes.sources.num; ++i) {
        QSet<obs_source_t *> visited;
        if (!obs_source_removed(scenes.sources.array[i]) && containsSource(scenes.sources.array[i], source, visited))
            matches.append(OBSSource(scenes.sources.array[i]));
    }
    obs_frontend_source_list_free(&scenes);
    return matches;
}

void switchScene(const QString &sourceUuid)
{
    OBSSourceAutoRelease source = obs_get_source_by_uuid(sourceUuid.toUtf8().constData());
    if (!source)
        return;
    const bool studio = obs_frontend_preview_program_mode_active();
    OBSSourceAutoRelease current = studio ? obs_frontend_get_current_preview_scene() : obs_frontend_get_current_scene();
    QSet<obs_source_t *> visited;
    if (current && containsSource(current, source, visited))
        return;
    const auto matches = matchingScenes(source);
    if (matches.size() != 1)
        return;
    if (studio)
        obs_frontend_set_current_preview_scene(matches.front());
    else
        obs_frontend_set_current_scene(matches.front());
}

void frontendEvent(obs_frontend_event event, void *)
{
    if (!dock)
        return;
    switch (event) {
    case OBS_FRONTEND_EVENT_FINISHED_LOADING:
    case OBS_FRONTEND_EVENT_SCENE_COLLECTION_CHANGED:
        dock->resume();
        break;
    case OBS_FRONTEND_EVENT_SCENE_COLLECTION_CHANGING:
    case OBS_FRONTEND_EVENT_SCENE_COLLECTION_CLEANUP:
        dock->suspend();
        break;
    case OBS_FRONTEND_EVENT_EXIT:
        exiting = true;
        dock->shutdown();
        break;
    default:
        break;
    }
}
}

bool obs_module_load()
{
    if (obs_get_version() != MAKE_SEMANTIC_VERSION(32, 0, 4)) {
        blog(LOG_ERROR, "[OBS-PlaylistDock] This build requires OBS 32.0.4.");
        return false;
    }
    if (QString::fromLatin1(qVersion()) != QStringLiteral("6.8.3")) {
        blog(LOG_ERROR, "[OBS-PlaylistDock] This build requires Qt 6.8.3; runtime is %s.", qVersion());
        return false;
    }
    char *configPath = obs_module_config_path("settings.json");
    const QString settingsFile = QString::fromUtf8(configPath ? configPath : "");
    bfree(configPath);
    auto *widget = new PlaylistDock(settingsFile);
    if (!obs_frontend_add_dock_by_id(DockId, obs_module_text("DockName"), widget)) {
        delete widget;
        return false;
    }
    dock = widget;
    QObject::connect(widget, &PlaylistDock::sceneSwitchRequested, widget, switchScene);
    QObject::connect(widget, &PlaylistDock::configurationChanged, widget, [] { obs_frontend_save(); });
    exiting = false;
    obs_frontend_add_event_callback(frontendEvent, nullptr);
    blog(LOG_INFO, "[OBS-PlaylistDock] %s loaded; Qt %s; dock registered.", PLAYLISTDOCK_VERSION, qVersion());
    return true;
}

void obs_module_unload()
{
    obs_frontend_remove_event_callback(frontendEvent, nullptr);
    if (!dock)
        return;
    dock->shutdown();
    if (!exiting)
        obs_frontend_remove_dock(DockId);
    // Destroy the widget before unloading its class code.
    if (dock)
        delete dock.data();
    dock.clear();
}
