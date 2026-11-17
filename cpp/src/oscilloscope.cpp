#include "oscilloscope.h"

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
#include <QLineEdit>
#include <QPainter>
#include <QPalette>
#include <QPen>
#include <QPushButton>
#include <QScrollArea>
#include <QStringList>
#include <QTimer>
#include <QVBoxLayout>

#include <algorithm>
#include <cmath>

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

    // 存储的是原始提取值，倍率只在绘制/图例时换算（改倍率不影响历史数据）
    double minY = 0, maxY = 0;
    int maxLen = 1;
    bool any = false;
    for (const OscSeries& s : *m_series) {
        if (!s.enabled || s.values.isEmpty())
            continue;
        for (const double v : s.values) {
            const double sv = v * s.scale;
            if (!any) {
                minY = maxY = sv;
                any = true;
            } else {
                minY = qMin(minY, sv);
                maxY = qMax(maxY, sv);
            }
        }
        maxLen = qMax(maxLen, s.values.size());
    }
    if (!any) {
        p.setPen(secondary);
        p.drawText(area, Qt::AlignCenter, QStringLiteral("等待数据…"));
        return;
    }
    if (qFuzzyCompare(minY, maxY)) {
        minY -= 1;
        maxY += 1;
    }
    const double padY = (maxY - minY) * 0.08;
    minY -= padY;
    maxY += padY;

    const auto toY = [&](double v) -> qreal {
        return area.bottom() - (v - minY) / (maxY - minY) * area.height();
    };
    const auto toX = [&](int i) -> qreal {
        return area.left() + (maxLen <= 1 ? 0.0 : double(i) / (maxLen - 1) * area.width());
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
    // X 轴横坐标是采样序号，范围 0..maxLen-1；段数随点数变化，点数少时刻度不会重复
    const int lastIdx = qMax(0, maxLen - 1);
    const int xSteps = qBound(1, lastIdx, 4);
    for (int i = 0; i <= xSteps; ++i) {
        const int x = area.left() + area.width() * i / xSteps;
        p.setPen(dash);
        p.drawLine(x, area.top(), x, area.bottom());
        if (lastIdx == 0 && i > 0)
            continue; // 只有一个数据点时右侧刻度不会再画一个 0
        p.setPen(secondary);
        p.drawText(QRect(x - 24, area.bottom() + 3, 48, 18), Qt::AlignCenter,
                   QString::number(qlonglong(lastIdx) * i / xSteps));
    }

    // 折线
    int ci = 0;
    for (const OscSeries& s : *m_series) {
        if (!s.enabled || s.values.isEmpty()) {
            ++ci;
            continue;
        }
        QPen line(seriesColors().at(ci % seriesColors().size()));
        line.setWidthF(1.6);
        line.setCapStyle(Qt::RoundCap);
        line.setJoinStyle(Qt::RoundJoin);
        p.setPen(line);
        for (int i = 1; i < s.values.size(); ++i)
            p.drawLine(QPointF(toX(i - 1), toY(s.values.at(i - 1) * s.scale)),
                       QPointF(toX(i), toY(s.values.at(i) * s.scale)));
        if (s.values.size() == 1)
            p.drawEllipse(QPointF(toX(0), toY(s.values.at(0) * s.scale)), 2.5, 2.5);
        ++ci;
    }

}

// ==================== Oscilloscope ====================

Oscilloscope::Oscilloscope(QWidget* parent) : QWidget(parent) {
    setWindowTitle(QStringLiteral("示波器"));
    resize(760, 520);
    setMinimumSize(520, 340);
    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(8, 8, 8, 8);
    root->setSpacing(6);

    auto* hint = new QLabel(QStringLiteral(
        "按正则从接收文本提取数字：一行取一个点，横轴为提取次数，纵轴为数值 × 倍率。"), this);
    hint->setWordWrap(true);
    root->addWidget(hint);

    m_chart = new WaveChart(this);
    m_chart->bindSeries(&m_series);
    root->addWidget(m_chart, 1);

    // 表达式配置收进弹窗：主界面只留一个入口，图表区域尽量大
    auto* bar = new QHBoxLayout;
    m_btnCfg = new QPushButton(this);
    m_btnCfg->setToolTip(QStringLiteral("打开提取表达式配置（关闭后收起，改动立即生效）"));
    connect(m_btnCfg, &QPushButton::clicked, this, &Oscilloscope::openConfig);
    bar->addWidget(m_btnCfg);
    bar->addStretch(1);
    auto* btnClear = new QPushButton(QStringLiteral("清空数据"), this);
    connect(btnClear, &QPushButton::clicked, this, &Oscilloscope::clearData);
    bar->addWidget(btnClear);
    root->addLayout(bar);

    buildConfigDialog();
    m_series.append(OscSeries());
    rebuildRows();
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
    head->addWidget(new QLabel(QStringLiteral("一行一条折线；正则取数字，倍率只影响显示"), m_cfgDlg));
    head->addStretch(1);
    auto* btnHelp = new QPushButton(QStringLiteral("正则帮助"), m_cfgDlg);
    btnHelp->setToolTip(QStringLiteral("正则写法说明与可直接复制的示例"));
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
    m_series[idx].pattern = text;
    m_series[idx].regex = QRegularExpression(text);
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
        r.pattern->setPlaceholderText(QStringLiteral("正则，如 xpp=(\\d+)"));
        r.pattern->setToolTip(QStringLiteral("正则表达式，第一处匹配的数字为该行取值"));
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
        "每行文本只取第一个匹配：有捕获组时取第 1 组，没有捕获组时取整个匹配。\n"
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

void Oscilloscope::feed(const QString& text) {
    if (text.isEmpty())
        return;
    // 按行处理：一行数据最多产生一个点（取该行第一个匹配），
    // 没有匹配到数字的行直接忽略。
    const QStringList lines = text.split(QLatin1Char('\n'), Qt::SkipEmptyParts);
    if (lines.isEmpty())
        return;
    bool changed = false;
    for (OscSeries& s : m_series) {
        if (!s.enabled || s.pattern.isEmpty() || !s.regex.isValid())
            continue;
        for (QString line : lines) {
            if (line.endsWith(QLatin1Char('\r')))
                line.chop(1);
            if (line.isEmpty())
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
            s.values.append(v);   // 只存原始值，倍率在绘制时换算
            if (s.values.size() > kMaxPoints)
                s.values.remove(0, s.values.size() - kMaxPoints);
            changed = true;
        }
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
        s.regex = QRegularExpression(s.pattern);
        m_series.append(s);
    }
    if (m_series.isEmpty())
        m_series.append(OscSeries());
    rebuildRows();
    m_chart->update();
}
