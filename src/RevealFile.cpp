// SPDX-License-Identifier: GPL-2.0-or-later
#include "RevealFile.hpp"
#include <QDir>
#ifdef Q_OS_WIN
#include <windows.h>
#include <shlobj.h>
#else
#include <QProcess>
#endif

bool revealFile(const QString &path)
{
#ifdef Q_OS_WIN
    const HRESULT initialized = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    if (FAILED(initialized) && initialized != RPC_E_CHANGED_MODE)
        return false;
    const QString native = QDir::toNativeSeparators(path);
    PIDLIST_ABSOLUTE item = ILCreateFromPathW(reinterpret_cast<LPCWSTR>(native.utf16()));
    const bool opened = item && SUCCEEDED(SHOpenFolderAndSelectItems(item, 0, nullptr, 0));
    CoTaskMemFree(item);
    if (SUCCEEDED(initialized))
        CoUninitialize();
    return opened;
#else
    return QProcess::startDetached(QStringLiteral("/usr/bin/open"), {QStringLiteral("-R"), path});
#endif
}
