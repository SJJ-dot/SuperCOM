#include "combobox.h"

#include <QDateTime>
#include <QGuiApplication>
#include <QHideEvent>
#include <QPainter>
#include <QPen>
#include <QScreen>
#include <QStyle>

// 下拉框外观全部交给全局 QSS（themes.cpp），本类只做三件事：
//   1) 自绘下拉箭头（原生箭头跟随系统调色板，深色主题下几乎看不见）；
//   2) 用自绘的 ListPopup 替代 Qt 原生弹层（与快捷命令分组浮层同一套外观）；
//   3) 弹窗位置：默认向下展开，越过主窗口底边再向上（屏幕比窗口大，否则弹层会
//      落到窗口外、超出窗口的条目点不到）。

namespace {
// 当前主题缓存：applyTheme 会刷新所有已存在的下拉框，但对话框等"运行期新建"的
// 下拉框在构造时也需要箭头颜色，故在构造时从这里取。
sjj::ThemeColors g_themeColors;

// 弹层除内容外的宽度：两侧留白(2×8) + 卡片左右内边距(2×6) + 卡片边框(2) +
// 竖向滚动条预留(14)
constexpr int kPopupChrome = ListPopup::kPad * 2 + 6 * 2 + 2 + 14;

// 弹层关闭后多久内忽略重开请求（点一下收起时，同一次点击会被继续转发给下拉框）
constexpr qint64 kReopenGuardMs = 200;
} // namespace

StyledComboBox::StyledComboBox(QWidget* parent) : QComboBox(parent) {
    m_arrowColor = QColor(g_themeColors.value(QStringLiteral("text_secondary")));
}

void StyledComboBox::setThemeColors(const sjj::ThemeColors& t) {
    g_themeColors = t;
    m_arrowColor = QColor(t.value(QStringLiteral("text_secondary")));
    update();
}

void StyledComboBox::paintEvent(QPaintEvent* e) {
    QComboBox::paintEvent(e);
    if (!m_arrowColor.isValid())
        return;
    // 下拉箭头：右侧留白区内画一个 chevron（∨）
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing, true);
    QColor c = m_arrowColor;
    if (!isEnabled())
        c.setAlpha(120);
    QPen pen(c);
    pen.setWidthF(1.6);
    pen.setCapStyle(Qt::RoundCap);
    pen.setJoinStyle(Qt::RoundJoin);
    p.setPen(pen);
    const qreal cx = width() - 12.0;
    const qreal cy = height() / 2.0;
    p.drawLine(QPointF(cx - 4.0, cy - 2.0), QPointF(cx, cy + 2.0));
    p.drawLine(QPointF(cx, cy + 2.0), QPointF(cx + 4.0, cy - 2.0));
}

void StyledComboBox::setPopupOpen(bool open) {
    if (property("popupOpen").toBool() == open)
        return;
    setProperty("popupOpen", open);
    style()->unpolish(this);
    style()->polish(this);
    update();
}

void StyledComboBox::showPopup() {
    if (count() <= 0)
        return;
    if (m_popup) {          // 已展开：再点一次收起
        hidePopup();
        return;
    }

    // 收起时那一次点击，Qt 关掉浮动窗口后还会把它继续传给下拉框，于是紧接着
    // 又调用到这里、弹层立刻被重新打开，表现就是"再点一次收不起来"。刚关掉的
    // 一小段时间内忽略重开请求即可（真正的新点击远大于这个间隔）。
    if (m_popupClosedAtMs > 0) {
        const qint64 dt = QDateTime::currentMSecsSinceEpoch() - m_popupClosedAtMs;
        m_popupClosedAtMs = 0;
        if (dt >= 0 && dt < kReopenGuardMs)
            return;
    }

    QStringList items;
    items.reserve(count());
    for (int i = 0; i < count(); ++i)
        items << itemText(i);

    // 内容宽度：至少让卡片与下拉框等宽，更长条目由 ListPopup 自身撑开
    const int minContentW = qMax(0, width() - kPopupChrome);
    auto* pop = new ListPopup(items, currentIndex(), g_themeColors, this, minContentW);
    m_popup = pop;
    pop->onPick = [this](int i) {
        if (i >= 0 && i < count())
            setCurrentIndex(i);
    };
    // 弹层不管被谁关掉（自己关 / 点空白处由 Qt 关）都会走到这里：复位高亮并记下
    // 关闭时间，供上面的"防立刻重开"判断使用。
    connect(pop, &QObject::destroyed, this, [this] {
        setPopupOpen(false);
        m_popupClosedAtMs = QDateTime::currentMSecsSinceEpoch();
    });

    pop->move(popupTopLeft(pop->size()));
    setPopupOpen(true);
    pop->show();
    pop->armAfterShow();
    pop->setFocus(Qt::PopupFocusReason);
}

void StyledComboBox::hidePopup() {
    if (m_popup) {
        ListPopup* pop = m_popup;
        m_popup = nullptr;
        pop->close();       // WA_DeleteOnClose，随后自销毁
    }
    setPopupOpen(false);
    QComboBox::hidePopup();
}

void StyledComboBox::hideEvent(QHideEvent* e) {
    hidePopup();            // 下拉框被隐藏/切页时不要留下浮层
    QComboBox::hideEvent(e);
}

QPoint StyledComboBox::popupTopLeft(const QSize& popupSize) const {
    const QRect box(mapToGlobal(QPoint(0, 0)), size());
    const int pad = ListPopup::kPad;
    const int gap = 4;

    // 弹层是顶层 Qt::Popup，允许超出父窗口（尤其「更多串口设置」这种小对话框）。
    // 若按父窗口边界钳制，比对话框还高的列表会被压到对话框顶部，反过来盖住触发
    // 它的下拉框：鼠标一松开就落进列表行、立即选中并关闭，表现为「点开就消失」。
    // 因此一律按屏幕可用区域定位，并保证卡片不与下拉框重叠。
    QRect avail;
    if (QScreen* s = QGuiApplication::screenAt(box.center()))
        avail = s->availableGeometry();
    else if (QScreen* s2 = QGuiApplication::primaryScreen())
        avail = s2->availableGeometry();
    else
        avail = QRect(box.center() - QPoint(400, 300), QSize(800, 600));

    const int need = popupSize.height() - pad;
    const int below = avail.bottom() + 1 - box.bottom() - gap;
    const int above = box.top() - avail.top() - gap;
    // 卡片顶部贴下拉框底边 / 卡片底部贴下拉框顶边，两侧都不重叠
    const bool down = (below >= need) || (above < need && below >= above);
    int x = box.left() - pad;   // 卡片左边缘与下拉框左边缘对齐
    int y = down ? (box.bottom() + gap - pad)
                 : (box.top() - gap - popupSize.height() + pad);

    // 只在屏幕范围内兜底；不按父窗口收缩
    x = qBound(avail.left(), x,
               qMax(avail.left(), avail.right() + 1 - popupSize.width()));
    y = qBound(avail.top(), y,
               qMax(avail.top(), avail.bottom() + 1 - popupSize.height()));
    return QPoint(x, y);
}
