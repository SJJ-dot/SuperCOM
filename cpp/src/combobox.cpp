#include "combobox.h"

#include <QAbstractItemView>
#include <QResizeEvent>
#include <QStyleOptionViewItem>
#include <QStyleOptionComboBox>
#include <QMouseEvent>
#include <QTimer>
#include <QApplication>

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

// 兜底：Release 后若 popup 仍开则手动选中点击的 item。
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

// 修复"高亮在主窗口外不跟随"：用 globalPos 手动换算 viewport 坐标。
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

// 全局鼠标拦截器（qApp 级）：popup 可见时拦截所有鼠标事件。
// 根因：Qt 6 的 QComboBox popup 在 DWM 无边框透明主窗口下，超出主窗口边界
// 的 popup viewport 收不到鼠标事件（WS_EX_LAYERED 分层窗口命中测试穿透）。
// 即使去掉 WS_EX_LAYERED 也不可靠。改用全局拦截 + mapFromGlobal 换算
// viewport 坐标做 indexAt，完全绕过 viewport 自身的事件接收。
class PopupGlobalMouseFilter : public QObject {
public:
    explicit PopupGlobalMouseFilter(QComboBox* cb) : m_cb(cb) {}
    bool eventFilter(QObject*, QEvent* e) override {
        if (!m_cb)
            return false;
        QAbstractItemView* v = m_cb->view();
        if (!v || !v->window() || !v->window()->isVisible())
            return false;
        const QEvent::Type t = e->type();
        if (t == QEvent::MouseMove) {
            auto* me = static_cast<QMouseEvent*>(e);
            const QPoint vp = v->viewport()->mapFromGlobal(me->globalPosition().toPoint());
            const QModelIndex idx = v->indexAt(vp);
            if (idx.isValid() && idx != v->currentIndex())
                v->setCurrentIndex(idx);
        } else if (t == QEvent::MouseButtonPress) {
            auto* me = static_cast<QMouseEvent*>(e);
            m_pressPos = me->globalPosition().toPoint();
            const QPoint vp = v->viewport()->mapFromGlobal(m_pressPos);
            const QModelIndex idx = v->indexAt(vp);
            if (idx.isValid())
                v->setCurrentIndex(idx);
        } else if (t == QEvent::MouseButtonRelease) {
            auto* me = static_cast<QMouseEvent*>(e);
            const QPoint releaseGpos = me->globalPosition().toPoint();
            const QPoint vp = v->viewport()->mapFromGlobal(releaseGpos);
            const QModelIndex idx = v->indexAt(vp);
            if (idx.isValid()) {
                m_cb->setCurrentIndex(idx.row());
                m_cb->hidePopup();
            } else {
                m_cb->hidePopup();
            }
        }
        return false;
    }
private:
    QComboBox* m_cb;
    QPoint m_pressPos;
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
        // 修复 1：去掉 popup 的 WS_EX_LAYERED
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
        // 修复 2+3+4：viewport 事件过滤器
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
        // 修复 5：全局鼠标拦截器（qApp 级）
        if (!m_globalMouseFilter) {
            m_globalMouseFilter = new PopupGlobalMouseFilter(this);
            qApp->installEventFilter(m_globalMouseFilter);
        }
    }
#endif
}

void StyledComboBox::hidePopup() {
    QComboBox::hidePopup();
}

void StyledComboBox::popupTab() {
    showPopup();
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
