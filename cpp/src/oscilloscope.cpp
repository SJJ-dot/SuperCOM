#include "oscilloscope.h"

#include <QAbstractButton>
#include <QCheckBox>
#include <QClipboard>
#include <QColor>
#include <QDialog>
#include <QEvent>
#include <QFont>
#include <QFontMetrics>
#include <QFrame>
#include <QGridLayout>
#include <QGuiApplication>
#include <QHBoxLayout>
#include <QJsonArray>
#include <QJsonObject>
#include <QLabel>
#include <QLayoutItem>
#include <QContextMenuEvent>
#include <QLineEdit>
#include <QMenu>
#include <QMessageBox>
#include <QMouseEvent>
#include <QPainter>
#include <QPalette>
#include <QPen>
#include <QPushButton>
#include <QScrollArea>
#include <QShowEvent>
#include <QStringList>
#include <QTimer>
#include <QToolButton>
#include <QVBoxLayout>
#include <QWheelEvent>

#include <algorithm>
#include <cmath>

#ifdef Q_OS_WIN
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <dwmapi.h>
#endif

#include "titlebar.h"

namespace {

// 倍率文本 → 实际乘数：支持 "2" / "*0.1" / "/100"
bool parseScaleText(const QString& text, double& out) {
    QString t = text.trimmed();
    out = 1.0;
    if (t.isEmpty())
        return true;
    bool divide = false;
    if (t.startsWith(QLatin1Char('/'))) {
        divide = true;
        t.remove(0, 1);
    } else if (t.startsWith(QLatin1Char('*'))) {
        t.remove(0, 1);
    }
    bool ok = false;
    const double v = t.toDouble(&ok);
    if (!ok || v == 0 || !std::isfinite(v))
        return false;
    out = divide ? (1.0 / v) : v;
    return true;
}

QString formatValue(double v) {
    if (!std::isfinite(v))
        return QStringLiteral("-");
    if (std::abs(v) >= 10000 || (v != 0 && std::abs(v) < 0.01))
        return QString::number(v, 'e', 2);
    return QString::number(v, 'f', 2);
}

// 图例里的倍率后缀："2" → " ×2"，"/2" → " ÷2"，"*0.1" → " ×0.1"，"1" → 空
QString scaleSuffix(const QString& text) {
    const QString t = text.trimmed();
    if (t.isEmpty() || t == QLatin1String("1"))
        return {};
    if (t.startsWith(QLatin1Char('/')))
        return QStringLiteral(" ÷") + t.mid(1);
    if (t.startsWith(QLatin1Char('*')))
        return QStringLiteral(" ×") + t.mid(1);
    return QStringLiteral(" ×") + t;
}

// 多条折线的固定配色（浅色/深色主题下都能看清）
const QVector<QColor>& seriesColors() {
    static const QVector<QColor> colors{
        QColor(QStringLiteral("#3B82F6")), QColor(QStringLiteral("#E11D48")),
        QColor(QStringLiteral("#10B981")), QColor(QStringLiteral("#F59E0B")),
        QColor(QStringLiteral("#8B5CF6")), QColor(QStringLiteral("#06B6D4")),
        QColor(QStringLiteral("#84CC16")), QColor(QStringLiteral("#EC4899")),
    };
    return colors;
}

// 从一次正则匹配里取数字：优先捕获组 1，否则整个匹配
const QRegularExpression& numberRe() {
    static const QRegularExpression re(
        QStringLiteral("[-+]?(?:\\d+\\.?\\d*|\\.\\d+)(?:[eE][-+]?\\d+)?"));
    return re;
}

// 字节取值写法：b2 / b2:3 / b2:3le / i2:3（b=无符号 i=有符号，le=小端，默认大端）
// 「第 N 字节」从 1 开始计数，区间含首尾。
// 可追加帧校验：#1=02,2=03,len=8,len>=6,sum=0,xor=0,crc16=0（逗号分隔）
bool parseByteSpec(const QString& text, OscByteSpec& out) {
    out = OscByteSpec();
    QString t = text.trimmed();
    if (t.size() < 2)
        return false;

    // 先切出 # 之后的帧校验条件
    QStringList conds;
    const int hash = t.indexOf(QLatin1Char('#'));
    if (hash >= 0) {
        const QString tail = t.mid(hash + 1).trimmed();
        if (!tail.isEmpty())
            conds = tail.split(QRegularExpression(QStringLiteral("[,，]")),
                               Qt::SkipEmptyParts);
        t = t.left(hash).trimmed();
    }

    // 取值区间
    if (t.size() < 2)
        return false;
    const QChar head = t.at(0).toLower();
    if (head != QLatin1Char('b') && head != QLatin1Char('i'))
        return false;
    out.sign = (head == QLatin1Char('i'));
    t.remove(0, 1);
    const QString lt = t.toLower();
    if (lt.endsWith(QLatin1String("le"))) {
        out.bigEndian = false;
        t.chop(2);
    } else if (lt.endsWith(QLatin1String("be"))) {
        t.chop(2);
    }
    const QStringList parts = t.split(QLatin1Char(':'));
    if (parts.isEmpty() || parts.size() > 2)
        return false;
    bool okA = false;
    out.from = parts.at(0).toInt(&okA);
    if (!okA)
        return false;
    if (parts.size() == 2) {
        bool okB = false;
        out.to = parts.at(1).toInt(&okB);
        if (!okB)
            return false;
    } else {
        out.to = out.from;
    }
    // 只接受 1~4 字节整数（double 可精确表示）
    if (out.from < 1 || out.to < out.from || (out.to - out.from + 1) > 4)
        return false;

    // 帧校验条件
    for (QString c : conds) {
        c = c.trimmed().toLower();
        if (c.isEmpty())
            continue;
        if (c.startsWith(QLatin1String("len"))) {
            const bool ge = c.contains(QLatin1String(">="));
            const int eq = c.indexOf(QLatin1Char('='));
            if (eq < 0)
                return false;
            bool ok = false;
            const int v = c.mid(eq + 1).trimmed().toInt(&ok);
            if (!ok || v < 1)
                return false;
            if (ge) {
                out.check.lenMin = v;
            } else {
                out.check.lenExact = v;
            }
            continue;
        }
        if (c == QLatin1String("crc16") || c == QLatin1String("crc16=0")) {
            out.check.crc16Modbus = true;
            continue;
        }
        const int eq = c.indexOf(QLatin1Char('='));
        if (eq <= 0)
            return false;
        const QString key = c.left(eq).trimmed();
        bool okV = false;
        const int v = c.mid(eq + 1).trimmed().toInt(&okV, 16);
        if (!okV || v < 0 || v > 0xFF)
            return false;
        if (key == QLatin1String("sum")) {
            out.check.sum8 = v;
        } else if (key == QLatin1String("xor")) {
            out.check.xor8 = v;
        } else {
            bool okIdx = false;
            const int idx = key.toInt(&okIdx);
            if (!okIdx || idx < 1)
                return false;
            out.check.byteEq.append(qMakePair(idx, v));
        }
    }
    return true;
}

// 帧校验：长度 / 指定字节 / 累加和 / 异或和 / Modbus CRC16
bool frameCheckPass(const QByteArray& raw, const OscFrameCheck& c) {
    if (c.lenExact >= 0 && raw.size() != c.lenExact)
        return false;
    if (c.lenMin > 0 && raw.size() < c.lenMin)
        return false;
    for (const auto& eq : c.byteEq) {
        if (raw.size() < eq.first || quint8(raw.at(eq.first - 1)) != eq.second)
            return false;
    }
    if (c.sum8 >= 0) {
        int acc = 0;
        for (char b : raw)
            acc = (acc + quint8(b)) & 0xFF;
        if (acc != c.sum8)
            return false;
    }
    if (c.xor8 >= 0) {
        int x = 0;
        for (char b : raw)
            x ^= quint8(b);
        if (x != c.xor8)
            return false;
    }
    if (c.crc16Modbus) {
        quint16 crc = 0xFFFF;
        for (char b : raw) {
            crc ^= quint8(b);
            for (int i = 0; i < 8; ++i)
                crc = (crc & 1) ? quint16((crc >> 1) ^ 0xA001) : quint16(crc >> 1);
        }
        if (crc != 0)
            return false;
    }
    return true;
}

// 按区间从原始字节取整数；帧长度不足时返回 false
bool byteValueOf(const QByteArray& raw, const OscByteSpec& spec, double& out) {
    const int n = spec.to - spec.from + 1;
    if (raw.size() < spec.to)
        return false;
    quint64 v = 0;
    if (spec.bigEndian) {
        for (int i = 0; i < n; ++i)
            v = (v << 8) | quint8(raw.at(spec.from - 1 + i));
    } else {
        for (int i = n - 1; i >= 0; --i)
            v = (v << 8) | quint8(raw.at(spec.from - 1 + i));
    }
    if (spec.sign) {
        const int bits = n * 8;
        const quint64 signBit = quint64(1) << (bits - 1);
        if (v & signBit)
            v |= (~quint64(0)) << bits;
        out = double(qint64(v));
    } else {
        out = double(v);
    }
    return true;
}

} // namespace

// ==================== WaveChart ====================

WaveChart::WaveChart(QWidget* parent) : QWidget(parent) {
    setAttribute(Qt::WA_StyledBackground, true);
    setMinimumHeight(240);
}

void WaveChart::bindSeries(const QVector<OscSeries>* series) {
    m_series = series;
    update();
}

void WaveChart::setThemeColors(const sjj::ThemeColors& t) {
    m_tc = t;
    update();
}

void WaveChart::paintEvent(QPaintEvent*) {
    QPainter p(this);
    const QColor bg(m_tc.value(QStringLiteral("panel_bg")));
    const QColor grid(m_tc.value(QStringLiteral("panel_border")));
    const QColor secondary(m_tc.value(QStringLiteral("text_secondary")));
    const QColor primary(m_tc.value(QStringLiteral("text_primary")));
    p.fillRect(rect(), bg);
    p.setPen(QPen(grid, 1));
    p.drawRect(rect().adjusted(0, 0, -1, -1));

    // 图例放在绘图区上方的独立条带里，避免遮住折线；横向排布，放不下自动换行（最多两行）
    struct LegendItem {
        QColor color;
        QString text;
    };
    QVector<LegendItem> legend;
    if (m_series) {
        int ci = 0;
        for (const OscSeries& s : *m_series) {
            if (s.enabled && !s.pattern.isEmpty() && !s.values.isEmpty()) {
                legend.append({seriesColors().at(ci % seriesColors().size()),
                               s.pattern + scaleSuffix(s.scaleText) + QStringLiteral(" = ")
                                   + formatValue(s.values.last() * s.scale)});
            }
            ++ci;
        }
    }
    const QFontMetrics fm(font());
    const int legendW = qMax(0, width() - 58 - 12);
    int legendLines = 0;
    if (!legend.isEmpty() && legendW > 80) {
        int used = 0;
        legendLines = 1;
        for (const LegendItem& it : legend) {
            const int w = 15 + fm.horizontalAdvance(it.text) + 14;
            if (used > 0 && used + w > legendW) {
                if (++legendLines > 2)
                    break;
                used = 0;
            }
            used += w;
        }
    }
    const int legendH = legendLines > 0 ? legendLines * 18 + 2 : 0;

    const QRect area = rect().adjusted(58, 10 + legendH, -12, -26);
    m_plot = area;
    m_i0 = 0;
    m_i1 = -1;
    if (area.width() < 40 || area.height() < 30 || !m_series)
        return;

    // 图例（左对齐到绘图区，最多两行）
    if (legendLines > 0) {
        int x = area.left();
        int y = 10;
        for (const LegendItem& it : legend) {
            QString text = it.text;
            const int avail = area.width();
            if (fm.horizontalAdvance(text) > avail - 15)
                text = fm.elidedText(text, Qt::ElideRight, qMax(20, avail - 15));
            const int w = 15 + fm.horizontalAdvance(text) + 14;
            if (x > area.left() && x + w > area.right()) {
                x = area.left();
                y += 18;
            }
            if (y > 10 + (legendLines - 1) * 18)
                break;
            p.fillRect(QRect(x, y + 4, 10, 9), it.color);
            p.setPen(primary);
            p.drawText(QRect(x + 14, y, w - 14, 17), Qt::AlignLeft | Qt::AlignVCenter, text);
            x += w;
        }
    }

    // 横轴窗口：xSpan=显示点数（0=全部），xRight=窗口右端（-1=跟随最新）
    const int total = dataCount();
    if (total <= 0) {
        p.setPen(secondary);
        p.drawText(area, Qt::AlignCenter, QStringLiteral("等待数据…"));
        return;
    }
    int i1 = (m_view.xRight < 0) ? total - 1 : qBound(0, m_view.xRight, total - 1);
    int span = (m_view.xSpan <= 0 || total < 2) ? total : qBound(2, m_view.xSpan, total);
    int i0 = qMax(0, i1 - span + 1);
    i1 = qMin(total - 1, i0 + span - 1);

    // 纵轴范围：自动取窗口内的数据范围，或用手动设置的 min/max
    double minY = 0, maxY = 0;
    bool any = false;
    for (const OscSeries& s : *m_series) {
        if (!s.enabled || s.values.isEmpty())
            continue;
        const int j1 = qMin(i1, int(s.values.size()) - 1);
        for (int i = i0; i <= j1; ++i) {
            const double sv = s.values.at(i) * s.scale;
            if (!any) {
                minY = maxY = sv;
                any = true;
            } else {
                minY = qMin(minY, sv);
                maxY = qMax(maxY, sv);
            }
        }
    }
    if (!any) {
        p.setPen(secondary);
        p.drawText(area, Qt::AlignCenter, QStringLiteral("该范围暂无数据…"));
        return;
    }
    const bool manualY = !m_view.yAuto && m_view.yMax > m_view.yMin;
    if (manualY) {
        minY = m_view.yMin;
        maxY = m_view.yMax;
    } else {
        if (qFuzzyCompare(minY, maxY)) {
            minY -= 1;
            maxY += 1;
        }
        const double padY = (maxY - minY) * 0.08;
        minY -= padY;
        maxY += padY;
    }
    m_i0 = i0;
    m_i1 = i1;
    m_y0 = minY;
    m_y1 = maxY;

    const auto toY = [&](double v) -> qreal {
        return area.bottom() - (v - minY) / (maxY - minY) * area.height();
    };
    const auto toX = [&](int i) -> qreal {
        const int d = qMax(1, i1 - i0);
        return area.left() + double(i - i0) / d * area.width();
    };

    // 网格 + 轴刻度
    QPen dash(grid, 1, Qt::DotLine);
    // Y 轴固定 4 段
    for (int i = 0; i <= 4; ++i) {
        const int y = area.top() + area.height() * i / 4;
        p.setPen(dash);
        p.drawLine(area.left(), y, area.right(), y);
        p.setPen(secondary);
        p.drawText(QRect(area.left() - 54, y - 9, 50, 18),
                   Qt::AlignRight | Qt::AlignVCenter,
                   formatValue(maxY - (maxY - minY) * i / 4));
    }
    // X 轴：标注窗口内的采样序号，段数随跨度变化，点数少时刻度不会重复
    const int xSpan = qMax(0, i1 - i0);
    const int xSteps = qBound(1, xSpan, 4);
    for (int i = 0; i <= xSteps; ++i) {
        const int x = area.left() + area.width() * i / xSteps;
        p.setPen(dash);
        p.drawLine(x, area.top(), x, area.bottom());
        if (xSpan == 0 && i > 0)
            continue; // 只有一个点时右侧不再重复画刻度
        p.setPen(secondary);
        p.drawText(QRect(x - 28, area.bottom() + 3, 56, 18), Qt::AlignCenter,
                   QString::number(qlonglong(i0) + qlonglong(xSpan) * i / xSteps));
    }

    // 折线（只画窗口内的点）
    int ci = 0;
    for (const OscSeries& s : *m_series) {
        if (!s.enabled || s.values.isEmpty()) {
            ++ci;
            continue;
        }
        const int j0 = qMax(0, i0);
        const int j1 = qMin(i1, int(s.values.size()) - 1);
        QPen line(seriesColors().at(ci % seriesColors().size()));
        line.setWidthF(1.6);
        line.setCapStyle(Qt::RoundCap);
        line.setJoinStyle(Qt::RoundJoin);
        p.setPen(line);
        for (int i = j0 + 1; i <= j1; ++i)
            p.drawLine(QPointF(toX(i - 1), toY(s.values.at(i - 1) * s.scale)),
                       QPointF(toX(i), toY(s.values.at(i) * s.scale)));
        if (j1 == j0)
            p.drawEllipse(QPointF(toX(j0), toY(s.values.at(j0) * s.scale)), 2.5, 2.5);
        ++ci;
    }

    // 框选中的橡皮筋
    if (m_selActive) {
        const QRect sel = QRect(m_selFrom, m_selTo).normalized().intersected(area);
        if (sel.isValid() && sel.width() > 1 && sel.height() > 1) {
            QColor accent(m_tc.value(QStringLiteral("accent")));
            accent.setAlpha(48);
            p.fillRect(sel, accent);
            QPen sp(QColor(m_tc.value(QStringLiteral("accent"))), 1, Qt::DashLine);
            p.setPen(sp);
            p.drawRect(sel);
        }
    }
}

int WaveChart::dataCount() const {
    int n = 0;
    if (!m_series)
        return 0;
    for (const OscSeries& s : *m_series) {
        if (s.enabled)
            n = qMax(n, int(s.values.size()));
    }
    return n;
}

int WaveChart::indexAtX(int x) const {
    if (m_i1 < m_i0 || m_plot.width() <= 0)
        return m_i0;
    double fx = double(x - m_plot.left()) / m_plot.width();
    fx = qBound(0.0, fx, 1.0);
    return m_i0 + int(qRound(fx * (m_i1 - m_i0)));
}

double WaveChart::valueAtY(int y) const {
    if (m_plot.height() <= 0)
        return m_y0;
    double fy = double(m_plot.bottom() - y) / m_plot.height();
    fy = qBound(0.0, fy, 1.0);
    return m_y0 + fy * (m_y1 - m_y0);
}

void WaveChart::setView(const WaveView& v) {
    m_view = v;
    update();
}

void WaveChart::resetView() {
    m_view = WaveView();
    update();
}

void WaveChart::wheelEvent(QWheelEvent* e) {
    const int delta = e->angleDelta().y();
    const QPoint pos = e->position().toPoint();
    if (delta == 0 || m_i1 < m_i0 || !m_plot.contains(pos)) {
        QWidget::wheelEvent(e);
        return;
    }
    const double factor = delta > 0 ? 0.8 : 1.25;   // 上滚放大
    if (e->modifiers() & (Qt::ShiftModifier | Qt::ControlModifier)) {
        // 纵轴缩放：以鼠标处的数值为锚点
        const double at = valueAtY(pos.y());
        const double lo = m_y0, hi = m_y1;
        const double n0 = at - (at - lo) * factor;
        const double n1 = at + (hi - at) * factor;
        if (n1 - n0 > 1e-12) {
            m_view.yAuto = false;
            m_view.yMin = n0;
            m_view.yMax = n1;
        }
    } else {
        // 横轴缩放：跟随最新时只改窗口宽度；窗口已固定时以鼠标处的序号为锚点伸缩
        const int total = qMax(2, dataCount());
        const int curSpan = m_i1 - m_i0 + 1;
        const int newSpan = qBound(2, int(qRound(curSpan * factor)), total);
        if (newSpan >= total) {
            m_view.xSpan = 0;                 // 拉到最大＝全部数据
        } else if (m_view.xRight < 0) {
            m_view.xSpan = newSpan;           // 仍跟随最新
        } else {
            const double fx = qBound(0.0, double(pos.x() - m_plot.left()) / m_plot.width(), 1.0);
            const int anchor = indexAtX(pos.x());
            int newI0 = anchor - int(qRound(fx * (newSpan - 1)));
            int newI1 = newI0 + newSpan - 1;
            if (newI1 > total - 1) {
                newI1 = total - 1;
                newI0 = newI1 - newSpan + 1;
            }
            if (newI0 < 0) {
                newI0 = 0;
                newI1 = newSpan - 1;
            }
            m_view.xSpan = newSpan;
            m_view.xRight = newI1;
        }
    }
    update();
    e->accept();
}

void WaveChart::mousePressEvent(QMouseEvent* e) {
    if (e->button() == Qt::LeftButton && m_i1 >= m_i0
        && m_plot.contains(e->position().toPoint())) {
        m_selActive = true;
        m_selFrom = m_selTo = e->position().toPoint();
        setCursor(Qt::CrossCursor);
        update();
        e->accept();
        return;
    }
    QWidget::mousePressEvent(e);
}

void WaveChart::mouseMoveEvent(QMouseEvent* e) {
    if (m_selActive) {
        const QPoint p(e->position().toPoint().x(), e->position().toPoint().y());
        m_selTo = QPoint(qBound(m_plot.left(), p.x(), m_plot.right()),
                         qBound(m_plot.top(), p.y(), m_plot.bottom()));
        update();
        e->accept();
        return;
    }
    QWidget::mouseMoveEvent(e);
}

void WaveChart::mouseReleaseEvent(QMouseEvent* e) {
    if (e->button() == Qt::LeftButton && m_selActive) {
        m_selActive = false;
        unsetCursor();
        const QPoint p(e->position().toPoint().x(), e->position().toPoint().y());
        m_selTo = QPoint(qBound(m_plot.left(), p.x(), m_plot.right()),
                         qBound(m_plot.top(), p.y(), m_plot.bottom()));
        const QRect sel = QRect(m_selFrom, m_selTo).normalized();
        if (sel.width() > 8 && sel.height() > 8) {
            const int total = dataCount();
            int a = indexAtX(sel.left());
            int b = indexAtX(sel.right());
            if (a > b)
                std::swap(a, b);
            const int span = qBound(2, b - a + 1, qMax(2, total));
            m_view.xSpan = span;
            m_view.xRight = b;
            const double top = valueAtY(sel.top());
            const double bot = valueAtY(sel.bottom());
            if (qAbs(top - bot) > 1e-12) {
                m_view.yAuto = false;
                m_view.yMin = qMin(top, bot);
                m_view.yMax = qMax(top, bot);
            }
            update();
        } else {
            update();
        }
        e->accept();
        return;
    }
    QWidget::mouseReleaseEvent(e);
}

void WaveChart::mouseDoubleClickEvent(QMouseEvent* e) {
    if (e->button() == Qt::LeftButton) {
        resetView();   // 双击复位：全部数据 + 纵轴自动
        e->accept();
        return;
    }
    QWidget::mouseDoubleClickEvent(e);
}

void WaveChart::contextMenuEvent(QContextMenuEvent* e) {
    QMenu menu(this);
    QAction* actReset = menu.addAction(QStringLiteral("重置"));
    menu.addSeparator();
    QAction* actAutoY = menu.addAction(QStringLiteral("自动纵轴"));
    actAutoY->setCheckable(true);
    actAutoY->setChecked(m_view.yAuto);
    QAction* actLatest = menu.addAction(QStringLiteral("最新数据"));
    actLatest->setCheckable(true);
    actLatest->setChecked(m_view.xRight < 0);

    QAction* picked = menu.exec(e->globalPos());
    if (!picked) {
        e->accept();
        return;
    }
    if (picked == actReset) {
        // 横轴、纵轴一起复位：全部数据 + 自动纵轴
        m_view = WaveView();
    } else if (picked == actAutoY) {
        if (actAutoY->isChecked()) {
            m_view.yAuto = true;
        } else {
            // 取消自动：锁定当前显示的纵轴范围
            m_view.yAuto = false;
            m_view.yMin = m_y0;
            m_view.yMax = m_y1;
        }
    } else if (picked == actLatest) {
        if (actLatest->isChecked()) {
            // 勾选：保持当前窗口点数，右端始终跟随最新数据
            m_view.xRight = -1;
        } else if (m_i1 >= m_i0) {
            // 取消勾选：窗口停在当前显示的数据上，不再自动跟随
            m_view.xSpan = qMax(0, m_i1 - m_i0 + 1);
            m_view.xRight = m_i1;
        }
    }
    update();
    e->accept();
}

// ==================== Oscilloscope ====================

Oscilloscope::Oscilloscope(QWidget* parent) : QWidget(parent) {
    // 无边框 + 自绘标题栏：与主窗口一致，「?」放进标题栏（最小化左侧）
    setWindowFlags(Qt::Window | Qt::FramelessWindowHint);
    setAttribute(Qt::WA_TranslucentBackground, true);
    setWindowTitle(QStringLiteral("示波器"));
    resize(760, 520);
    setMinimumSize(520, 340);

    auto* outer = new QVBoxLayout(this);
    outer->setContentsMargins(0, 0, 0, 0);
    outer->setSpacing(0);

    m_titleBar = new TitleBar(this, false);
    m_titleBar->setTitleText(windowTitle());
    QToolButton* btnHelp = m_titleBar->insertBeforeControls(
        QStringLiteral("?"), QStringLiteral("示波器操作提示"));
    connect(btnHelp, &QToolButton::clicked, this, &Oscilloscope::showHelp);
    outer->addWidget(m_titleBar);

    auto* content = new QWidget(this);
    outer->addWidget(content, 1);
    auto* root = new QVBoxLayout(content);
    root->setContentsMargins(8, 8, 8, 8);
    root->setSpacing(6);

    m_chart = new WaveChart(content);
    m_chart->bindSeries(&m_series);
    root->addWidget(m_chart, 1);

    // 表达式配置收进弹窗：主界面只留一个入口，图表区域尽量大
    auto* bar = new QHBoxLayout;
    m_btnCfg = new QPushButton(content);
    m_btnCfg->setToolTip(QStringLiteral("打开提取表达式配置（关闭后收起，改动立即生效）"));
    connect(m_btnCfg, &QPushButton::clicked, this, &Oscilloscope::openConfig);
    bar->addWidget(m_btnCfg);
    bar->addStretch(1);
    auto* btnClear = new QPushButton(QStringLiteral("清空数据"), content);
    connect(btnClear, &QPushButton::clicked, this, &Oscilloscope::clearData);
    bar->addWidget(btnClear);
    root->addLayout(bar);

    buildConfigDialog();
    m_series.append(OscSeries());
    rebuildRows();
    installResizeSupport();
}

void Oscilloscope::showEvent(QShowEvent* e) {
    QWidget::showEvent(e);
#ifdef Q_OS_WIN
    // 与主窗口一致：DWM 系统阴影 + Win11 圆角（无边框窗口自带）
    const HWND hwnd = HWND(winId());
    MARGINS margins = {-1, -1, -1, -1};
    DwmExtendFrameIntoClientArea(hwnd, &margins);
    const int pref = 2;   // DWMWCP_ROUND
    DwmSetWindowAttribute(hwnd, 33 /* DWMWA_WINDOW_CORNER_PREFERENCE */, &pref, sizeof(pref));
#endif
    installResizeSupport();
}

void Oscilloscope::paintEvent(QPaintEvent*) {
    // 无边框窗口：自己画不透明背景（主题 win_bg），圆角由系统裁剪
    QColor bg(m_tc.value(QStringLiteral("win_bg")));
    if (!bg.isValid())
        bg = palette().window().color();
    QPainter p(this);
    p.fillRect(rect(), bg);
}

// 边缘缩放：鼠标跟踪 + 事件过滤（子控件不拦截窗口边缘的拖动）
void Oscilloscope::installResizeSupport() {
    setMouseTracking(true);
    if (!property("resizeFilterInstalled").toBool()) {
        installEventFilter(this);
        setProperty("resizeFilterInstalled", true);
    }
    const auto children = findChildren<QWidget*>();
    for (QWidget* w : children) {
        if (w->window() != this || w->property("resizeFilterInstalled").toBool())
            continue;
        w->setMouseTracking(true);
        w->installEventFilter(this);
        w->setProperty("resizeFilterInstalled", true);
    }
}

int Oscilloscope::detectResizeEdge(const QPoint& pos) const {
    if (isMaximized())
        return 0;
    if (!rect().contains(pos))
        return 0;
    const int m = m_resizeMargin;
    int edges = 0;
    if (pos.x() <= m)
        edges |= EdgeLeft;
    if (pos.x() >= width() - m)
        edges |= EdgeRight;
    if (pos.y() <= m)
        edges |= EdgeTop;
    if (pos.y() >= height() - m)
        edges |= EdgeBottom;
    return edges;
}

Qt::CursorShape Oscilloscope::resizeCursor(int edges) {
    if (edges == EdgeLeft || edges == EdgeRight)
        return Qt::SizeHorCursor;
    if (edges == EdgeTop || edges == EdgeBottom)
        return Qt::SizeVerCursor;
    if (edges == (EdgeLeft | EdgeTop) || edges == (EdgeRight | EdgeBottom))
        return Qt::SizeFDiagCursor;
    if (edges == (EdgeRight | EdgeTop) || edges == (EdgeLeft | EdgeBottom))
        return Qt::SizeBDiagCursor;
    return Qt::ArrowCursor;
}

void Oscilloscope::applyResize(const QPoint& gpos) {
    const int edge = m_resizeEdge;
    QRect geo = m_resizeStartGeo;
    const int dx = gpos.x() - m_resizeStartPos.x();
    const int dy = gpos.y() - m_resizeStartPos.y();
    const int minW = minimumWidth(), minH = minimumHeight();
    if (edge & EdgeLeft) {
        const int newW = geo.width() - dx;
        if (newW >= minW) {
            geo.setX(geo.x() + dx);
            geo.setWidth(newW);
        } else {
            geo.setX(geo.x() + geo.width() - minW);
            geo.setWidth(minW);
        }
    }
    if (edge & EdgeRight)
        geo.setWidth(qMax(minW, geo.width() + dx));
    if (edge & EdgeTop) {
        const int newH = geo.height() - dy;
        if (newH >= minH) {
            geo.setY(geo.y() + dy);
            geo.setHeight(newH);
        } else {
            geo.setY(geo.y() + geo.height() - minH);
            geo.setHeight(minH);
        }
    }
    if (edge & EdgeBottom)
        geo.setHeight(qMax(minH, geo.height() + dy));
    setGeometry(geo);
}

void Oscilloscope::showHelp() {
    QMessageBox box(QMessageBox::Information, QStringLiteral("示波器操作提示"),
                    QStringLiteral(
                        "数据来源：接收区每产生一个分包（一条接收记录），就把该包文本作为一次匹配输入，"
                        "每个正则运行一次、取首个匹配；分包由接收区的分包规则决定，与换行符无关。\n"
                        "也可以按字节取值：同一个分包算一帧，如 b2:3 取第 2~3 字节（大端）。\n\n"
                        "鼠标操作\n"
                        "· 滚轮：缩放横轴（时间窗）\n"
                        "· Shift / Ctrl + 滚轮：缩放纵轴\n"
                        "· 左键拖拽：框选放大\n"
                        "· 双击：恢复全部数据\n"
                        "· 右键：重置 / 自动纵轴 / 最新数据\n\n"
                        "提取表达式（每个表达式一条折线，倍率只影响显示）\n"
                        "· 文本：正则如 xpp=(\\d+)，取该分包第一个匹配\n"
                        "· 字节：b2:3 大端 / b2:3le 小端 / i2:3 有符号 / i2:3le 小端有符号"
                        " / b5 单字节（第 1 字节起算）\n"
                        "· 帧校验（字节取值可加在 # 之后）：b2:3#1=02,2=03 校帧头，"
                        "len=8 / len>=6 校长度；sum=0 / xor=0 / crc16=0 表示整包（含帧尾校验字节）"
                        "按累加和 / 异或 / Modbus CRC 算出来等于 0，即协议里校验通过的帧；"
                        "不满足的分包（如混在其中的日志）不画点\n"
                        "· 「清空数据」只清曲线，不改表达式配置"),
                    QMessageBox::NoButton, this);
    QAbstractButton* btnOk =
        box.addButton(QStringLiteral("知道了"), QMessageBox::AcceptRole);
    box.setEscapeButton(btnOk);   // Esc 也能关掉
    box.exec();
}

// 表达式配置弹窗：非模态，关闭只是收起，不销毁，改动实时作用于 m_series
void Oscilloscope::buildConfigDialog() {
    if (m_cfgDlg)
        return;
    m_cfgDlg = new QDialog(this);
    m_cfgDlg->setWindowTitle(QStringLiteral("提取表达式"));
    m_cfgDlg->resize(680, 320);
    m_cfgDlg->setMinimumSize(560, 240);
    auto* lay = new QVBoxLayout(m_cfgDlg);
    lay->setContentsMargins(10, 10, 10, 10);
    lay->setSpacing(8);

    auto* head = new QHBoxLayout;
    head->addWidget(new QLabel(QStringLiteral("文本按分包取数，也可按字节取值（b2:3）；倍率只影响显示"),
                               m_cfgDlg));
    head->addStretch(1);
    auto* btnHelp = new QPushButton(QStringLiteral("写法帮助"), m_cfgDlg);
    btnHelp->setToolTip(QStringLiteral("正则与字节取值写法说明、可直接复制的示例"));
    connect(btnHelp, &QPushButton::clicked, this, &Oscilloscope::showRegexHelp);
    head->addWidget(btnHelp);
    auto* btnAdd = new QPushButton(QStringLiteral("＋ 添加表达式"), m_cfgDlg);
    connect(btnAdd, &QPushButton::clicked, this, &Oscilloscope::addSeries);
    head->addWidget(btnAdd);
    lay->addLayout(head);

    m_rowsScroll = new QScrollArea(m_cfgDlg);
    m_rowsScroll->setWidgetResizable(true);
    m_rowsScroll->setFrameShape(QFrame::NoFrame);
    m_rowsScroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    m_rowsScroll->setVerticalScrollBarPolicy(Qt::ScrollBarAsNeeded);
    auto* rowsHost = new QWidget(m_rowsScroll);
    m_rowsLay = new QVBoxLayout(rowsHost);
    m_rowsLay->setContentsMargins(0, 0, 0, 0);
    m_rowsLay->setSpacing(4);
    m_rowsScroll->setWidget(rowsHost);
    lay->addWidget(m_rowsScroll, 1);

    auto* bottom = new QHBoxLayout;
    bottom->addStretch(1);
    auto* btnClose = new QPushButton(QStringLiteral("关闭"), m_cfgDlg);
    connect(btnClose, &QPushButton::clicked, m_cfgDlg, &QDialog::hide);
    bottom->addWidget(btnClose);
    lay->addLayout(bottom);
}

void Oscilloscope::openConfig() {
    buildConfigDialog();
    m_cfgDlg->show();
    m_cfgDlg->raise();
    m_cfgDlg->activateWindow();
}

void Oscilloscope::updateCfgButton() {
    if (!m_btnCfg)
        return;
    int on = 0;
    for (const OscSeries& s : m_series) {
        if (s.enabled)
            ++on;
    }
    m_btnCfg->setText(QStringLiteral("提取表达式（%1/%2）").arg(on).arg(m_series.size()));
}

bool Oscilloscope::parseScale(const QString& text, double& out) {
    return parseScaleText(text, out);
}

void Oscilloscope::setThemeColors(const sjj::ThemeColors& t) {
    m_tc = t;
    m_chart->setThemeColors(t);
    for (int i = 0; i < m_rows.size() && i < m_series.size(); ++i) {
        if (!m_rows.at(i).scale)
            continue;
        double dummy = 0;
        const QColor textColor(parseScaleText(m_series.at(i).scaleText, dummy)
                                   ? m_tc.value(QStringLiteral("text_primary"))
                                   : QStringLiteral("#E5484D"));
        QPalette pal = m_rows.at(i).scale->palette();
        pal.setColor(QPalette::Text, textColor);
        m_rows.at(i).scale->setPalette(pal);
    }
    update();
}

void Oscilloscope::applyPattern(int idx, const QString& text) {
    if (idx < 0 || idx >= m_series.size())
        return;
    OscSeries& s = m_series[idx];
    s.pattern = text;
    OscByteSpec spec;
    if (parseByteSpec(text, spec)) {
        // 字节取值：不编译正则，按字节区间从原始帧里取（可带帧校验）
        s.byteMode = true;
        s.byte = spec;
        s.regex = QRegularExpression();
    } else {
        s.byteMode = false;
        s.regex = QRegularExpression(text);
    }
    m_chart->update();
}

void Oscilloscope::applyScale(int idx, const QString& text) {
    if (idx < 0 || idx >= m_series.size())
        return;
    double factor = 1.0;
    const bool ok = parseScaleText(text, factor);
    m_series[idx].scaleText = text;
    m_series[idx].scale = ok ? factor : 1.0;
    if (idx < m_rows.size() && m_rows.at(idx).scale) {
        QPalette pal = m_rows.at(idx).scale->palette();
        pal.setColor(QPalette::Text, QColor(ok ? m_tc.value(QStringLiteral("text_primary"))
                                               : QStringLiteral("#E5484D")));
        m_rows.at(idx).scale->setPalette(pal);
        m_rows.at(idx).scale->setToolTip(ok ? QStringLiteral("倍率：填 1 / 0.1 / *2 / /100")
                                            : QStringLiteral("倍率无效，按 1 处理"));
    }
    m_chart->update();
}

void Oscilloscope::rebuildRows() {
    if (!m_rowsLay)
        return;
    while (QLayoutItem* item = m_rowsLay->takeAt(0)) {
        if (QWidget* w = item->widget())
            w->deleteLater();
        delete item;
    }
    m_rows.clear();
    for (int i = 0; i < m_series.size(); ++i) {
        Row r;
        r.widget = new QWidget(this);
        auto* lay = new QHBoxLayout(r.widget);
        lay->setContentsMargins(0, 0, 0, 0);
        lay->setSpacing(4);

        r.check = new QCheckBox(r.widget);
        r.check->setChecked(m_series.at(i).enabled);
        r.check->setToolTip(QStringLiteral("勾选后参与提取和绘制"));
        connect(r.check, &QCheckBox::toggled, this, [this, i](bool v) {
            if (i >= 0 && i < m_series.size()) {
                m_series[i].enabled = v;
                updateCfgButton();
                m_chart->update();
            }
        });
        lay->addWidget(r.check);

        r.pattern = new QLineEdit(m_series.at(i).pattern, r.widget);
        r.pattern->setPlaceholderText(QStringLiteral("正则如 xpp=(\\d+)；字节如 b2:3"));
        r.pattern->setToolTip(QStringLiteral(
            "文本：正则，该分包第一处匹配的数字即为取值（每包每正则只取一个点）\n"
            "字节：b2:3 取第 2~3 字节（大端无符号），b2:3le 小端，i2:3 有符号，b5 单字节\n"
            "帧校验：b2:3#1=02,2=03 校帧头；另可 len=8 / len>=6 / sum=0 / xor=0 / crc16=0"));
        r.pattern->installEventFilter(this);   // 窗口未激活时首次单击即可编辑
        connect(r.pattern, &QLineEdit::textChanged, this,
                [this, i](const QString& t) { applyPattern(i, t); });
        lay->addWidget(r.pattern, 1);

        r.scale = new QLineEdit(m_series.at(i).scaleText, r.widget);
        r.scale->setFixedWidth(66);
        r.scale->setPlaceholderText(QStringLiteral("1 或 /100"));
        r.scale->setToolTip(QStringLiteral("显示倍率：1 / 0.1 / *2 / /100（只影响显示）"));
        r.scale->installEventFilter(this);     // 窗口未激活时首次单击即可编辑
        connect(r.scale, &QLineEdit::textChanged, this,
                [this, i](const QString& t) { applyScale(i, t); });
        lay->addWidget(r.scale);

        auto* btnDel = new QPushButton(QStringLiteral("\u2715"), r.widget);
        btnDel->setFixedWidth(28);
        btnDel->setToolTip(QStringLiteral("删除该表达式"));
        connect(btnDel, &QPushButton::clicked, this, [this, i] {
            if (i >= 0 && i < m_series.size()) {
                m_series.removeAt(i);
                rebuildRows();
                m_chart->update();
            }
        });
        lay->addWidget(btnDel);

        m_rowsLay->addWidget(r.widget);
        m_rows.append(r);
    }
    m_rowsLay->addStretch(1);   // 行少时贴顶，不上下分散
    setThemeColors(m_tc);
    updateCfgButton();
}

void Oscilloscope::addSeries() {
    m_series.append(OscSeries());
    rebuildRows();
    if (!m_rows.isEmpty() && m_rows.last().pattern) {
        m_rows.last().pattern->setFocus();
        // 行数超出可视高度时把新行滚进视野
        m_rowsLay->activate();
        m_rowsScroll->ensureWidgetVisible(m_rows.last().widget);
    }
    m_chart->update();
}

// 窗口在后台时，第一次点击只会激活窗口而进不到输入框；这里在按下鼠标时就
// 激活所在窗口并显式给输入框焦点，做到单击即可编辑（表达式配置在弹窗里）。
bool Oscilloscope::eventFilter(QObject* watched, QEvent* e) {
    if (e->type() == QEvent::MouseButtonPress) {
        if (auto* w = qobject_cast<QWidget*>(watched); w && qobject_cast<QLineEdit*>(w)) {
            QWidget* win = w->window();
            if (win && !win->isActiveWindow()) {
                win->activateWindow();
                win->raise();
            }
            w->setFocus(Qt::MouseFocusReason);
        }
    }

    // 无边框窗口边缘缩放（仅本窗口内的事件）
    const QEvent::Type t = e->type();
    if (t == QEvent::MouseMove || t == QEvent::MouseButtonPress
        || t == QEvent::MouseButtonRelease) {
        auto* src = qobject_cast<QWidget*>(watched);
        if (!src || src->window() != this)
            return QWidget::eventFilter(watched, e);
        auto* me = static_cast<QMouseEvent*>(e);
        const QPoint gpos = me->globalPosition().toPoint();
        const QPoint pos = mapFromGlobal(gpos);
        // 标题栏按钮贴着窗口右上角：放行，避免点关闭/最小化被当成缩放
        QWidget* child = childAt(pos);
        if (child && qobject_cast<QToolButton*>(child))
            return QWidget::eventFilter(watched, e);
        if (t == QEvent::MouseButtonRelease) {
            if (m_resizeEdge != 0) {
                m_resizeEdge = 0;
                m_resizeStartPos = QPoint();
                return true;
            }
            return QWidget::eventFilter(watched, e);
        }
        if (t == QEvent::MouseButtonPress) {
            if (me->button() == Qt::LeftButton) {
                const int edges = detectResizeEdge(pos);
                if (edges) {
                    m_resizeEdge = edges;
                    m_resizeStartPos = gpos;
                    m_resizeStartGeo = geometry();
                    return true;
                }
            }
            return QWidget::eventFilter(watched, e);
        }
        // MouseMove
        if (m_resizeEdge != 0) {
            applyResize(gpos);
            return true;
        }
        const int edges = detectResizeEdge(pos);
        const Qt::CursorShape cur = resizeCursor(edges);
        if (cur != Qt::ArrowCursor) {
            setCursor(cur);
            return true;   // 边缘区域不传给子控件
        }
        unsetCursor();
    }
    return QWidget::eventFilter(watched, e);
}

void Oscilloscope::showRegexHelp() {
    QDialog dlg(m_cfgDlg ? static_cast<QWidget*>(m_cfgDlg) : this);
    dlg.setWindowTitle(QStringLiteral("正则表达式帮助"));
    dlg.setMinimumWidth(600);
    auto* root = new QVBoxLayout(&dlg);
    root->setContentsMargins(14, 12, 14, 12);
    root->setSpacing(8);

    auto* tip = new QLabel(QStringLiteral(
        "文本：以接收区的一个分包为单位（不是文本里的一行），每个正则在该分包上运行一次、"
        "取第一个匹配；有捕获组时取第 1 组，没有捕获组时取整个匹配。\n"
        "字节：同样以接收区的一个分包为一帧，按「第几字节」取值（从 1 开始数、区间含首尾；"
        "第 1 字节即该分包收到的第一个字节，分隔符字节也在包内计入），"
        "b=无符号 i=有符号，默认大端，末尾加 le 表示小端，最多 4 字节；包长不够时跳过该包。\n"
        "帧校验：字节取值可在 # 后面追加条件，用逗号分隔——1=02 指定第 1 字节必须是 0x02，"
        "len=8 整包必须 8 字节，len>=6 至少 6 字节。\n"
        "  校验和类的 sum=0 / xor=0 / crc16=0，含义是「把这个包的所有字节（含帧尾的校验字节）"
        "按该算法算一遍，结果等于 0」，也就是协议里校验通过的帧，不是「校验字节等于 0」。\n"
        "  · sum=0：逐字节相加只保留低 8 位，如 01 02 03 FA（01+02+03=06，校验收尾 FA），整包相加=0x100→低 8 位为 0\n"
        "  · xor=0：逐字节异或，如 01 02 03 00（01^02^03=00），整包异或为 0\n"
        "  · crc16=0：两个字节的 Modbus CRC16（低字节在前），如 02 03 CB 01 67 6C，67 6C 就是 CRC\n"
        "  不满足条件的分包（例如混在协议里的日志）不会画点。\n"
        "倍率只影响显示，不修改已提取的原始数据。点右侧按钮即可复制示例。"), &dlg);
    tip->setWordWrap(true);
    root->addWidget(tip);

    struct Sample {
        const char* desc;
        const char* re;
    };
    static const Sample kSamples[] = {
        {"取 xpp= 后面的整数", "xpp=(\\d+)"},
        {"取可能有符号/小数的值", "xpp=(-?\\d+(?:\\.\\d+)?)"},
        {"取 xmax 值", "xmax=(\\d+)"},
        {"取帧率 fs", "fs=(\\d+)"},
        {"抓本行第一个小数", "-?\\d+\\.\\d+"},
        {"数字 + 单位（取数字）", "(\\d+)\\s*ms"},
        {"多个字段任取其一", "(?:th|temp)=(-?\\d+)"},
        {"十六进制数值", "0x([0-9A-Fa-f]+)"},
        {"字节：第 2~3 字节（大端无符号）", "b2:3"},
        {"字节：第 2~3 字节（小端无符号）", "b2:3le"},
        {"字节：第 2~3 字节（大端有符号）", "i2:3"},
        {"字节：第 2~3 字节（小端有符号）", "i2:3le"},
        {"字节：第 5 个字节", "b5"},
        {"字节 + 帧头校验（第 1=02、第 2=03）", "b2:3#1=02,2=03"},
        {"字节 + 长度校验（整包 8 字节）", "b2:3#len=8"},
        {"字节 + 累加和校验（整包低 8 位和为 0）", "b2:3#sum=0"},
        {"字节 + 异或校验（整包异或为 0）", "b2:3#xor=0"},
        {"字节 + Modbus CRC16（整包 CRC 为 0）", "b2:3#crc16=0"},
        {"字节 + 帧头/长度/累加和（日志混流）", "b2:3#1=02,2=03,len=8,sum=0"},
    };

    auto* grid = new QGridLayout;
    grid->setHorizontalSpacing(8);
    grid->setVerticalSpacing(5);
    int row = 0;
    for (const Sample& s : kSamples) {
        auto* desc = new QLabel(QString::fromUtf8(s.desc), &dlg);
        auto* re = new QLineEdit(QString::fromUtf8(s.re), &dlg);
        re->setReadOnly(true);
        re->setCursorPosition(0);
        QFont mono = re->font();
        mono.setFamily(QStringLiteral("Consolas"));
        re->setFont(mono);
        auto* btn = new QPushButton(QStringLiteral("复制"), &dlg);
        btn->setFixedWidth(60);
        const QString text = re->text();
        connect(btn, &QPushButton::clicked, &dlg, [btn, text] {
            QGuiApplication::clipboard()->setText(text);
            btn->setText(QStringLiteral("已复制"));
            QTimer::singleShot(1200, btn, [btn] { btn->setText(QStringLiteral("复制")); });
        });
        grid->addWidget(desc, row, 0);
        grid->addWidget(re, row, 1);
        grid->addWidget(btn, row, 2);
        ++row;
    }
    grid->setColumnStretch(1, 1);
    root->addLayout(grid);

    auto* bottom = new QHBoxLayout;
    bottom->addStretch(1);
    auto* btnClose = new QPushButton(QStringLiteral("关闭"), &dlg);
    btnClose->setDefault(true);
    connect(btnClose, &QPushButton::clicked, &dlg, &QDialog::accept);
    bottom->addWidget(btnClose);
    root->addLayout(bottom);

    btnClose->setFocus();
    dlg.exec();
}

void Oscilloscope::feed(const QString& text, const QByteArray& raw) {
    if (text.isEmpty() && raw.isEmpty())
        return;
    // 一次 feed = 接收区的一个分包（＝一条 rx 记录），不是文本里的一行：
    // 整包文本作为一次匹配的输入，每个正则只运行一次、取首个匹配；
    // 字节取值：同一个分包按字节区间取一个整数。
    QString line = text;
    while (line.endsWith(QLatin1Char('\n')) || line.endsWith(QLatin1Char('\r')))
        line.chop(1);
    bool changed = false;
    auto appendValue = [&](OscSeries& s, double v) {
        s.values.append(v);   // 只存原始值，倍率在绘制时换算
        if (s.values.size() > kMaxPoints)
            s.values.remove(0, s.values.size() - kMaxPoints);
        changed = true;
    };
    for (OscSeries& s : m_series) {
        if (!s.enabled || s.pattern.isEmpty())
            continue;
        if (s.byteMode) {
            // 先做帧校验（长度/帧头/校验和），不通过的分包（如日志行）直接丢弃
            if (raw.isEmpty() || !frameCheckPass(raw, s.byte.check))
                continue;
            double v = 0;
            if (!byteValueOf(raw, s.byte, v))
                continue;
            appendValue(s, v);
            continue;
        }
        if (!s.regex.isValid() || line.isEmpty())
            continue;
        const QRegularExpressionMatch m = s.regex.match(line);
        if (!m.hasMatch())
            continue;
        QString piece = m.lastCapturedIndex() >= 1 ? m.captured(1) : m.captured(0);
        if (piece.isEmpty())
            piece = m.captured(0);
        const QRegularExpressionMatch nm = numberRe().match(piece);
        if (!nm.hasMatch())
            continue;
        bool ok = false;
        const double v = nm.captured().toDouble(&ok);
        if (!ok || !std::isfinite(v))
            continue;
        appendValue(s, v);
    }
    if (changed)
        m_chart->update();
}

void Oscilloscope::clearData() {
    for (OscSeries& s : m_series)
        s.values.clear();
    m_chart->update();
}

QVector<double> Oscilloscope::samples(int seriesIndex) const {
    if (seriesIndex < 0 || seriesIndex >= m_series.size())
        return {};
    return m_series.at(seriesIndex).values;
}

QJsonArray Oscilloscope::toJson() const {
    QJsonArray arr;
    for (const OscSeries& s : m_series) {
        QJsonObject o;
        o[QStringLiteral("enabled")] = s.enabled;
        o[QStringLiteral("pattern")] = s.pattern;
        o[QStringLiteral("scale")] = s.scaleText;
        arr.append(o);
    }
    return arr;
}

void Oscilloscope::fromJson(const QJsonArray& arr) {
    m_series.clear();
    for (const QJsonValue& v : arr) {
        const QJsonObject o = v.toObject();
        OscSeries s;
        s.enabled = o.value(QStringLiteral("enabled")).toBool(true);
        s.pattern = o.value(QStringLiteral("pattern")).toString();
        s.scaleText = o.value(QStringLiteral("scale")).toString(QStringLiteral("1"));
        parseScaleText(s.scaleText, s.scale);
        OscByteSpec spec;
        if (parseByteSpec(s.pattern, spec)) {
            s.byteMode = true;
            s.byte = spec;
        } else {
            s.regex = QRegularExpression(s.pattern);
        }
        m_series.append(s);
    }
    if (m_series.isEmpty())
        m_series.append(OscSeries());
    rebuildRows();
    m_chart->update();
}

QJsonObject Oscilloscope::viewToJson() const {
    QJsonObject o;
    if (!m_chart)
        return o;
    const WaveView& v = m_chart->view();
    o[QStringLiteral("x_span")] = v.xSpan;
    o[QStringLiteral("x_right")] = v.xRight;
    o[QStringLiteral("y_auto")] = v.yAuto;
    o[QStringLiteral("y_min")] = v.yMin;
    o[QStringLiteral("y_max")] = v.yMax;
    return o;
}

void Oscilloscope::viewFromJson(const QJsonObject& o) {
    if (!m_chart)
        return;
    WaveView v;
    v.xSpan = o.value(QStringLiteral("x_span")).toInt(0);
    v.xRight = o.value(QStringLiteral("x_right")).toInt(-1);
    v.yAuto = o.value(QStringLiteral("y_auto")).toBool(true);
    v.yMin = o.value(QStringLiteral("y_min")).toDouble(0);
    v.yMax = o.value(QStringLiteral("y_max")).toDouble(1);
    m_chart->setView(v);
}
