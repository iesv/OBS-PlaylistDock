// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <obs.hpp>
#include <QElapsedTimer>
#include <QHash>
#include <QList>
#include <QTimer>
#include <QWidget>
#include <mutex>

class QAction;
class QComboBox;
class QGridLayout;
class QLabel;
class QLineEdit;
class QListWidget;
class QToolButton;
class QSlider;
class QResizeEvent;

class PlaylistDock final : public QWidget {
    Q_OBJECT
public:
    explicit PlaylistDock(const QString &settingsFile, QWidget *parent = nullptr);
    ~PlaylistDock() override;
    void resume();
    void suspend();
    void shutdown();
    void refreshSources();

signals:
    void configurationChanged();
    void sceneSwitchRequested(const QString &sourceUuid);

private:
    struct Track { QString id; QString path; qint64 bytes = -2; };
    struct RemovedTrack { Track track; int row; };
    struct ViewState { QString selected; QString query; int scroll = 0; };
    enum EndMode { Stop = 0, Next = 1, Repeat = 2 };

    void buildUi();
    void bindSource(const QString &uuid);
    void rememberView();
    void restoreView(const QString &uuid);
    void loadQueue(obs_source_t *source, bool force = false);
    void storeQueue();
    void rebuildList(const QString &selected = {});
    void filterList();
    void orderChanged();
    void moveSelected(int offset);
    void addFiles();
    void appendFiles(const QStringList &files);
    void removeSelected();
    void undoRemove();
    void locateCurrent();
    void openFolder();
    void restoreSource();
    void playSelected();
    void playTrack(const QString &id, bool switchScene = false);
    void cancelSwitch();
    static void mediaStopped(void *context, calldata_t *data);
    void playRelative(int offset);
    void togglePlay();
    void stopPlayback();
    void tick();
    void updateButtons();
    void updateTime(qint64 position);
    void toggleTimeDisplay();
    bool eventFilter(QObject *watched, QEvent *event) override;
    void resizeEvent(QResizeEvent *event) override;
    void saveSelection();
    void clearError();
    void showError(const QString &value);
    QString selectedId() const;
    int currentIndex() const;
    obs_source_t *lockSource() const;

    QString settingsFile_;
    QString wantedUuid_;
    OBSWeakSource source_;
    OBSSignal stoppedSignal_;
    std::mutex switchMutex_;
    OBSData pendingPlayback_;
    quint64 switchGeneration_ = 0;
    bool switching_ = false;
    bool switchSceneOnStart_ = false;
    QList<Track> tracks_;
    QList<RemovedTrack> removedTracks_;
    QHash<QString, ViewState> views_;
    QStringList observed_;
    QString currentId_;
    bool managed_ = false;
    bool originalLoop_ = false;
    bool originalShuffle_ = false;
    QString originalBehavior_;
    bool suspended_ = true;
    bool closing_ = false;
    bool rebuilding_ = false;
    bool remainingTime_ = false;
    bool compactHeader_ = false;
    bool endArmed_ = false;
    int endMode_ = Stop;
    int pollCount_ = 0;
    qint64 duration_ = 0;
    qint64 seekTarget_ = -1;
    QElapsedTimer lastCommand_;
    QElapsedTimer seekCommand_;
    QTimer poll_;
    QTimer save_;

    QComboBox *sources_ = nullptr;
    QGridLayout *heading_ = nullptr;
    QWidget *sourceBar_ = nullptr;
    QLineEdit *search_ = nullptr;
    QListWidget *list_ = nullptr;
    QLabel *count_ = nullptr;
    QLabel *time_ = nullptr;
    QLabel *message_ = nullptr;
    QSlider *seek_ = nullptr;
    QComboBox *endModes_ = nullptr;
    QToolButton *play_ = nullptr;
    QToolButton *previous_ = nullptr;
    QToolButton *next_ = nullptr;
    QToolButton *stop_ = nullptr;
    QToolButton *add_ = nullptr;
    QAction *remove_ = nullptr;
    QAction *undo_ = nullptr;
    QAction *locate_ = nullptr;
    QAction *openFolder_ = nullptr;
    QAction *up_ = nullptr;
    QAction *down_ = nullptr;
    QAction *top_ = nullptr;
    QAction *bottom_ = nullptr;
    QAction *restore_ = nullptr;
};

