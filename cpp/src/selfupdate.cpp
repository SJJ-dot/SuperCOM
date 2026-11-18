#include "selfupdate.h"

#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QString>
#include <QStringList>

#ifdef Q_OS_WIN
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <shellapi.h>
#endif

namespace {

QString g_logPath;      // 更新日志（exe 目录，不可写时退回临时目录）
QString g_targetPath;   // 被替换的 exe

// 写一行日志（带时间戳）。日志失败不影响更新流程。
void logLine(const QString& text) {
    if (g_logPath.isEmpty())
        return;
    QFile f(g_logPath);
    if (!f.open(QIODevice::Append | QIODevice::Text))
        return;
    f.write(QStringLiteral("[%1] %2\n")
                .arg(QDateTime::currentDateTime().toString(QStringLiteral("yyyy-MM-dd HH:mm:ss")),
                     text)
                .toUtf8());
    f.flush();
}

#ifdef Q_OS_WIN

QString lastErrorText(DWORD err) {
    wchar_t* buf = nullptr;
    const DWORD n = FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM
                                       | FORMAT_MESSAGE_IGNORE_INSERTS,
                                   nullptr, err, 0, reinterpret_cast<wchar_t*>(&buf), 0, nullptr);
    QString text = n ? QString::fromWCharArray(buf, int(n)).trimmed()
                     : QStringLiteral("错误码 %1").arg(err);
    if (buf)
        LocalFree(buf);
    return text;
}

std::wstring wide(const QString& path) {
    return QDir::toNativeSeparators(path).toStdWString();
}

// 覆盖式移动：杀软/索引器可能短暂占用目标文件，重试一会儿
bool moveWithRetry(const QString& from, const QString& to, QString* err) {
    const std::wstring s = wide(from);
    const std::wstring d = wide(to);
    DWORD last = 0;
    for (int i = 0; i < 40; ++i) {
        if (MoveFileExW(s.c_str(), d.c_str(), MOVEFILE_REPLACE_EXISTING))
            return true;
        last = GetLastError();
        if (last != ERROR_ACCESS_DENIED && last != ERROR_SHARING_VIOLATION
            && last != ERROR_USER_MAPPED_FILE) {
            break;
        }
        Sleep(250);   // 最多等 10 秒
    }
    if (err)
        *err = lastErrorText(last);
    return false;
}

bool launchExe(const QString& exe, const QString& workDir) {
    const std::wstring exeW = wide(exe);
    const std::wstring dirW = wide(workDir);
    std::wstring cmd = L"\"" + exeW + L"\"";
    STARTUPINFOW si{};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi{};
    if (!CreateProcessW(exeW.c_str(), cmd.data(), nullptr, nullptr, FALSE, 0, nullptr,
                        dirW.c_str(), &si, &pi)) {
        logLine(QStringLiteral("启动新版本失败：%1").arg(lastErrorText(GetLastError())));
        return false;
    }
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return true;
}

// 目录是否可写：只有写不进去才值得提权，避免文件被占用时白弹一次 UAC
bool dirWritable(const QString& dir) {
    QFile probe(dir + QStringLiteral("/.supercom_write_probe"));
    if (!probe.open(QIODevice::WriteOnly | QIODevice::Truncate))
        return false;
    probe.close();
    probe.remove();
    return true;
}

// 目标目录不可写（如装在 Program Files）时，以管理员身份重跑自己
bool relaunchElevated(const QString& self, DWORD pid) {
    const std::wstring selfW = wide(self);
    const std::wstring params =
        QStringLiteral("--self-update %1 --elevated").arg(pid).toStdWString();
    SHELLEXECUTEINFOW sei{};
    sei.cbSize = sizeof(sei);
    sei.fMask = SEE_MASK_NOCLOSEPROCESS;
    sei.lpVerb = L"runas";
    sei.lpFile = selfW.c_str();
    sei.lpParameters = params.c_str();
    sei.nShow = SW_SHOWNORMAL;
    if (!ShellExecuteExW(&sei)) {
        logLine(QStringLiteral("请求管理员权限失败：%1").arg(lastErrorText(GetLastError())));
        return false;
    }
    logLine(QStringLiteral("已以管理员身份重新启动更新器"));
    if (sei.hProcess)
        CloseHandle(sei.hProcess);
    return true;
}

void showFailure(const QString& reason) {
    const QString text = QStringLiteral(
                             "更新失败：%1\n\n原版本已保留，程序会继续使用旧版本。\n"
                             "详细信息见日志：\n%2")
                             .arg(reason, g_logPath);
    MessageBoxW(nullptr, reinterpret_cast<const wchar_t*>(text.utf16()),
                L"SuperCOM 更新", MB_ICONWARNING | MB_OK);
}

#endif   // Q_OS_WIN

}   // namespace

namespace selfupdate {

bool runIfRequested(int argc, char** argv) {
#ifndef Q_OS_WIN
    Q_UNUSED(argc)
    Q_UNUSED(argv)
    return false;
#else
    bool wantUpdate = false;
    bool elevated = false;
    DWORD pid = 0;
    for (int i = 1; i < argc; ++i) {
        const QByteArray arg(argv[i]);
        if (arg == "--self-update") {
            wantUpdate = true;
            if (i + 1 < argc)
                pid = DWORD(atoi(argv[i + 1]));   // 纯数字，和代码页无关
        } else if (arg == "--elevated") {
            elevated = true;
        }
    }
    if (!wantUpdate)
        return false;

    // 自身即新版本 exe；目标固定为同目录的主程序
    wchar_t selfBuf[MAX_PATH * 4] = {0};
    const DWORD selfLen = GetModuleFileNameW(nullptr, selfBuf, DWORD(sizeof(selfBuf) / sizeof(wchar_t)));
    const QString self = QString::fromWCharArray(selfBuf, int(selfLen));
    const QFileInfo selfInfo(self);
    const QString dir = selfInfo.absolutePath();
    const QString target = dir + QStringLiteral("/SuperCOM.exe");
    const QString staging = dir + QStringLiteral("/SuperCOM.pending.exe");
    const QString backup = target + QStringLiteral(".bak");
    g_targetPath = target;
    g_logPath = dir + QStringLiteral("/SuperCOM_update.log");
    if (!QFileInfo(QFileInfo(g_logPath).absolutePath()).isWritable()) {
        g_logPath = QDir::tempPath() + QStringLiteral("/SuperCOM_update.log");
    }

    logLine(QStringLiteral("===== 开始自更新：新版本 %1，旧进程 pid=%2%3 =====")
                .arg(self, QString::number(pid), elevated ? QStringLiteral("（管理员）")
                                                          : QString()));
    if (!QFileInfo::exists(target)) {
        const QString err = QStringLiteral("目标不存在：%1").arg(target);
        logLine(err);
        showFailure(err);
        return true;
    }

    // 1) 等旧进程退出（最长 30 秒；超时也继续，靠后面的重试兜底）
    if (pid != 0) {
        HANDLE h = OpenProcess(SYNCHRONIZE, FALSE, pid);
        if (h) {
            const DWORD w = WaitForSingleObject(h, 30000);
            logLine(w == WAIT_OBJECT_0 ? QStringLiteral("旧进程已退出")
                                       : QStringLiteral("等待旧进程超时，继续尝试替换"));
            CloseHandle(h);
        } else {
            logLine(QStringLiteral("旧进程已不存在（pid=%1）").arg(pid));
        }
    }

    // 2) 把自己复制成暂存文件：运行中的 exe 不能改名/删除，但可以读
    DeleteFileW(wide(staging).c_str());
    if (!CopyFileW(wide(self).c_str(), wide(staging).c_str(), FALSE)) {
        const DWORD e = GetLastError();
        const QString err = QStringLiteral("复制新版本失败：%1").arg(lastErrorText(e));
        logLine(err);
        DeleteFileW(wide(staging).c_str());
        if (!elevated && !dirWritable(dir) && relaunchElevated(self, pid)) {
            return true;   // 已提权重跑，本进程退出
        }
        launchExe(target, dir);   // 失败也把旧版本拉起来，别让用户没程序用
        showFailure(err);
        return true;
    }
    const qint64 srcSize = selfInfo.size();
    if (QFileInfo(staging).size() != srcSize) {
        const QString err = QStringLiteral("复制不完整（%1 ≠ %2 字节）")
                                .arg(QFileInfo(staging).size())
                                .arg(srcSize);
        logLine(err);
        DeleteFileW(wide(staging).c_str());
        launchExe(target, dir);
        showFailure(err);
        return true;
    }
    logLine(QStringLiteral("已暂存新版本：%1（%2 字节）").arg(staging).arg(srcSize));

    // 3) 备份旧版本 → 4) 暂存文件替换目标；任一步失败都回滚
    DeleteFileW(wide(backup).c_str());
    const bool hadBackup = MoveFileExW(wide(target).c_str(), wide(backup).c_str(),
                                       MOVEFILE_REPLACE_EXISTING) != 0;
    logLine(hadBackup ? QStringLiteral("旧版本已备份为 %1").arg(backup)
                      : QStringLiteral("备份旧版本失败（继续尝试替换）"));

    QString err;
    if (!moveWithRetry(staging, target, &err)) {
        logLine(QStringLiteral("替换失败：%1").arg(err));
        if (hadBackup)
            MoveFileExW(wide(backup).c_str(), wide(target).c_str(), MOVEFILE_REPLACE_EXISTING);
        DeleteFileW(wide(staging).c_str());
        if (!elevated && !dirWritable(dir) && relaunchElevated(self, pid)) {
            return true;
        }
        launchExe(target, dir);
        showFailure(QStringLiteral("无法替换程序文件：%1").arg(err));
        return true;
    }

    // 5) 校验替换结果
    if (QFileInfo(target).size() != srcSize) {
        const QString msg = QStringLiteral("替换后大小不符（%1 ≠ %2 字节），已回滚")
                                .arg(QFileInfo(target).size())
                                .arg(srcSize);
        logLine(msg);
        if (hadBackup) {
            DeleteFileW(wide(target).c_str());
            MoveFileExW(wide(backup).c_str(), wide(target).c_str(), MOVEFILE_REPLACE_EXISTING);
        }
        launchExe(target, dir);
        showFailure(msg);
        return true;
    }
    logLine(QStringLiteral("替换完成并校验通过：%1").arg(target));

    // 6) 清理备份 → 拉起新版本
    DeleteFileW(wide(backup).c_str());
    if (!launchExe(target, dir)) {
        showFailure(QStringLiteral("新版本已就位，但启动失败，请手动运行 %1").arg(target));
        return true;
    }
    logLine(QStringLiteral("新版本已启动，更新流程结束"));
    // 运行中的自身删不掉，安排重启后删除（无管理员权限时会失败，忽略即可：
    // 下次下载会用同名文件覆盖）
    MoveFileExW(wide(self).c_str(), nullptr, MOVEFILE_DELAY_UNTIL_REBOOT);
    return true;
#endif
}

}   // namespace selfupdate
