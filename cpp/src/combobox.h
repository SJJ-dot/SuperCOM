#pragma once
// 主题化下拉框：在右侧叠加 QLabel 显示箭头（绕开 Qt 对 QSS 图标的兼容问题）。
// PortComboBox：端口下拉，宽度不足时末尾硬截断，弹出列表项末尾省略号、按内容宽度展开。

#include <QComboBox>
#include <QLabel>
#include <QStyledItemDelegate>

class PopupPressEnforceFilter;
class PopupFallbackFilter;
class PopupHoverFilter;
class PopupGlobalMouseFilter;

class StyledComboBox : public QComboBox {
    Q_OBJECT
public:
    explicit StyledComboBox(QWidget* parent = nullptr);
    void setArrowColor(const QColor& c);
    void popupTab();   // 公开包装：showPopup 给 tab 下拉用

protected:
    void resizeEvent(QResizeEvent* e) override;
    // 修复 popup 超出主窗口部分单击无效：
    // 1) 去掉 WS_EX_LAYERED 恢复矩形命中测试；
    // 2) Press 时强制 setCurrentIndex(点击项)，绕开 Qt 内部 currentIndex 不更新的问题；
    // 3) Release 后 popup 仍开则兜底手动选中；
    // 4) MouseMove 手动跟随高亮（Qt 容器 filter 在主窗口外 indexAt 失效）。
    // 5) 全局鼠标拦截（qApp 级）：popup viewport 在 DWM 无边框透明窗口下超出
    //    主窗口边界收不到鼠标事件，改用全局拦截 + mapFromGlobal 换算 viewport 坐标。
    void showPopup() override;
    void hidePopup() override;

private:
    QLabel* m_arrow = nullptr;
    PopupPressEnforceFilter* m_pressEnforceFilter = nullptr;
    PopupFallbackFilter* m_fallbackFilter = nullptr;
    PopupHoverFilter* m_hoverFilter = nullptr;
    PopupGlobalMouseFilter* m_globalMouseFilter = nullptr;
};

// 弹出列表项：省略号在末尾
class ElideRightDelegate : public QStyledItemDelegate {
public:
    using QStyledItemDelegate::QStyledItemDelegate;
protected:
    void initStyleOption(QStyleOptionViewItem* option, const QModelIndex& index) const override;
};

// 端口号下拉：收起时末尾硬截断；弹出时按内容宽度展开
class PortComboBox : public StyledComboBox {
    Q_OBJECT
public:
    explicit PortComboBox(QWidget* parent = nullptr);
};
