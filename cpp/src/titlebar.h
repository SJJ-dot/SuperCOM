#pragma once
// 自绘标题栏（无边框窗口用）：颜色完全可控，不受系统失焦变白影响。
// 含图标、标题（版本号，点击检查更新）、主题切换、最小化/最大化/关闭按钮，
// 支持拖动、双击最大化、右键菜单。

#include <QWidget>
#include <QLabel>
#include <QToolButton>

#include "themes.h"

class QMouseEvent;
class QContextMenuEvent;
class QHBoxLayout;

class TitleBar : public QWidget {
    Q_OBJECT
public:
    // appBar=true：主窗口标题栏（图标 + 标题/版本 + 示波器 + 主题 + 窗口按钮）
    // appBar=false：子窗口标题栏（图标 + 自定义标题 + 窗口按钮，可插入自定义按钮）
    explicit TitleBar(QWidget* window, bool appBar = true);

    void applyTheme(const sjj::ThemeColors& t, const QString& themeName);
    void setTitleText(const QString& text);   // 自定义标题（子窗口用，不可点击）

    // 在「最小化」左侧插入一个 30x30 按钮（如示波器的「?」）
    QToolButton* insertBeforeControls(const QString& text, const QString& tip);

signals:
    void versionClicked();   // 点击版本号（检查更新）
    void themeClicked();     // 点击主题切换按钮
    void oscilloClicked();   // 点击示波器按钮

protected:
    void mousePressEvent(QMouseEvent* e) override;
    void mouseMoveEvent(QMouseEvent* e) override;
    void mouseReleaseEvent(QMouseEvent* e) override;
    void mouseDoubleClickEvent(QMouseEvent* e) override;
    void contextMenuEvent(QContextMenuEvent* e) override;

private:
    void toggleMax();

    QWidget* m_win = nullptr;          // 顶层窗口引用
    bool m_appBar = true;              // 主窗口标题栏 / 子窗口标题栏
    QHBoxLayout* m_lay = nullptr;
    QLabel* m_lblIcon = nullptr;
    QLabel* m_lblTitle = nullptr;
    QToolButton* m_btnOsc = nullptr;
    QToolButton* m_btnTheme = nullptr;
    QToolButton* m_btnMin = nullptr;
    QToolButton* m_btnMax = nullptr;
    QToolButton* m_btnClose = nullptr;
    bool m_pressed = false;
    QPoint m_dragPos;
};
