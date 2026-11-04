#include "listpopup.h"

#include <QFont>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QLabel>
#include <QMouseEvent>
#include <QScrollArea>
#include <QScrollBar>
#include <QTimer>
#include <QVBoxLayout>

namespace {

// 下拉行：整行可点击，hover 由 QSS 驱动
class PopupRow : public QWidget {
public:
    explicit PopupRow(QWidget* parent = nullptr) : QWidget(parent) {}

    std::function<void()> onClick;

protected:
    void mouseReleaseEvent(QMouseEvent* e) override {
        if (e->button() == Qt::LeftButton && rect().contains(e->pos()) && onClick)
            onClick();
        QWidget::mouseReleaseEvent(e);
    }
};

} // namespace

ListPopup::ListPopup(const QStringList& items, int current, const sjj::ThemeColors& c,
                     QWidget* parent, int minContentWidth)
    : QFrame(parent, Qt::Popup | Qt::FramelessWindowHint | Qt::NoDropShadowWindowHint) {
    setAttribute(Qt::WA_TranslucentBackground);
    setAttribute(Qt::WA_DeleteOnClose);
    setFocusPolicy(Qt::StrongFocus);

    m_textColor   = c.value(QStringLiteral("text_primary"));
    m_accentColor = c.value(QStringLiteral("accent"));
    m_accentFg    = c.value(QStringLiteral("accent_fg"));
    m_hoverBg     = c.value(QStringLiteral("list_sel"));
    m_itemBorder  = c.value(QStringLiteral("panel_border"));
    m_itemBg      = c.value(QStringLiteral("combo_item_bg"));
    const QString bg     = c.value(QStringLiteral("popup_bg"));
    const QString border = c.value(QStringLiteral("input_border"));

    auto* outer = new QVBoxLayout(this);
    outer->setContentsMargins(kPad, kPad, kPad, kPad);
    outer->setSpacing(0);

    auto* card = new QFrame(this);
    card->setObjectName(QStringLiteral("listPopupCard"));
    card->setStyleSheet(QStringLiteral(
        "QFrame#listPopupCard{background:%1;border:1px solid %2;border-radius:%3px;}")
        .arg(bg, border).arg(kRadius));
    outer->addWidget(card);

    auto* cardLay = new QVBoxLayout(card);
    cardLay->setContentsMargins(6, 6, 6, 6);
    cardLay->setSpacing(0);

    auto* scroll = new QScrollArea(card);
    scroll->setFrameShape(QFrame::NoFrame);
    scroll->setWidgetResizable(true);
    scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    scroll->setStyleSheet(QStringLiteral("QScrollArea{background:transparent;}"));
    m_scroll = scroll;

    auto* host = new QWidget(scroll);
    auto* listLay = new QVBoxLayout(host);
    listLay->setContentsMargins(0, 0, 0, 0);
    listLay->setSpacing(2);

    int maxTextW = 120;
    for (int i = 0; i < items.size(); ++i) {
        auto* row = new PopupRow(host);
        row->setFixedHeight(kRowHeight);
        row->setCursor(Qt::PointingHandCursor);
        row->setAttribute(Qt::WA_Hover, true);
        row->setAttribute(Qt::WA_StyledBackground, true);

        auto* h = new QHBoxLayout(row);
        h->setContentsMargins(12, 0, 12, 0);
        h->setSpacing(0);

        auto* lab = new QLabel(items.at(i), row);
        lab->setStyleSheet(QStringLiteral("background:transparent;"));
        h->addWidget(lab, 1);

        maxTextW = qMax(maxTextW, lab->sizeHint().width());
        row->onClick = [this, i] {
            m_current = i;
            if (onPick)
                onPick(i);
            close();
        };
        m_rows << row;
        m_labels << lab;
        listLay->addWidget(row);
    }
    listLay->addStretch(1);

    const int contentW = qMax(maxTextW + 40, minContentWidth);
    host->setMinimumWidth(contentW);
    scroll->setWidget(host);

    // 容器高度 = 行高总和（含行间距）后再封顶，避免条目少时上下留一大片空白
    const int rowsH = items.size() * kRowHeight + qMax(0, items.size() - 1) * 2;
    const int scrollH = qMin(rowsH, kMaxHeight);
    const int scrollW = contentW + 14;      // 预留竖向滚动条宽度
    scroll->setFixedSize(scrollW, scrollH);
    cardLay->addWidget(scroll);

    // 尺寸自己算死：窗口 = 四周留白 + 卡片（滚动区 + 卡片内边距 + 1px 边框×2）。
    // 不用 adjustSize()，它给出的高度会比布局实际需要的大，多出来的空白会被拉伸
    // 到卡片里，看上去就是"上下空白太多"。
    setFixedSize(kPad * 2 + scrollW + 6 * 2 + 2,
                 kPad * 2 + scrollH + 6 * 2 + 2);

    if (current >= 0 && current < items.size())
        m_current = current;
    applyCurrent();

    // 弹层出现后把当前项滚到可见区（列表比浮层高时才有意义）
    const int cur = m_current;
    if (cur >= 0 && cur < m_rows.size()) {
        QPointer<QScrollArea> sc = scroll;
        QWidget* row = m_rows.at(cur);
        QTimer::singleShot(0, this, [sc, row] {
            if (sc)
                sc->ensureWidgetVisible(row, 0, 0);
        });
    }
}

void ListPopup::applyCurrent() {
    for (int i = 0; i < m_rows.size(); ++i) {
        const bool cur = (i == m_current);
        m_rows.at(i)->setObjectName(cur ? QStringLiteral("listPopupRowCur")
                                        : QStringLiteral("listPopupRow"));
        // 重新设置样式表会触发重新 polish，objectName 变化才能立即生效。
        // 未选中项 = 白（深色下为略亮的表面色）卡片，配细描边 + 2px 间距，浮在弹窗
        // 底板上形成层次；悬停用 list_sel；当前项为强调色实心卡片。
        m_rows.at(i)->setStyleSheet(QStringLiteral(
            "QWidget#listPopupRow{background:%3;border:1px solid %1;border-radius:4px;}"
            "QWidget#listPopupRow:hover{background:%2;border:1px solid %1;border-radius:4px;}"
            "QWidget#listPopupRowCur{background:%4;border:1px solid %4;border-radius:4px;}")
            .arg(m_itemBorder, m_hoverBg, m_itemBg, m_accentColor));

        QFont f = m_labels.at(i)->font();
        f.setBold(cur);
        m_labels.at(i)->setFont(f);
        m_labels.at(i)->setStyleSheet(QStringLiteral("color:%1;background:transparent;")
                                          .arg(cur ? m_accentFg : m_textColor));
        m_rows.at(i)->update();
    }
}

void ListPopup::moveCurrent(int delta) {
    if (m_rows.isEmpty())
        return;
    int idx = m_current + delta;
    idx = qBound(0, idx, m_rows.size() - 1);
    if (idx == m_current)
        return;
    m_current = idx;
    applyCurrent();
    if (m_scroll)
        m_scroll->ensureWidgetVisible(m_rows.at(idx), 0, 0);
}

void ListPopup::activateCurrent() {
    if (m_current < 0 || m_current >= m_rows.size())
        return;
    if (onPick)
        onPick(m_current);
    close();
}

void ListPopup::keyPressEvent(QKeyEvent* e) {
    switch (e->key()) {
    case Qt::Key_Up:
        moveCurrent(-1);
        return;
    case Qt::Key_Down:
        moveCurrent(1);
        return;
    case Qt::Key_PageUp:
        moveCurrent(-5);
        return;
    case Qt::Key_PageDown:
        moveCurrent(5);
        return;
    case Qt::Key_Home:
        moveCurrent(-m_rows.size());
        return;
    case Qt::Key_End:
        moveCurrent(m_rows.size());
        return;
    case Qt::Key_Return:
    case Qt::Key_Enter:
    case Qt::Key_Space:
        activateCurrent();
        return;
    case Qt::Key_Escape:
        close();
        return;
    default:
        break;
    }
    QFrame::keyPressEvent(e);
}
