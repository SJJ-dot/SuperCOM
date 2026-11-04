#pragma once
#include <QFrame>
#include <QPointer>
#include <QStringList>
#include <functional>

#include "themes.h"

class QLabel;
class QScrollArea;

// 统一下拉列表浮层：快捷命令分组、端口/波特率/编码/校验等下拉框共用。
//
// 半透明 Qt::Popup 窗口 + 四周留白 + 内层圆角卡片，全部自绘。原生 QComboBox
// 弹层由平台绘制，圆角、留白、阴影各系统表现不一（Windows 高 DPI 下圆角切口
// 还会露出窗口底色，表现为"右下角黑角"），所以统一换成这一套。
class ListPopup : public QFrame {
    Q_OBJECT
public:
    static constexpr int kPad = 8;        // 浮层四周留白（透明，不遮挡下层）
    static constexpr int kRadius = 6;     // 卡片圆角（与输入框、选项卡等一致）
    static constexpr int kRowHeight = 30; // 行高
    static constexpr int kMaxHeight = 360;

    // current：当前项（-1 表示无）；minContentWidth：卡片内容最小宽度
    ListPopup(const QStringList& items, int current, const sjj::ThemeColors& c,
              QWidget* parent, int minContentWidth = 0);

    std::function<void(int)> onPick;

protected:
    void keyPressEvent(QKeyEvent* e) override;

private:
    void moveCurrent(int delta);
    void activateCurrent();
    void applyCurrent();

    QList<QWidget*> m_rows;
    QList<QLabel*> m_labels;
    QPointer<QScrollArea> m_scroll;
    int m_current = -1;

    QString m_textColor;
    QString m_accentColor;
    QString m_accentFg;
    QString m_hoverBg;
    QString m_itemBorder;   // 单条 item 的细描边（比外层卡片更浅，形成层叠卡片层次）
};
