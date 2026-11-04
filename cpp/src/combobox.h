#pragma once
#include <QColor>
#include <QComboBox>
#include <QtGlobal>
#include <QPointer>
#include <QRect>

#include "listpopup.h"
#include "themes.h"

// 统一样式的下拉框：
// 1) 外观由全局 QSS（themes.cpp）绘制，箭头由本类自绘（保证深浅主题都清晰）；
// 2) 弹层换成自绘的 ListPopup，与快捷命令分组浮层同一套（半透明窗口 + 留白 +
//    圆角卡片）。原生 QComboBox 弹层由平台绘制，圆角、留白、阴影各系统表现不一，
//    Windows 高 DPI 下圆角切口还会露出窗口底色（看起来像右下角黑角），故弃用。
class StyledComboBox : public QComboBox {
    Q_OBJECT
public:
    explicit StyledComboBox(QWidget* parent = nullptr);

    // 跟随主题刷新箭头颜色
    void setThemeColors(const sjj::ThemeColors& t);

    void showPopup() override;
    void hidePopup() override;

protected:
    void paintEvent(QPaintEvent* e) override;
    void hideEvent(QHideEvent* e) override;

private:
    // 弹层左上角（含四周留白）：默认贴下拉框下方，放不下再翻到上方，并钳在主窗口内
    QPoint popupTopLeft(const QSize& popupSize) const;
    // 展开时给下拉框加高亮边框（等价于原生弹层的 :on 状态）
    void setPopupOpen(bool open);

    QColor m_arrowColor;
    QPointer<ListPopup> m_popup;
    // 弹层关闭时刻：用于吞掉"关闭那一次点击"连带产生的立刻重开请求
    qint64 m_popupClosedAtMs = 0;
};

// 端口号下拉（样式与通用下拉一致）
class PortComboBox : public StyledComboBox {
    Q_OBJECT
public:
    explicit PortComboBox(QWidget* parent = nullptr) : StyledComboBox(parent) {}
};
