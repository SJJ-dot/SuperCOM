#include <QApplication>
#include <QFont>
#include <QIcon>
#include <QFileInfo>
#include <QCoreApplication>
#include <QtPlugin>

#include "serialtool.h"

// ===== 静态链接 Qt 时显式导入插件（由 CMake 定义 SUPERCOM_STATIC_BUILD） =====
#ifdef SUPERCOM_STATIC_BUILD
Q_IMPORT_PLUGIN(QWindowsIntegrationPlugin)      // Windows 平台集成（必需）
Q_IMPORT_PLUGIN(QModernWindowsStylePlugin)      // Windows 原生样式
#endif

int main(int argc, char** argv) {
    QApplication app(argc, argv);
    app.setApplicationName(QStringLiteral("SuperCOM"));
    // 关闭下拉框弹出动画：QComboBox 在 SH_ComboBox_Popup=0 时会用 qScrollEffect
    // 播放"向下滚动展开"动画，与自定义弹窗定位叠加会出现"先向下展开再换向"的观感。
    QApplication::setEffectEnabled(Qt::UI_AnimateCombo, false);
    // 统一控件文字字号 = 接收区数据文本字号（10pt），字体族保持系统默认（中文正常显示）
    QFont f = app.font();
    f.setPointSize(10);
    app.setFont(f);
    // 应用图标（窗口标题栏 + 任务栏）：优先内嵌资源，其次程序目录/项目 imgs 目录
    QStringList candidates = {
        QStringLiteral(":/imgs/ic_xue_xi.png"),
        QCoreApplication::applicationDirPath() + QStringLiteral("/imgs/ic_xue_xi.png"),
        QCoreApplication::applicationDirPath() + QStringLiteral("/../imgs/ic_xue_xi.png"),
    };
    for (const QString& p : candidates) {
        if (QFileInfo::exists(p)) {
            app.setWindowIcon(QIcon(p));
            break;
        }
    }
    SerialTool win;
    if (!app.windowIcon().isNull())
        win.setWindowIcon(app.windowIcon());
    win.show();
    return app.exec();
}
