#include "combobox.h"

#include <QAbstractItemView>
#include <QResizeEvent>
#include <QStyleOptionViewItem>
#include <QStyleOptionComboBox>
#include <QMouseEvent>
#include <QTimer>

#ifdef Q_OS_WIN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

// Press 时强制把 view 的 currentIndex 同步为点击的 item。
// 根因：Qt 的 QAbstractItemView::mousePressEvent 在 currentIndex 已被
// MouseMove 悬停高亮改成"鼠标悬停项"后，本次 setCurrentIndex(点击项)
// 不会更新 currentIndex，导致容器 eventFilter 的 release 判定读到旧值。
// 强制 setCurrentIndex 确保 currentIndex 与点击项同步。
class PopupPressEnforceFilter : public QObject {
public:
    bool eventFilter(QObject* obj, QEvent* e) override {
        if (e->type() == QEvent::MouseButtonPress) {
            if (auto* v = qobject_cast<QAbstractItemView*>(obj->parent())) {
                auto* me = static_cast<QMouseEvent*>(e);
                const QModelIndex idx = v->indexAt(me->position().toPoint());
                if (idx.isValid())
                    v->setCurrentIndex(idx);
            }
        }
        return false;
    }
};

// 兜底：Qt 容器 release 判定链在你的机器上始终失败（5 项条件全 true 仍不
// 触发 itemSelected），Release 后若 popup 仍开着则手动选中点击的 item。
class PopupFallbackFilter : public QObject {
public:
    explicit PopupFallbackFilter(QComboBox* cb) : m_cb(cb) {}
    bool eventFilter(QObject* obj, QEvent* e) override {
        if (e->type() == QEvent::MouseButtonPress) {
            auto* me = static_cast<QMouseEvent*>(e);
            m_pressPos = me->position().toPoint();
        } else if (e->type() == QEvent::MouseButtonRelease) {
            const QPoint pressPos = m_pressPos;
            QTimer::singleShot(0, this, [this, pressPos] {
                QAbstractItemView* v = m_cb->view();
                if (v && v->window() && v->window()->isVisible()) {
                    const QModelIndex idx = v->indexAt(pressPos);
                    if (idx.isValid()) {
                        m_cb->setCurrentIndex(idx.row());
                        m_cb->hidePopup();
                    }
                }
            });
        }
        return false;
    }
private:
    QComboBox* m_cb;
    QPoint m_pressPos;
};

// 修复"高亮在主窗口外不跟随"：Qt 容器 eventFilter 的 MouseMove case 用
// m->position()（事件接收者本地坐标）做 indexAt，鼠标移出主窗口后该坐标
// 换算异常导致 indexAt 返回无效，setCurrentIndex 不执行，高亮"卡"在边缘。
// 这里用 globalPos 手动换算 viewport 坐标，保证 currentIndex 跟随悬停项。
class PopupHoverFilter : public QObject {
public:
    bool eventFilter(QObject* obj, QEvent* e) override {
        if (e->type() == QEvent::MouseMove) {
            if (auto* v = qobject_cast<QAbstractItemView*>(obj->parent())) {
                auto* me = static_cast<QMouseEvent*>(e);
                const QPoint vp = v->viewport()->mapFromGlobal(me->globalPosition().toPoint());
                const QModelIndex idx = v->indexAt(vp);
                if (idx.isValid() && idx != v->currentIndex())
                    v->setCurrentIndex(idx);
            }
        }
        return false;
    }
};

StyledComboBox::StyledComboBox(QWidget* parent) : QComboBox(parent) {
    m_arrow = new QLabel(QStringLiteral("\u25BE"), this);
    m_arrow->setAttribute(Qt::WA_TransparentForMouseEvents, true);
    m_arrow->setAlignment(Qt::AlignCenter);
    m_arrow->setStyleSheet(QStringLiteral("background:transparent;color:#4C4F69;font-size:14px;"));
    m_arrow->resize(18, height());
    m_arrow->move(width() - 18, 0);
    m_arrow->show();
}

void StyledComboBox::setArrowColor(const QColor& c) {
    m_arrow->setStyleSheet(QStringLiteral("background:transparent;color:%1;font-size:14px;")
                               .arg(c.name()));
    m_arrow->update();
}

void StyledComboBox::resizeEvent(QResizeEvent* e) {
    QComboBox::resizeEvent(e);
    m_arrow->resize(18, height());
    m_arrow->move(width() - 18, 0);
}

void StyledComboBox::showPopup() {
    QComboBox::showPopup();
#ifdef Q_OS_WIN
    if (QWidget* popup = view()->window(); popup && popup != this) {
        // 修复 1：去掉 popup 的 WS_EX_LAYERED（Qt 6 的 QComboBox popup 默认是
        // 分层窗口，per-pixel alpha 命中测试在 DWM 无边框透明主窗口下会穿透）。
        popup->setAttribute(Qt::WA_TranslucentBackground, false);
        popup->setAttribute(Qt::WA_NoSystemBackground, false);
        if (HWND h = reinterpret_cast<HWND>(popup->winId())) {
            LONG_PTR ex = GetWindowLongPtr(h, GWL_EXSTYLE);
            if (ex & WS_EX_LAYERED) {
                SetWindowLongPtr(h, GWL_EXSTYLE, ex & ~WS_EX_LAYERED);
                SetWindowPos(h, nullptr, 0, 0, 0, 0,
                             SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_FRAMECHANGED);
            }
        }
        // 修复 2 + 3 + 4：装三个事件过滤器
        if (!m_pressEnforceFilter) {
            m_pressEnforceFilter = new PopupPressEnforceFilter;
            view()->viewport()->installEventFilter(m_pressEnforceFilter);
        }
        if (!m_fallbackFilter) {
            m_fallbackFilter = new PopupFallbackFilter(this);
            view()->viewport()->installEventFilter(m_fallbackFilter);
        }
        if (!m_hoverFilter) {
            m_hoverFilter = new PopupHoverFilter;
            view()->viewport()->installEventFilter(m_hoverFilter);
        }
    }
#endif
}

void ElideRightDelegate::initStyleOption(QStyleOptionViewItem* option,
                                         const QModelIndex& index) const {
    QStyledItemDelegate::initStyleOption(option, index);
    option->textElideMode = Qt::ElideRight;
}

PortComboBox::PortComboBox(QWidget* parent) : StyledComboBox(parent) {
    setItemDelegate(new ElideRightDelegate(this));
    setSizeAdjustPolicy(QComboBox::AdjustToContents);
}
