#include "serialtool.h"

#include <QApplication>
#include <QCoreApplication>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QVBoxLayout>
#include <QFormLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QToolButton>
#include <QTextEdit>
#include <QCheckBox>
#include <QRegularExpression>
#include <QOverload>
#include <QComboBox>
#include <QListWidget>
#include <QListWidgetItem>
#include <QTabWidget>
#include <QTabBar>
#include <QScrollArea>
#include <QFileDialog>
#include <QInputDialog>
#include <QMessageBox>
#include <QDialog>
#include <QFrame>
#include <QProgressBar>
#include <QToolButton>
#include <QShortcut>
#include <QScrollBar>
#include <QMenu>
#include <QTextCursor>
#include <QTextCharFormat>
#include <QTextDocument>
#include <QFont>
#include <QColor>
#include <QIcon>
#include <QPainter>
#include <QClipboard>
#include <QStyle>
#include <QStyleOption>
#include <QScreen>
#include <QTimer>
#include <QUrl>
#include <QDesktopServices>
#include <QMouseEvent>
#include <QEvent>
#include <QSerialPortInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>
#include <QFile>
#include <QFileInfo>
#include <QFileSystemWatcher>
#include <QDir>
#include <QDateTime>
#include <QProcess>
#include <QRegularExpression>
#include <QSizePolicy>
#include <QHash>
#include <algorithm>
#include <functional>

#ifdef Q_OS_WIN
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <dwmapi.h>
#endif

#include "listpopup.h"
#include "utils.h"
#include "updater.h"

namespace {
// 程序性滚动保护（构造函数置 true，析构恢复 false）
struct ScrollGuard {
    bool& flag;
    explicit ScrollGuard(bool& f) : flag(f) { flag = true; }
    ~ScrollGuard() { flag = false; }
};
} // namespace

// ================= 构造 =================

SerialTool::SerialTool(QWidget* parent) : QWidget(parent) {
    buildUi();
    applyTheme(m_theme);
    refreshPorts();
    autoloadConfig();
    // 监听配置文件（多实例快捷命令实时同步；autoload 之后添加，避免加载自身写入误触发）
    m_cfgWatcher = new QFileSystemWatcher(this);
    connect(m_cfgWatcher, &QFileSystemWatcher::fileChanged,
            this, &SerialTool::onConfigFileChanged);
    if (QFileInfo::exists(configPath()))
        m_cfgWatcher->addPath(configPath());
    m_cfgSaveTimer = new QTimer(this);
    m_cfgSaveTimer->setSingleShot(true);
    connect(m_cfgSaveTimer, &QTimer::timeout, this, [this] { saveParams(true); });
    // 启动延迟 2 秒自动检查更新（后台线程，不阻塞 UI；失败静默）
    QTimer::singleShot(2000, this, &SerialTool::checkUpdateAuto);

    // 无边框窗口边缘缩放（事件过滤器全局拦截）
    m_resizeMargin = 8;
    m_resizeEdge = 0;
    installResizeFilter();
    qApp->installEventFilter(this);   // 全局拦截：搜索历史弹窗外部点击

    m_flushTimer = new QTimer(this);
    connect(m_flushTimer, &QTimer::timeout, this, &SerialTool::drainRx);
    m_flushTimer->start(50);

    // 分包：空闲超时 / 指定分隔符，两种可同时启用，任一条件满足即切出一条
    m_pktTimer = new QTimer(this);
    m_pktTimer->setSingleShot(true);
    connect(m_pktTimer, &QTimer::timeout, this, &SerialTool::flushRxPacket);

    // 串口热插拔自动检测：每 2 秒轮询端口列表，变化时才刷新 UI
    m_portPollTimer = new QTimer(this);
    m_portPollTimer->setInterval(2000);
    connect(m_portPollTimer, &QTimer::timeout, this, [this] {
        // 收集当前可用端口设备名集合
        QStringList curDevs;
        for (const auto& info : QSerialPortInfo::availablePorts())
            curDevs << info.portName();
        curDevs.sort();
        // 与上次记录的集合比较
        if (curDevs != m_lastPortDevs) {
            m_lastPortDevs = curDevs;
            refreshPorts();
        }
    });
    // 初始化记录（构造时 refreshPorts 已跑过一次）
    for (const auto& info : QSerialPortInfo::availablePorts())
        m_lastPortDevs << info.portName();
    m_lastPortDevs.sort();
    m_portPollTimer->start();
}

// ================= 事件（缩放 / DWM / 背景 / 关闭） =================

void SerialTool::installResizeFilter() {
    m_filterInstalled.insert(this);
    setMouseTracking(true);
    installEventFilter(this);
    const auto children = findChildren<QWidget*>();
    for (auto* child : children) {
        m_filterInstalled.insert(child);
        child->setMouseTracking(true);
        child->installEventFilter(this);
    }
}

int SerialTool::detectResizeEdge(const QPoint& pos) const {
    if (isMaximized())
        return 0;
    const int m = m_resizeMargin;
    const int w = width(), h = height();
    int edges = 0;
    if (pos.x() <= m)
        edges |= EdgeLeft;
    if (pos.x() >= w - m)
        edges |= EdgeRight;
    if (pos.y() <= m)
        edges |= EdgeTop;
    if (pos.y() >= h - m)
        edges |= EdgeBottom;
    return edges;
}

Qt::CursorShape SerialTool::resizeCursor(int edges) const {
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

void SerialTool::applyResize(const QPoint& gpos) {
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

bool SerialTool::eventFilter(QObject* obj, QEvent* event) {
    // 动态补装：配置加载后创建的控件
    if (auto* w = qobject_cast<QWidget*>(obj); w && w != this
        && !m_filterInstalled.contains(w)) {
        m_filterInstalled.insert(w);
        w->setMouseTracking(true);
        w->installEventFilter(this);
    }
    const QEvent::Type t = event->type();
    // 搜索历史弹窗：点击弹窗外部任意位置（含搜索输入框）时自动隐藏
    // 仅排除左侧箭头区域（由 dropAct::triggered 处理 toggle，避免双击 Bug）
    if (m_historyPopup && m_historyPopup->isVisible() && obj != m_historyPopup
        && (t == QEvent::MouseButtonPress || t == QEvent::MouseButtonDblClick)) {
        if (auto* me = static_cast<QMouseEvent*>(event)) {
            const QPoint gpos = me->globalPosition().toPoint();
            if (!m_historyPopup->geometry().contains(gpos)) {
                // 检查是否点击在搜索框左侧箭头区域（LeadingPosition 图标约 20px 宽）
                QRect entryRect = m_entrySearch->rect();
                entryRect.moveTo(m_entrySearch->mapToGlobal(QPoint(0, 0)));
                QRect arrowRect = entryRect;
                arrowRect.setWidth(20);   // 箭头图标区域
                // 点击不在箭头区域 -> 立即隐藏（搜索输入框、其他区域都隐藏）
                if (!arrowRect.contains(gpos))
                    m_historyPopup->hide();
            }
        }
    }
    if (obj == m_recvBox && t == QEvent::Resize) {
        positionScrollBottomBtn();
        return QWidget::eventFilter(obj, event);
    }
    // 选项卡列表浮层：点击浮层外部（含再次点击下拉按钮）立即收起。
    // 自己处理而不依赖 Qt，是因为该浮层是普通 QWidget(Qt::Popup)，
    // 未被 QApplicationPrivate 的弹出列表接管，点击外部不会自动关闭。
    if (m_tabPopup && m_tabPopup->isVisible()
        && (t == QEvent::MouseButtonPress || t == QEvent::MouseButtonDblClick)) {
        if (auto* me = static_cast<QMouseEvent*>(event)) {
            const QPoint gpos = me->globalPosition().toPoint();
            if (!m_tabPopup->geometry().contains(gpos))
                hideTabPopup();
        }
    }
    // 弹出层打开期间不参与窗口边缘缩放判定：否则落在窗口边缘的点击会被缩放逻辑
    // 吞掉，导致「再点一次按钮菜单不收起」，且按钮收不到 Release 卡在按下态。
    if (QApplication::activePopupWidget())
        return false;
    // 角落下拉按钮本身就贴着窗口右边缘，必须放行它的点击/悬停
    if (auto* w = qobject_cast<QWidget*>(obj);
        w && m_tabDropdownBtn
        && (w == m_tabDropdownBtn || m_tabDropdownBtn->isAncestorOf(w)))
        return false;

    if (t == QEvent::MouseMove || t == QEvent::MouseButtonPress
        || t == QEvent::MouseButtonRelease) {
        auto* me = static_cast<QMouseEvent*>(event);
        const QPoint gpos = me->globalPosition().toPoint();
        const QPoint pos = mapFromGlobal(gpos);
        if (t == QEvent::MouseButtonRelease) {
            if (m_resizeEdge != 0) {
                m_resizeEdge = 0;
                m_resizeStartPos = QPoint();
                return true;
            }
            return false;
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
            return false;
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
    return QWidget::eventFilter(obj, event);
}

void SerialTool::enableDwmEffects() {
#ifdef Q_OS_WIN
    // 1) 系统阴影：扩展 DWM frame 到客户区（经典 Win10 做法）
    const HWND hwnd = HWND(winId());
    MARGINS margins = {-1, -1, -1, -1};
    DwmExtendFrameIntoClientArea(hwnd, &margins);
    // 2) Win11 22H2+：系统圆角（DWMWA_WINDOW_CORNER_PREFERENCE = 33, DWMWCP_ROUND = 2）
    const int corner = 2;
    DwmSetWindowAttribute(hwnd, 33, &corner, sizeof(corner));
#endif
}

void SerialTool::showEvent(QShowEvent* e) {
    QWidget::showEvent(e);
    // 窗口首次显示后启用 DWM 系统阴影 + 圆角
    enableDwmEffects();
}

void SerialTool::paintEvent(QPaintEvent*) {
    // 画窗口不透明背景（主题色 win_bg），系统圆角负责裁剪圆角外的部分
    QPainter p(this);
    p.fillRect(rect(), QColor(m_winBg));
}

void SerialTool::closeEvent(QCloseEvent* e) {
    saveParams(true);
    closePort();
    if (m_settingsWin) {
        m_settingsWin->close();
        m_settingsWin->deleteLater();
        m_settingsWin = nullptr;
    }
    if (m_downloadThread && m_downloadThread->isRunning()) {
        m_downloadThread->cancel();
        m_downloadThread->wait(3000);
    }
    if (m_updateChecker) {
        m_updateChecker->deleteLater();
        m_updateChecker = nullptr;
    }
    e->accept();
}

// ================= UI 构建 =================

void SerialTool::buildUi() {
    // 无边框 + 透明背景：阴影和圆角由 Windows DWM 系统绘制
    setWindowFlags(Qt::FramelessWindowHint | Qt::Window);
    setAttribute(Qt::WA_TranslucentBackground, true);
    QString ver = QString::fromUtf8(APP_VERSION);
    while (ver.startsWith(QLatin1Char('v')) || ver.startsWith(QLatin1Char('V')))
        ver.remove(0, 1);
    setWindowTitle(QStringLiteral("%1  v%2").arg(sjj::APP_TITLE, ver));
    // 默认宽度取到"校验组控件（第X字节…加校验）右侧"，不留多余弹性空白
    resize(690, 500);
    setMinimumSize(640, 500);

    auto* outer = new QVBoxLayout(this);
    outer->setContentsMargins(0, 0, 0, 0);
    outer->setSpacing(0);

    // 自绘标题栏
    m_titleBar = new TitleBar(this);
    outer->addWidget(m_titleBar);
    connect(m_titleBar, &TitleBar::versionClicked, this, &SerialTool::checkUpdateManual);
    connect(m_titleBar, &TitleBar::themeClicked, this, &SerialTool::toggleTheme);

    // 内容容器
    auto* content = new QWidget(this);
    content->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
    auto* main = new QVBoxLayout(content);
    main->setContentsMargins(6, 4, 6, 4);
    main->setSpacing(4);
    outer->addWidget(content, 1);

    // ===== 上半部分：接收区 + 命令面板 =====
    auto* upper = new QWidget(content);
    auto* upperLay = new QVBoxLayout(upper);
    upperLay->setContentsMargins(0, 0, 0, 0);
    upperLay->setSpacing(4);

    auto* upperRow = new QHBoxLayout;
    upperRow->setSpacing(4);
    upperLay->addLayout(upperRow, 1);

    // 左侧：接收区
    auto* recvBox = new QGroupBox(QString(), upper);
    m_recvBox = recvBox;
    auto* recvLay = new QGridLayout(recvBox);
    recvLay->setContentsMargins(8, 6, 8, 6);
    recvLay->setSpacing(2);
    m_txtRecv = new QTextEdit(recvBox);
    m_txtRecv->setReadOnly(true);
    QFont mono(QStringLiteral("Consolas"), 10);
    m_txtRecv->setFont(mono);
    m_txtRecv->setLineWrapMode(QTextEdit::WidgetWidth);
    // 接收区右键菜单：清除窗口 / HEX显示 / 时间戳 / 暂停刷新 / 编码修改 / 查找
    m_txtRecv->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(m_txtRecv, &QWidget::customContextMenuRequested,
            this, &SerialTool::onRecvContextMenu);
    recvLay->addWidget(m_txtRecv, 0, 0);
    upperRow->addWidget(recvBox, 1);

    // 接收区右下角悬浮按钮：滚动离开底部（自动暂停刷新）时出现，点击一键回到底部并恢复刷新
    m_btnScrollBottom = new QPushButton(QStringLiteral("↓"), recvBox);
    m_btnScrollBottom->setToolTip(QStringLiteral("滚动到底部并恢复刷新"));
    m_btnScrollBottom->setFixedSize(30, 24);
    m_btnScrollBottom->hide();
    connect(m_btnScrollBottom, &QPushButton::clicked, this, &SerialTool::scrollRecvToBottom);

    // 接收区右上角悬浮搜索条（默认隐藏；焦点在接收区时 Ctrl+F 唤出）
    m_searchBar = new QWidget(recvBox);
    m_searchBar->setObjectName(QStringLiteral("searchBar"));
    m_searchBar->setAttribute(Qt::WA_StyledBackground, true);
    auto* sbLay = new QHBoxLayout(m_searchBar);
    sbLay->setContentsMargins(8, 4, 8, 4);
    sbLay->setSpacing(4);
    m_entrySearch = new QLineEdit(m_searchBar);
    m_entrySearch->setPlaceholderText(QStringLiteral("查找..."));
    m_entrySearch->setFixedWidth(160);
    // 搜索框左侧：下拉箭头（LeadingPosition）— 点击弹出/收起搜索历史候选列表
    auto* dropAct = m_entrySearch->addAction(
        m_entrySearch->style()->standardIcon(QStyle::SP_ArrowDown),
        QLineEdit::LeadingPosition);
    dropAct->setToolTip(QStringLiteral("搜索历史"));
    connect(dropAct, &QAction::triggered, this, [this] {
        // toggle：可见则隐藏，不可见则展开（搜索框区域的 press 不由
        // eventFilter 隐藏，故 release 的 triggered 状态可靠，无双击 Bug）
        if (m_historyPopup && m_historyPopup->isVisible())
            m_historyPopup->hide();
        else
            showSearchHistoryPopup();
    });
    // 输入框右侧 trailing ✕ 按钮（仅在有内容时显示）
    auto* clearAct = m_entrySearch->addAction(QIcon(), QLineEdit::TrailingPosition);
    clearAct->setVisible(false);
    clearAct->setToolTip(QStringLiteral("清空"));
    connect(m_entrySearch, &QLineEdit::textChanged, this, [this, clearAct](const QString& t) {
        clearAct->setVisible(!t.isEmpty());
        // 弹窗展开时，随输入实时过滤历史
        if (m_historyPopup && m_historyPopup->isVisible())
            refreshHistoryPopup();
        onSearchChange();
    });
    connect(clearAct, &QAction::triggered, this, [this] {
        m_entrySearch->clear();
        m_entrySearch->setFocus();
    });
    connect(m_entrySearch, &QLineEdit::returnPressed, this, &SerialTool::commitSearchHistory);
    // 失焦时：匹配成功则记录历史
    connect(m_entrySearch, &QLineEdit::editingFinished, this, &SerialTool::onSearchFocusOut);
    // 搜索历史下拉：自定义顶层列表（不抢鼠标，点击外部由全局 eventFilter 隐藏）
    m_historyPopup = new QListWidget(this);
    m_historyPopup->setWindowFlag(Qt::ToolTip);   // 顶层无边框、不抢鼠标焦点
    m_historyPopup->setWindowFlag(Qt::FramelessWindowHint, false);
    m_historyPopup->setWindowFlags(Qt::ToolTip | Qt::FramelessWindowHint);
    m_historyPopup->setFocusPolicy(Qt::NoFocus);
    m_historyPopup->setObjectName(QStringLiteral("searchHistoryPopup"));
    m_historyPopup->setStyleSheet(QStringLiteral(
        "QListWidget#searchHistoryPopup{padding:4px;border-radius:4px;outline:0;}"
        "QListWidget#searchHistoryPopup::item{padding:4px 8px;}"
        "QListWidget#searchHistoryPopup::item:selected{background:#0078d4;color:#fff;}"));
    m_historyPopup->hide();
    connect(m_historyPopup, &QListWidget::itemClicked, this, [this](QListWidgetItem* it) {
        if (!it)
            return;
        const QString sel = it->text();
        // 选中某条历史 -> 刷新其时间戳（移到最前）
        const qint64 now = QDateTime::currentMSecsSinceEpoch();
        for (int i = m_searchHistory.size() - 1; i >= 0; --i) {
            if (m_searchHistory[i].text == sel) {
                m_searchHistory[i].timestamp = now;
                // 移到最前（时间倒序）
                SearchHistoryEntry e = m_searchHistory.takeAt(i);
                m_searchHistory.prepend(e);
                break;
            }
        }
        m_entrySearch->setText(sel);
        m_historyPopup->hide();
        m_entrySearch->setFocus();
        onSearchChange();
        saveParams(true);
    });
    sbLay->addWidget(m_entrySearch);
    m_lblSearchCount = new QLabel(QStringLiteral("0/0"), m_searchBar);
    m_lblSearchCount->setAlignment(Qt::AlignCenter);
    m_lblSearchCount->setMinimumWidth(36);
    sbLay->addWidget(m_lblSearchCount);
    m_btnSearchPrev = new QToolButton(m_searchBar);
    m_btnSearchPrev->setText(QStringLiteral("▲"));
    m_btnSearchPrev->setObjectName(QStringLiteral("searchNav"));
    m_btnSearchPrev->setToolTip(QStringLiteral("上一个匹配 (Shift+Enter)"));
    m_btnSearchPrev->setFixedSize(22, 22);
    m_btnSearchPrev->setCursor(Qt::PointingHandCursor);
    connect(m_btnSearchPrev, &QToolButton::clicked, this, [this] { searchJump(-1); });
    sbLay->addWidget(m_btnSearchPrev);
    m_btnSearchNext = new QToolButton(m_searchBar);
    m_btnSearchNext->setText(QStringLiteral("▼"));
    m_btnSearchNext->setObjectName(QStringLiteral("searchNav"));
    m_btnSearchNext->setToolTip(QStringLiteral("下一个匹配 (Enter)"));
    m_btnSearchNext->setFixedSize(22, 22);
    m_btnSearchNext->setCursor(Qt::PointingHandCursor);
    connect(m_btnSearchNext, &QToolButton::clicked, this, [this] { searchJump(1); });
    sbLay->addWidget(m_btnSearchNext);

    // —— VS Code 风格图标化开关：Cc (case) / W (whole word) / .* (regex) ——
    auto makeOptBtn = [this, sbLay](const QString& text, const QString& tip) {
        auto* b = new QToolButton(m_searchBar);
        b->setText(text);
        b->setCheckable(true);
        b->setToolTip(tip);
        b->setFixedSize(26, 22);
        b->setCursor(Qt::PointingHandCursor);
        b->setObjectName(QStringLiteral("searchOpt"));
        sbLay->addWidget(b);
        return b;
    };
    m_btnCase = makeOptBtn(QStringLiteral("Cc"), QStringLiteral("区分大小写"));
    connect(m_btnCase, &QToolButton::toggled, this, &SerialTool::onSearchOptionChanged);
    m_btnRegex = makeOptBtn(QStringLiteral(".*"), QStringLiteral("正则表达式"));
    connect(m_btnRegex, &QToolButton::toggled, this, &SerialTool::onSearchOptionChanged);
    // 筛选：图标按钮（漏斗）— 不常点，所以也用 QToolButton
    m_btnFilter = new QToolButton(m_searchBar);
    m_btnFilter->setText(QStringLiteral("筛选"));
    m_btnFilter->setCheckable(true);
    m_btnFilter->setToolTip(QStringLiteral("只显示匹配行"));
    m_btnFilter->setFixedSize(40, 22);
    m_btnFilter->setCursor(Qt::PointingHandCursor);
    m_btnFilter->setObjectName(QStringLiteral("searchOpt"));
    connect(m_btnFilter, &QToolButton::toggled, this, &SerialTool::refreshView);
    sbLay->addWidget(m_btnFilter);
    // 关闭搜索条（×）
    m_btnSearchClose = new QToolButton(m_searchBar);
    m_btnSearchClose->setText(QStringLiteral("✕"));
    m_btnSearchClose->setObjectName(QStringLiteral("searchClose"));
    m_btnSearchClose->setToolTip(QStringLiteral("关闭搜索 (Esc)"));
    m_btnSearchClose->setFixedSize(22, 22);
    m_btnSearchClose->setCursor(Qt::PointingHandCursor);
    connect(m_btnSearchClose, &QToolButton::clicked, this, &SerialTool::hideSearchBar);
    sbLay->addWidget(m_btnSearchClose);
    // 悬浮于接收区右上角；后 add 的在 QGridLayout 同格中位于上层。
    // 用一层宿主 widget 加右边距（= 滚动条宽度 + 4），把搜索条左移，避免遮挡右侧滚动条
    auto* sbHost = new QWidget(recvBox);
    sbHost->setObjectName(QStringLiteral("sbHost"));
    sbHost->setStyleSheet(QStringLiteral("#sbHost{background:transparent;border:none;}"));
    auto* sbHostLay = new QHBoxLayout(sbHost);
    const int sbExtent = m_txtRecv->style()->pixelMetric(QStyle::PM_ScrollBarExtent,
                                                        nullptr, m_txtRecv);
    sbHostLay->setContentsMargins(0, 0, sbExtent + 4, 0);
    sbHostLay->addWidget(m_searchBar);
    recvLay->addWidget(sbHost, 0, 0, Qt::AlignTop | Qt::AlignRight);
    m_searchBar->hide();
    // Ctrl+F：焦点在接收区时唤出搜索条（再按一次关闭）
    auto* scFind = new QShortcut(QKeySequence::Find, m_txtRecv);
    scFind->setContext(Qt::WidgetShortcut);
    connect(scFind, &QShortcut::activated, this, &SerialTool::toggleSearchBar);
    // 焦点在搜索条内时 Ctrl+F 同样可关闭
    auto* scFindBar = new QShortcut(QKeySequence::Find, m_searchBar);
    scFindBar->setContext(Qt::WidgetShortcut);
    connect(scFindBar, &QShortcut::activated, this, &SerialTool::toggleSearchBar);
    // Esc 关闭搜索条（WidgetWithChildrenShortcut：焦点在搜索条本身或任一子控件
    // —— 含输入框 m_entrySearch、上一/下一按钮、关闭按钮 —— 均生效）
    auto* scEsc = new QShortcut(QKeySequence(Qt::Key_Escape), m_searchBar);
    scEsc->setContext(Qt::WidgetWithChildrenShortcut);
    connect(scEsc, &QShortcut::activated, this, &SerialTool::hideSearchBar);
    auto* scEscRecv = new QShortcut(QKeySequence(Qt::Key_Escape), m_txtRecv);
    scEscRecv->setContext(Qt::WidgetShortcut);
    connect(scEscRecv, &QShortcut::activated, this, &SerialTool::hideSearchBar);

    // 右侧：历史/快捷命令面板（默认隐藏，固定宽 420）
    m_msPanel = new QWidget(upper);
    m_msPanel->setFixedWidth(420);
    auto* msPanelLay = new QVBoxLayout(m_msPanel);
    msPanelLay->setContentsMargins(0, 0, 0, 0);
    msPanelLay->setSpacing(4);

    // 顶部工具栏：循环发送控件 + 新建分组（作用于当前选中的分组选项卡）
    auto* msTop = new QWidget(m_msPanel);
    auto* msTopLay = new QHBoxLayout(msTop);
    msTopLay->setContentsMargins(4, 4, 4, 4);
    msTopLay->setSpacing(4);
    m_chkMsLoop = new ThemeCheckBox(QStringLiteral("循环发送"), msTop);
    msTopLay->addWidget(m_chkMsLoop);
    msTopLay->addWidget(new QLabel(QStringLiteral("间隔ms:"), msTop));
    m_entryMsInterval = new QLineEdit(QStringLiteral("1000"), msTop);
    m_entryMsInterval->setFixedWidth(56);
    msTopLay->addWidget(m_entryMsInterval);
    m_btnStartMs = new QPushButton(QStringLiteral("▶ 开始"), msTop);
    connect(m_btnStartMs, &QPushButton::clicked, this, &SerialTool::startMsLoop);
    msTopLay->addWidget(m_btnStartMs);
    m_btnStopMs = new QPushButton(QStringLiteral("■ 停止"), msTop);
    m_btnStopMs->setEnabled(false);
    connect(m_btnStopMs, &QPushButton::clicked, this, &SerialTool::stopMsLoop);
    msTopLay->addWidget(m_btnStopMs);
    msTopLay->addStretch(1);
    auto* btnNewGrp = new QPushButton(QStringLiteral("新建分组"), msTop);
    connect(btnNewGrp, &QPushButton::clicked, this, &SerialTool::newMsGroup);
    msTopLay->addWidget(btnNewGrp);
    msPanelLay->addWidget(msTop);

    // 选项卡：每页=一个快捷命令分组（横向排列），支持手动拖动排序
    m_msTabs = new QTabWidget(m_msPanel);
    m_msTabs->setUsesScrollButtons(false);  // 禁用 Qt 自带滚动箭头，改用右侧下拉箭头
    m_msTabs->tabBar()->setMovable(true);   // 快捷命令分组支持拖动排序
    m_msTabs->tabBar()->setToolTip(QStringLiteral("按住标签左右拖动可调整分组顺序"));
    // 分组选项卡由 refreshMsRows() 按 m_msGroups 构建（buildUi 末尾 loadParams 时调用）
    msPanelLay->addWidget(m_msTabs, 1);
    m_msPanel->hide();
    upperRow->addWidget(m_msPanel);
    // 切换选项卡即切换当前活动分组（tab 下标 = 分组下标）；并刷新活动组高亮
    connect(m_msTabs, &QTabWidget::currentChanged, this, [this](int idx) {
        if (idx >= 0 && idx < m_msGroups.size()) {
            m_activeMsGroup = idx;
            applyMsActiveStyle();
        }
    });
    // 拖动排序完成：同步 m_msGroups 顺序（选项卡顺序即分组顺序），并持久化
    connect(m_msTabs->tabBar(), &QTabBar::tabMoved, this, [this](int from, int to) {
        if (from < 0 || from >= m_msGroups.size() || to < 0
                || to >= m_msGroups.size() || from == to)
            return;
        m_msGroups.insert(to, m_msGroups.takeAt(from));
        // 活动分组跟着被移动/平移，保持指向同一个分组
        if (m_activeMsGroup == from)
            m_activeMsGroup = to;
        else if (from < m_activeMsGroup && to >= m_activeMsGroup)
            --m_activeMsGroup;
        else if (from > m_activeMsGroup && to <= m_activeMsGroup)
            ++m_activeMsGroup;
        applyMsActiveStyle();
        scheduleQuickCmdSave();   // 保存新顺序（多实例经配置文件同步）
    });
    // 右上角下拉箭头：点击弹出分组列表浮层（TabListPopup），浮层显示在按钮左侧
    m_tabDropdownBtn = new QToolButton(m_msTabs);
    m_tabDropdownBtn->setText(QStringLiteral("\u25BE"));  // ▾
    m_tabDropdownBtn->setToolTip(QStringLiteral("显示/收起选项卡列表"));
    m_tabDropdownBtn->setObjectName(QStringLiteral("tabDropdown"));
    m_tabDropdownBtn->setCheckable(true);                 // 展开期间保持高亮，收起后自动复位
    m_tabDropdownBtn->setAutoRaise(true);
    m_tabDropdownBtn->setFixedSize(22, 22);
    m_tabDropdownBtn->setCursor(Qt::PointingHandCursor);
    // 用容器加右边距：面板布局本身无内边距，按钮直接贴边会被面板边框裁掉一截
    auto* tabCorner = new QWidget(m_msTabs);
    auto* tabCornerLay = new QHBoxLayout(tabCorner);
    tabCornerLay->setContentsMargins(0, 0, 6, 0);
    tabCornerLay->setSpacing(0);
    tabCornerLay->addWidget(m_tabDropdownBtn);
    m_msTabs->setCornerWidget(tabCorner, Qt::TopRightCorner);
    connect(m_tabDropdownBtn, &QToolButton::clicked, this, &SerialTool::showTabDropdown);

    // 接收区按钮行
    auto* rt = new QHBoxLayout;
    rt->setSpacing(4);
    auto* btn = new QPushButton(QStringLiteral("清除窗口"), upper);
    btn->setObjectName(QStringLiteral("btnClearRecv"));
    connect(btn, &QPushButton::clicked, this, &SerialTool::clearRecv);
    rt->addWidget(btn);
    m_chkShowHex = new ThemeCheckBox(QStringLiteral("HEX显示"), upper);
    rt->addWidget(m_chkShowHex);
    connect(m_chkShowHex, &QCheckBox::stateChanged, this, &SerialTool::refreshView);
    m_chkShowTs = new ThemeCheckBox(QStringLiteral("时间戳"), upper);
    m_chkShowTs->setToolTip(QStringLiteral("显示时间戳和分包（多行数据每行加前缀）"));
    m_chkShowTs->setChecked(true);
    rt->addWidget(m_chkShowTs);
    connect(m_chkShowTs, &QCheckBox::stateChanged, this, &SerialTool::refreshView);
    m_chkPause = new ThemeCheckBox(QStringLiteral("暂停刷新"), upper);
    rt->addWidget(m_chkPause);
    // 暂停 = 不自动滚动：数据照常渲染，滚到底由 onRecvScroll 自动解除暂停
    rt->addWidget(new QLabel(QStringLiteral("编码:"), upper));
    m_cmbEncoding = new StyledComboBox(upper);
    m_cmbEncoding->addItems(sjj::ENCODINGS);
    m_cmbEncoding->setEditable(false);
    m_cmbEncoding->setFixedWidth(90);
    connect(m_cmbEncoding, &QComboBox::currentTextChanged, this, &SerialTool::refreshView);
    rt->addWidget(m_cmbEncoding);
    btn = new QPushButton(QStringLiteral("保存数据"), upper);
    connect(btn, &QPushButton::clicked, this, &SerialTool::saveRecv);
    rt->addWidget(btn);
    btn = new QPushButton(QStringLiteral("快捷命令"), upper);
    btn->setObjectName(QStringLiteral("btnQuickCmd"));
    connect(btn, &QPushButton::clicked, this, [this] { toggleWindow(0); });
    rt->addWidget(btn);
    rt->addStretch(1);
    upperLay->addLayout(rt);

    main->addWidget(upper, 1);

    // 滚动跟随：滚轮上翻/拖动滚动条上移 → 自动勾选暂停刷新；滚回底部自动取消
    connect(m_txtRecv->verticalScrollBar(), &QScrollBar::valueChanged,
            this, &SerialTool::onRecvScroll);

    // ===== 下半部分：串口设置块（左） + 发送区（右） =====
    auto* lower = new QHBoxLayout;
    lower->setSpacing(6);
    main->addLayout(lower);

    // 左侧：串口设置块
    auto* sbBox = new QGroupBox(QString(), content);
    sbBox->setMaximumWidth(230);
    auto* sb = new QVBoxLayout(sbBox);
    sb->setContentsMargins(6, 1, 6, 1);
    sb->setSpacing(1);

    // 第 1 行：端口号（刷新按钮已移至波特率行，区域缩窄）
    auto* portRow = new QHBoxLayout;
    portRow->setSpacing(2);
    portRow->addWidget(new QLabel(QStringLiteral("端口号:"), sbBox));
    m_cmbPort = new PortComboBox(sbBox);
    m_cmbPort->setEditable(false);
    m_cmbPort->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    portRow->addWidget(m_cmbPort, 1);
    sb->addLayout(portRow);

    // 第 2 行：波特率 + 刷新按钮
    auto* baudRow = new QHBoxLayout;
    baudRow->setSpacing(2);
    auto* lblBaud = new QLabel(QStringLiteral("波特率:"), sbBox);
    lblBaud->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
    baudRow->addWidget(lblBaud);
    m_cmbBaud = new StyledComboBox(sbBox);
    m_cmbBaud->addItems(sjj::DEFAULT_BAUDRATES);
    m_cmbBaud->setCurrentText(QStringLiteral("115200"));
    m_cmbBaud->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    m_cmbBaud->setMinimumWidth(60);
    baudRow->addWidget(m_cmbBaud, 1);
    connect(m_cmbPort, &QComboBox::currentIndexChanged, this, &SerialTool::onPortChanged);
    connect(m_cmbBaud, &QComboBox::currentIndexChanged, this, &SerialTool::onBaudChanged);
    btn = new QPushButton(QStringLiteral("刷新"), sbBox);
    btn->setFixedWidth(40);
    connect(btn, &QPushButton::clicked, this, &SerialTool::refreshPorts);
    baudRow->addWidget(btn);
    sb->addLayout(baudRow);

    // 第 3 行：更多设置 + 打开串口
    auto* optRow = new QHBoxLayout;
    optRow->setSpacing(2);
    btn = new QPushButton(QStringLiteral("更多设置"), sbBox);
    connect(btn, &QPushButton::clicked, this, &SerialTool::showSettingsDialog);
    optRow->addWidget(btn, 1);
    m_btnOpen = new QPushButton(QStringLiteral("打开串口"), sbBox);
    m_btnOpen->setObjectName(QStringLiteral("btnConnect"));
    connect(m_btnOpen, &QPushButton::clicked, this, &SerialTool::togglePort);
    optRow->addWidget(m_btnOpen, 1);
    sb->addLayout(optRow);

    // 第 4 行：选择文件 + 发送文件（从发送区移入）
    auto* fileRow = new QHBoxLayout;
    fileRow->setSpacing(2);
    m_btnSelectFile = new QPushButton(QStringLiteral("选择文件"), sbBox);
    m_btnSelectFile->setToolTip(QStringLiteral("点击选择要发送的文件"));
    connect(m_btnSelectFile, &QPushButton::clicked, this, &SerialTool::pickFile);
    fileRow->addWidget(m_btnSelectFile, 1);
    m_btnSendFile = new QPushButton(QStringLiteral("发送文件"), sbBox);
    m_btnSendFile->setObjectName(QStringLiteral("btnSendFile"));
    connect(m_btnSendFile, &QPushButton::clicked, this, &SerialTool::sendFile);
    fileRow->addWidget(m_btnSendFile, 1);
    sb->addLayout(fileRow);
    lower->addWidget(sbBox);

    // 右侧：发送区
    auto* sendBox = new QGroupBox(QString(), content);
    auto* sendLay = new QVBoxLayout(sendBox);
    sendLay->setContentsMargins(8, 4, 8, 4);
    sendLay->setSpacing(2);

    // 校验范围 + 算法（整组包进 cs_group，≠None 时高亮）。
    // 选择文件/发送文件按钮已移至左侧"历史记录"下方（fileRow）
    auto* fb = new QHBoxLayout;
    fb->setSpacing(4);
    fb->setAlignment(Qt::AlignVCenter);
    // HEX发送 置于校验范围组（第X字节…）之前
    m_chkSendHex = new ThemeCheckBox(QStringLiteral("HEX发送"), sendBox);
    connect(m_chkSendHex, &QCheckBox::stateChanged, this, &SerialTool::onSendHexToggle);
    fb->addWidget(m_chkSendHex);
    m_csGroup = new QFrame(sendBox);
    m_csGroup->setObjectName(QStringLiteral("cs_group"));
    m_csGroup->setFrameShape(QFrame::NoFrame);
    auto* csLay = new QHBoxLayout(m_csGroup);
    csLay->setContentsMargins(4, 1, 4, 1);
    csLay->setSpacing(3);
    csLay->addWidget(new QLabel(QStringLiteral("第"), m_csGroup));
    m_entryCsStart = new QLineEdit(QStringLiteral("1"), m_csGroup);
    m_entryCsStart->setFixedWidth(40);
    m_entryCsStart->setAlignment(Qt::AlignCenter);
    csLay->addWidget(m_entryCsStart);
    csLay->addWidget(new QLabel(QStringLiteral("字节至末尾"), m_csGroup));
    m_entryCsEnd = new QLineEdit(QStringLiteral("0"), m_csGroup);
    m_entryCsEnd->setFixedWidth(40);
    m_entryCsEnd->setAlignment(Qt::AlignCenter);
    csLay->addWidget(m_entryCsEnd);
    csLay->addWidget(new QLabel(QStringLiteral("加校验"), m_csGroup));
    m_cmbChecksum = new StyledComboBox(m_csGroup);
    m_cmbChecksum->addItems(sjj::CHECKSUMS);
    m_cmbChecksum->setEditable(false);
    m_cmbChecksum->setFixedWidth(110);
    csLay->addWidget(m_cmbChecksum);
    m_lblCsResult = new QLabel(QString(), m_csGroup);
    csLay->addWidget(m_lblCsResult);
    fb->addWidget(m_csGroup);
    connect(m_cmbChecksum, &QComboBox::currentIndexChanged,
            this, &SerialTool::updateChecksumHighlight);
    updateChecksumHighlight();
    fb->addStretch(1);
    sendLay->addLayout(fb);

    auto* sbar = new QHBoxLayout;
    sbar->setSpacing(4);
    auto* btnSend = new QPushButton(QStringLiteral("发送"), sendBox);
    btnSend->setObjectName(QStringLiteral("btnSend"));
    connect(btnSend, &QPushButton::clicked, this, &SerialTool::send);
    sbar->addWidget(btnSend);
    btn = new QPushButton(QStringLiteral("清空发送"), sendBox);
    btn->setObjectName(QStringLiteral("btnClearSend"));
    connect(btn, &QPushButton::clicked, this, &SerialTool::clearSend);
    sbar->addWidget(btn);
    m_chkAddCrlf = new ThemeCheckBox(QStringLiteral("加回车换行"), sendBox);
    sbar->addWidget(m_chkAddCrlf);
    m_chkTimer = new ThemeCheckBox(QStringLiteral("定时发送:"), sendBox);
    connect(m_chkTimer, &QCheckBox::stateChanged, this, &SerialTool::onTimerToggle);
    sbar->addWidget(m_chkTimer);
    m_entryInterval = new QLineEdit(QStringLiteral("1000"), sendBox);
    m_entryInterval->setFixedWidth(56);
    sbar->addWidget(m_entryInterval);
    sbar->addWidget(new QLabel(QStringLiteral("ms/次"), sendBox));
    sbar->addStretch(1);
    sendLay->addLayout(sbar);

    m_txtSend = new QTextEdit(sendBox);
    m_txtSend->setObjectName(QStringLiteral("txt_send"));   // 让全局 QSS 给发送框加边框
    m_txtSend->setFont(mono);
    m_txtSend->setFixedHeight(65);      // 固定高度，比上两行高，但不撑大整体高度
    connect(m_txtSend, &QTextEdit::textChanged, this, &SerialTool::updateCsPreview);
    sendLay->addWidget(m_txtSend, 0);   // 不拉伸，sendBox 高度跟随左侧 sbBox
    lower->addWidget(sendBox, 1);

    // ===== 状态栏 =====
    m_statusBar = new QWidget(content);
    m_statusBar->setObjectName(QStringLiteral("statusBar"));
    m_statusBar->setAttribute(Qt::WA_StyledBackground, true);
    m_sbLay = new QHBoxLayout(m_statusBar);
    m_sbLay->setContentsMargins(6, 1, 6, 1);
    m_sbLay->setSpacing(8);
    m_lblStatus = new QLabel(QStringLiteral("未连接"), m_statusBar);
    m_sbLay->addWidget(m_lblStatus, 1);

    m_progressContainer = new QWidget(m_statusBar);
    auto* pcLay = new QHBoxLayout(m_progressContainer);
    pcLay->setContentsMargins(0, 0, 0, 0);
    pcLay->setSpacing(6);
    // 进度文字显示在进度条上（内置文字）。
    // 对比度由 QSS 保证：填充深蓝 #1E66F5、未填充深色 #24273A、文字白色——
    // 白字在深蓝/深灰两种背景下都清晰（两套主题一致）
    m_progressBar = new QProgressBar(m_progressContainer);
    m_progressBar->setFixedWidth(220);
    m_progressBar->setFixedHeight(16);
    m_progressBar->setTextVisible(true);
    m_progressBar->setFormat(QStringLiteral("%v%"));
    pcLay->addWidget(m_progressBar);
    m_lblSpeed = new QLabel(QString(), m_progressContainer);
    m_lblSpeed->setMinimumWidth(70);
    m_lblSpeed->setStyleSheet(QStringLiteral("color:transparent;"));
    pcLay->addWidget(m_lblSpeed);
    m_progressContainer->hide();
    m_sbLay->addWidget(m_progressContainer, 0);

    m_btnCancelDl = new QToolButton(m_statusBar);
    m_btnCancelDl->setText(QStringLiteral("\u2715"));
    m_btnCancelDl->setToolTip(QStringLiteral("取消下载"));
    m_btnCancelDl->setAutoRaise(true);
    m_btnCancelDl->hide();
    m_sbLay->addWidget(m_btnCancelDl);

    m_btnRestart = new QToolButton(m_statusBar);
    m_btnRestart->setText(QStringLiteral("立即重启"));
    m_btnRestart->setToolTip(QStringLiteral("用下载好的新版替换当前程序并重启"));
    m_btnRestart->hide();
    m_sbLay->addWidget(m_btnRestart);

    m_lblSr = new QLabel(QStringLiteral("S:0  R:0"), m_statusBar);
    m_sbLay->addWidget(m_lblSr);
    m_lblHandshake = new QLabel(QStringLiteral("CTS=0 DSR=0 RLSD=0"), m_statusBar);
    m_sbLay->addWidget(m_lblHandshake);
    m_lblApp = new QLabel(m_statusBar);
    m_lblApp->setOpenExternalLinks(false);
    m_lblApp->setCursor(Qt::PointingHandCursor);
    connect(m_lblApp, &QLabel::linkActivated, this, &SerialTool::openGithub);
    m_sbLay->addWidget(m_lblApp);
    outer->addWidget(m_statusBar);

    // 定时发送
    m_timerSend = new QTimer(this);
    connect(m_timerSend, &QTimer::timeout, this, &SerialTool::timerSendTick);
    // 多字符串循环发送
    m_msLoopTimer = new QTimer(this);
    connect(m_msLoopTimer, &QTimer::timeout, this, &SerialTool::msLoopTick);
}

// ================= 主题 =================

void SerialTool::applyTheme(const QString& themeName) {
    m_theme = (themeName == QStringLiteral("dark")) ? QStringLiteral("dark")
                                                    : QStringLiteral("light");
    m_themeC = sjj::theme(m_theme);
    m_winBg = m_themeC.value(QStringLiteral("win_bg"));
    if (QApplication* app = qApp)
        app->setStyleSheet(sjj::buildQss(m_themeC));
    m_titleBar->applyTheme(m_themeC, m_theme);
    m_lblCsResult->setStyleSheet(
        QStringLiteral("color:%1;font-weight:bold;")
            .arg(m_themeC.value(QStringLiteral("err_color"))));
    if (m_lblSearchCount)
        m_lblSearchCount->setStyleSheet(
            QStringLiteral("color:%1;").arg(m_themeC.value(QStringLiteral("text_secondary"))));
    if (m_searchBar)
        m_searchBar->setStyleSheet(
            QStringLiteral("QWidget#searchBar{background-color:%1;border:1px solid %2;"
                           "border-radius:6px;}")
            .arg(m_themeC.value(QStringLiteral("panel_bg")))
            .arg(m_themeC.value(QStringLiteral("panel_border"))));
    m_lblApp->setText(
        QStringLiteral("<a href=\"github\" style=\"color:%1;\">SuperCOM</a>")
            .arg(m_themeC.value(QStringLiteral("link_color"))));
    const auto checkboxes = findChildren<ThemeCheckBox*>();
    for (auto* cb : checkboxes)
        cb->setThemeColors(m_themeC);
    const auto combos = findChildren<StyledComboBox*>();
    for (auto* cb : combos)
        cb->setThemeColors(m_themeC);
    updateChecksumHighlight();
    refreshStatus();
    refreshView();   // 切主题后强制重建接收区（按新主题色重新渲染已有文字）
    refreshMsRows(); // 用新主题色重建分组卡片边框/背景
    update();   // 立即触发 paintEvent 重画窗口背景
}

void SerialTool::toggleTheme() {
    const QString newTheme = (m_theme == QStringLiteral("light"))
        ? QStringLiteral("dark") : QStringLiteral("light");
    applyTheme(newTheme);
    saveParams(true);
}

QString SerialTool::themeTextColor() const {
    return m_themeC.value(QStringLiteral("text_primary"));
}

// ================= 更新检查 / 下载 =================

void SerialTool::checkUpdateAuto() {
    if (m_updateChecker)
        return;
    m_updateChecker = new UpdateChecker(this);
    connect(m_updateChecker, &UpdateChecker::resultReady,
            this, &SerialTool::onUpdateCheckResult);
    m_updateChecker->check(false);
}

void SerialTool::checkUpdateManual() {
    if (m_updateChecker)
        return;
    // 用主题 accent 色作临时提示色（两套主题下都清晰）
    m_lblStatus->setStyleSheet(
        QStringLiteral("color:%1;").arg(m_themeC.value(QStringLiteral("accent"))));
    m_lblStatus->setText(QStringLiteral("正在检查更新..."));
    m_updateChecker = new UpdateChecker(this);
    connect(m_updateChecker, &UpdateChecker::resultReady,
            this, &SerialTool::onUpdateCheckResult);
    m_updateChecker->check(true);
}

void SerialTool::onUpdateCheckResult(const QVariantMap& info, bool manual) {
    if (m_updateChecker) {
        m_updateChecker->deleteLater();
        m_updateChecker = nullptr;
    }
    if (info.isEmpty()) {
        // 网络失败：仅手动检查时提示（自动检查静默）
        if (manual) {
            m_lblStatus->setText(QStringLiteral("检查更新失败（网络不可用）"));
            QMessageBox::information(this, QStringLiteral("检查更新"),
                                     QStringLiteral("无法连接到 GitHub，请检查网络后重试。"));
        }
        refreshStatus();
        return;
    }
    const QString latest = info.value(QStringLiteral("latest")).toString();
    QString ver = QString::fromUtf8(APP_VERSION);
    while (ver.startsWith(QLatin1Char('v')) || ver.startsWith(QLatin1Char('V')))
        ver.remove(0, 1);
    if (!sjj::versionIsNewer(latest, QString::fromUtf8(APP_VERSION))) {
        if (manual) {
            m_lblStatus->setText(QStringLiteral("已是最新版本 v%1").arg(ver));
            QMessageBox::information(this, QStringLiteral("检查更新"),
                                     QStringLiteral("当前已是最新版本 v%1。").arg(ver));
            refreshStatus();
        }
        return;
    }
    // 自动检查：被忽略过的版本不再提示
    if (!manual && m_ignoreUpdateVersion == latest)
        return;
    showUpdateDialog(info);
    refreshStatus();
}

void SerialTool::showUpdateDialog(const QVariantMap& info) {
    const QString latest = info.value(QStringLiteral("latest")).toString();
    QString ver = QString::fromUtf8(APP_VERSION);
    while (ver.startsWith(QLatin1Char('v')) || ver.startsWith(QLatin1Char('V')))
        ver.remove(0, 1);
    QMessageBox box(this);
    box.setWindowTitle(QStringLiteral("发现新版本"));
    box.setIcon(QMessageBox::Information);
    box.setText(QStringLiteral("发现新版本 v%1\n当前版本 v%2").arg(latest, ver));
    const QString body = info.value(QStringLiteral("body")).toString().trimmed();
    if (!body.isEmpty()) {
        QStringList lines = body.split(QLatin1Char('\n'));
        while (lines.size() > 6)
            lines.removeLast();
        box.setInformativeText(QStringLiteral("更新说明：\n") + lines.join(QLatin1Char('\n')));
    }
    auto* btnUpdate = box.addButton(QStringLiteral("立即更新"), QMessageBox::AcceptRole);
    box.addButton(QStringLiteral("下次再说"), QMessageBox::RejectRole);
    auto* btnIgnore = box.addButton(QStringLiteral("忽略本次更新"), QMessageBox::DestructiveRole);
    box.setDefaultButton(btnUpdate);
    box.exec();
    auto* clicked = box.clickedButton();
    if (clicked == btnUpdate) {
        startUpdateDownload(info);
    } else if (clicked == btnIgnore) {
        m_ignoreUpdateVersion = latest;
        saveParams(true);
    }
}

void SerialTool::startUpdateDownload(const QVariantMap& info) {
    const QString exeUrl = info.value(QStringLiteral("exe_url")).toString();
    if (exeUrl.isEmpty()) {
        QDesktopServices::openUrl(QUrl(info.value(QStringLiteral("html_url")).toString()));
        return;
    }
    const auto ret = QMessageBox::question(
        this, QStringLiteral("自动更新"),
        QStringLiteral("将下载最新版 v%1 并自动替换当前程序（下载完成后可选择是否立即重启）。\n是否继续？")
            .arg(info.value(QStringLiteral("latest")).toString()));
    if (ret != QMessageBox::Yes)
        return;
    if (m_downloadThread && m_downloadThread->isRunning())
        return;
    // 状态栏进度条 + 取消按钮（下载时显示，"立即重启"隐藏）
    m_sbLay->setStretch(m_sbLay->indexOf(m_lblStatus), 0);
    m_sbLay->setStretch(m_sbLay->indexOf(m_progressContainer), 1);
    m_progressBar->setValue(0);
    m_progressBar->setFormat(QStringLiteral("%v%"));
    m_lblSpeed->setText(QString());
    m_lblSpeed->setStyleSheet(QStringLiteral("color:transparent;"));
    m_progressContainer->show();
    m_btnCancelDl->show();
    m_btnRestart->hide();
    m_pendingUpdateVersion = info.value(QStringLiteral("latest")).toString();
    m_downloadThread = new ExeDownloader(exeUrl, 5, this);
    connect(m_downloadThread, &ExeDownloader::progress,
            this, &SerialTool::onDownloadProgress);
    connect(m_downloadThread, &ExeDownloader::finishedOk,
            this, &SerialTool::onUpdateDownloaded);
    connect(m_downloadThread, &ExeDownloader::failed,
            this, &SerialTool::onUpdateDownloadFailed);
    // 取消按钮（先断开旧的，避免重复连接）
    disconnect(m_btnCancelDl, &QToolButton::clicked, nullptr, nullptr);
    connect(m_btnCancelDl, &QToolButton::clicked, this, [this] {
        if (m_downloadThread)
            m_downloadThread->cancel();
    });
    m_downloadThread->start();
}

void SerialTool::onDownloadProgress(qint64 done, qint64 total, int speedKbps) {
    if (total > 0) {
        m_progressBar->setRange(0, 100);
        m_progressBar->setValue(int(qMin<qint64>(100, done * 100 / total)));
        m_progressBar->setFormat(
            QStringLiteral("%v%  (%1/%2 KB)").arg(done / 1024).arg(total / 1024));
    } else {
        m_progressBar->setRange(0, 0);   // busy 模式
        m_progressBar->setValue(0);
        m_progressBar->setFormat(QStringLiteral("已下载 %1 KB").arg(done / 1024));
    }
    if (speedKbps > 0) {
        m_lblSpeed->setText(QStringLiteral("%1 KB/s").arg(speedKbps));
        m_lblSpeed->setStyleSheet(QStringLiteral("color:%1;").arg(themeTextColor()));
    }
}

void SerialTool::closeDownloadUi() {
    m_progressContainer->hide();
    m_btnCancelDl->hide();
    m_lblSpeed->setText(QString());
    m_lblSpeed->setStyleSheet(QStringLiteral("color:transparent;"));
    m_sbLay->setStretch(m_sbLay->indexOf(m_lblStatus), 1);
    m_sbLay->setStretch(m_sbLay->indexOf(m_progressContainer), 0);
    disconnect(m_btnCancelDl, &QToolButton::clicked, nullptr, nullptr);
}

void SerialTool::onUpdateDownloaded(const QString& tmp) {
    closeDownloadUi();
    refreshStatus();
    m_updateTmp = tmp;
    // 源码运行（exe 目录下没有 SuperCOM.exe）：无自动替换，提示文件位置
    const QString exeDir = QCoreApplication::applicationDirPath();
    const bool sourceMode = !QFileInfo::exists(exeDir + QStringLiteral("/SuperCOM.exe"));
    if (sourceMode) {
        m_btnRestart->hide();
        QMessageBox::information(
            this, QStringLiteral("更新下载完成"),
            QStringLiteral("新版本 v%1 已下载完成：\n%2\n\n当前为源码运行，无法自动替换 exe，已为你打开所在文件夹。")
                .arg(m_pendingUpdateVersion, tmp));
        QProcess::startDetached(QStringLiteral("explorer"),
                                {QStringLiteral("/select,"), tmp});
        return;
    }
    disconnect(m_btnRestart, &QToolButton::clicked, nullptr, nullptr);
    connect(m_btnRestart, &QToolButton::clicked, this, &SerialTool::performUpdateRestart);
    QMessageBox box(this);
    box.setWindowTitle(QStringLiteral("更新下载完成"));
    box.setIcon(QMessageBox::Information);
    box.setText(QStringLiteral("新版本 v%1 已下载完成。\n是否立即重启以完成更新？")
                    .arg(m_pendingUpdateVersion));
    auto* btnNow = box.addButton(QStringLiteral("立即重启"), QMessageBox::AcceptRole);
    box.addButton(QStringLiteral("稍后"), QMessageBox::RejectRole);
    box.setDefaultButton(btnNow);
    box.exec();
    if (box.clickedButton() == btnNow) {
        performUpdateRestart();
    } else {
        // 状态栏显示"立即重启 v..."按钮，稍后手动触发
        m_btnRestart->setText(QStringLiteral("立即重启 v%1").arg(m_pendingUpdateVersion));
        m_btnRestart->show();
    }
}

void SerialTool::onUpdateDownloadFailed(const QString& err) {
    closeDownloadUi();
    refreshStatus();
    QMessageBox::warning(this, QStringLiteral("更新下载失败"),
                         QStringLiteral("下载更新失败：%1\n请稍后重试或前往 GitHub 手动下载。").arg(err));
}

void SerialTool::performUpdateRestart() {
    if (m_updateTmp.isEmpty() || !QFileInfo::exists(m_updateTmp)) {
        QMessageBox::critical(this, QStringLiteral("更新失败"),
                              QStringLiteral("下载文件已丢失：%1\n请重新检查更新。").arg(m_updateTmp));
        m_btnRestart->hide();
        return;
    }
    const QString exeDir = QCoreApplication::applicationDirPath();
    const QString target = exeDir + QStringLiteral("/SuperCOM.exe");
    const QString bat = exeDir + QStringLiteral("/_supercom_update.bat");
    // cmd/start 对正斜杠路径兼容性差（explorer.exe 传正斜杠会失败），统一转反斜杠
    const QString tmp = QDir::toNativeSeparators(m_updateTmp);
    const QString tgt = QDir::toNativeSeparators(target);
    // bat 用 UTF-8 with BOM 写入，cmd 需 chcp 65001 切到 UTF-8 代码页
    // （默认 GBK 代码页会导致含中文的 exe 路径乱码，move 找不到文件）
    // 启动新 exe 用 cmd 内建 start（explorer.exe 启动 exe 行为不可靠，实测会失败）
    const QString script = QStringLiteral(
        "@echo off\r\n"
        "chcp 65001 >nul\r\n"
        ":wait\r\n"
        "tasklist /fi \"imagename eq SuperCOM.exe\" 2>nul "
        "| find /i \"SuperCOM.exe\" >nul\r\n"
        "if errorlevel 1 goto replace\r\n"
        "timeout /t 1 /nobreak >nul\r\n"
        "goto wait\r\n"
        ":replace\r\n"
        "move /y \"%1\" \"%2\" >nul 2>&1\r\n"
        "start \"\" \"%2\"\r\n"
        "del \"%~f0\"\r\n")
        // QString::arg 没有多参重载——必须链式调用
        .arg(tmp).arg(tgt);
    QFile f(bat);
    if (!f.open(QIODevice::WriteOnly)) {
        QMessageBox::critical(this, QStringLiteral("更新失败"),
                              QStringLiteral("无法写入更新脚本：%1").arg(f.errorString()));
        return;
    }
    f.write("\xEF\xBB\xBF");
    f.write(script.toUtf8());
    f.close();
    // CREATE_NO_WINDOW 静默启动批处理，不弹黑窗
    if (!QProcess::startDetached(QStringLiteral("cmd"), {QStringLiteral("/c"), bat})) {
        QMessageBox::critical(this, QStringLiteral("更新失败"),
                              QStringLiteral("无法启动更新程序。\n新版本已下载到：%1\n请手动替换 %2")
                                  .arg(m_updateTmp).arg(target));
        return;
    }
    m_btnRestart->hide();
    m_lblStatus->setText(QStringLiteral("更新完成，正在重启..."));
    QTimer::singleShot(800, this, &SerialTool::close);
}

// ================= 端口 / 串口 =================

void SerialTool::refreshPorts() {
    m_suppressApply = true;   // 刷新列表期间不触发"自动重开"
    struct PortInfo {
        QString device;
        QString desc;
        int comNum = -1;
    };
    QVector<PortInfo> ports;
    static const QRegularExpression re(QStringLiteral("^COM(\\d+)$"),
                                       QRegularExpression::CaseInsensitiveOption);
    const auto infos = QSerialPortInfo::availablePorts();
    for (const auto& info : infos) {
        PortInfo p;
        p.device = info.portName();
        p.desc = info.description().trimmed();
        const auto m = re.match(p.device);
        if (m.hasMatch())
            p.comNum = m.captured(1).toInt();
        ports.append(p);
    }
    std::sort(ports.begin(), ports.end(), [](const PortInfo& a, const PortInfo& b) {
        if (a.comNum >= 0 && b.comNum >= 0)
            return a.comNum < b.comNum;
        if (a.comNum >= 0)
            return true;
        if (b.comNum >= 0)
            return false;
        return a.device < b.device;
    });
    m_portMap.clear();
    QStringList items;
    for (const auto& p : ports) {
        // 显示 "COM7  名称"；去掉描述末尾重复附带的 (COMx)
        QString desc = p.desc;
        desc.remove(QRegularExpression(QStringLiteral("\\s*\\(?COM\\d+\\)?\\s*$")));
        QString label = p.device;
        if (!desc.isEmpty())
            label += QStringLiteral("  ") + desc;
        items.append(label);
        m_portMap[label] = p.device;
    }
    if (items.isEmpty()) {
        items.append(QStringLiteral("(无可用端口)"));
        m_portMap[items.first()] = QString();
    }
    const QString cur = m_cmbPort->currentText();
    m_cmbPort->clear();
    m_cmbPort->addItems(items);
    if (items.contains(cur))
        m_cmbPort->setCurrentText(cur);
    m_suppressApply = false;
}

QString SerialTool::getSelectedPort() const {
    const QString label = m_cmbPort->currentText();
    if (m_portMap.contains(label))
        return m_portMap.value(label);
    const QStringList parts = label.split(QLatin1Char(' '));
    return parts.isEmpty() ? QString() : parts.first();
}

void SerialTool::showSettingsDialog() {
    if (m_settingsWin) {
        m_settingsWin->showNormal();
        m_settingsWin->raise();
        m_settingsWin->activateWindow();
        return;
    }
    auto* dlg = new QDialog(this);
    dlg->setWindowTitle(QStringLiteral("更多串口设置"));
    dlg->setModal(false);
    m_settingsWin = dlg;
    connect(dlg, &QDialog::finished, this, [this] { m_settingsWin = nullptr; });
    auto* form = new QFormLayout(dlg);

    auto* cmbBaud = new StyledComboBox(dlg);
    cmbBaud->addItems(sjj::DEFAULT_BAUDRATES);
    cmbBaud->setCurrentText(m_cmbBaud->currentText());
    form->addRow(QStringLiteral("波特率:"), cmbBaud);

    auto* cmbDatabits = new StyledComboBox(dlg);
    cmbDatabits->addItems(sjj::DEFAULT_DATABITS);
    cmbDatabits->setCurrentText(m_databits);
    form->addRow(QStringLiteral("数据位:"), cmbDatabits);

    auto* cmbStopbits = new StyledComboBox(dlg);
    cmbStopbits->addItems(sjj::DEFAULT_STOPBITS);
    cmbStopbits->setCurrentText(m_stopbits);
    form->addRow(QStringLiteral("停止位:"), cmbStopbits);

    auto* cmbParity = new StyledComboBox(dlg);
    cmbParity->addItems(sjj::DEFAULT_PARITY);
    cmbParity->setCurrentText(m_parity);
    form->addRow(QStringLiteral("校验位:"), cmbParity);

    auto* cmbFlow = new StyledComboBox(dlg);
    cmbFlow->addItems(sjj::DEFAULT_FLOW);
    cmbFlow->setCurrentText(m_flow);
    form->addRow(QStringLiteral("流控:"), cmbFlow);

    // ---- 接收数据分包：两种方式可单独或同时启用，任一条件满足即切出一条 ----
    auto* splitTitle = new QLabel(QStringLiteral("—— 接收数据分包 ——"), dlg);
    splitTitle->setAlignment(Qt::AlignCenter);
    form->addRow(splitTitle);

    // 勾选框直接作为输入框的标签：一行同时表达「是否启用」+ 对应参数
    auto* chkSplitTime = new ThemeCheckBox(QStringLiteral("读超时(ms):"), dlg);
    chkSplitTime->setToolTip(QStringLiteral(
        "勾选＝启用空闲超时分包：数据流停顿超过该时间后，把缓存数据作为一条显示。\n"
        "默认按此方式分包；值越小实时性越高，值越大越容易把断续的一帧归并为一条。"));
    chkSplitTime->setChecked(m_splitByTime);
    auto* entryTimeout = new QLineEdit(m_readTimeout, dlg);
    entryTimeout->setToolTip(chkSplitTime->toolTip());
    form->addRow(chkSplitTime, entryTimeout);

    auto* chkSplitChar = new ThemeCheckBox(QStringLiteral("分隔符:"), dlg);
    chkSplitChar->setToolTip(QStringLiteral(
        "勾选＝启用按字符分包：数据流中出现该分隔符时立即切出一条；\n"
        "可与超时同时勾选，谁先满足谁切。两种方式都不勾选时按空闲超时分包。"));
    chkSplitChar->setChecked(m_splitByChar);
    auto* entrySplitPattern = new QLineEdit(m_splitPatternText, dlg);
    entrySplitPattern->setToolTip(QStringLiteral(
        "分包分隔符，支持转义：\\r \\n \\t \\0 \\\\ \\xNN（如 \\r\\n，单字节 0x0A 写 \\x0A）。\n"
        "分隔符保留在该条数据内，接收字节不做任何修饰，按当前 HEX / 编码设置如实显示；\n"
        "分隔符被拆到多次读取里也能正确拼齐。"));
    form->addRow(chkSplitChar, entrySplitPattern);

    auto* btns = new QHBoxLayout;
    btns->addStretch(1);
    auto* ok = new QPushButton(QStringLiteral("确定"), dlg);
    connect(ok, &QPushButton::clicked, this,
            [this, dlg, cmbBaud, cmbDatabits, cmbStopbits, cmbParity, cmbFlow,
             entryTimeout, chkSplitTime, chkSplitChar, entrySplitPattern] {
                applySettings(dlg, cmbBaud, cmbDatabits, cmbStopbits, cmbParity,
                              cmbFlow, entryTimeout, chkSplitTime, chkSplitChar,
                              entrySplitPattern);
            });
    btns->addWidget(ok);
    auto* cancel = new QPushButton(QStringLiteral("取消"), dlg);
    connect(cancel, &QPushButton::clicked, dlg, &QDialog::close);
    btns->addWidget(cancel);
    form->addRow(btns);
    dlg->resize(360, 320);
    dlg->show();
}

void SerialTool::applySettings(QDialog* dlg, QComboBox* cmbBaud, QComboBox* cmbDatabits,
                               QComboBox* cmbStopbits, QComboBox* cmbParity,
                               QComboBox* cmbFlow, QLineEdit* entryTimeout,
                               ThemeCheckBox* chkSplitTime, ThemeCheckBox* chkSplitChar,
                               QLineEdit* entrySplitPattern) {
    bool ok = false;
    const int t = entryTimeout->text().toInt(&ok);
    if (!ok || t < 1) {
        QMessageBox::critical(this, QStringLiteral("错误"), QStringLiteral("读超时必须是正整数"));
        return;
    }
    cmbBaud->currentText().toInt(&ok);
    if (!ok) {
        QMessageBox::critical(this, QStringLiteral("错误"), QStringLiteral("波特率必须是数字"));
        return;
    }
    bool patOk = false;
    QString err;
    const QString patText = entrySplitPattern->text();
    const QByteArray pat = sjj::parseEscapeBytes(patText, patOk, err);
    if (!patOk) {
        QMessageBox::critical(this, QStringLiteral("错误"), err);
        return;
    }
    if (chkSplitChar->isChecked() && pat.isEmpty()) {
        QMessageBox::critical(this, QStringLiteral("错误"),
                              QStringLiteral("按字符分包需要填写非空的分隔符"));
        return;
    }

    // 分包规则切换前先把旧规则下攒的残留作为一条送出，避免新旧数据混进同一条
    flushRxPacket();

    m_readTimeout = QString::number(t);
    m_splitByTime = chkSplitTime->isChecked();
    m_splitByChar = chkSplitChar->isChecked();
    m_splitPatternText = patText;
    m_splitPattern = pat;

    // 仅改分包设置时不要重开串口（否则会中断正在进行的接收）
    const bool serialChanged =
        cmbBaud->currentText() != m_cmbBaud->currentText()
        || cmbDatabits->currentText() != m_databits
        || cmbStopbits->currentText() != m_stopbits
        || cmbParity->currentText() != m_parity
        || cmbFlow->currentText() != m_flow;

    m_suppressApply = true;   // 批量赋值期间不触发"自动重开"
    m_cmbBaud->setCurrentText(cmbBaud->currentText());
    m_databits = cmbDatabits->currentText();
    m_stopbits = cmbStopbits->currentText();
    m_parity = cmbParity->currentText();
    m_flow = cmbFlow->currentText();
    m_suppressApply = false;
    refreshStatus();
    saveParams(true);
    if (serialChanged)
        applySerialSettings();   // 串口参数变了 → 立即用新参数重开
    dlg->close();
}

void SerialTool::onPortChanged(int) {
    if (m_suppressApply)
        return;
    applySerialSettings();
}

void SerialTool::onBaudChanged(int) {
    if (m_suppressApply)
        return;
    applySerialSettings();
}

void SerialTool::applySerialSettings() {
    // 串口参数被修改：若串口已打开则立即关闭并按新参数重开；未打开仅刷新状态栏
    if (!m_ser || !m_ser->isOpen()) {
        refreshStatus();
        return;
    }
    const QString port = getSelectedPort();
    if (port.isEmpty() || port == QStringLiteral("(无可用端口)")) {
        QMessageBox::warning(this, QStringLiteral("提示"), QStringLiteral("请先选择有效的串口"));
        closePort();
        return;
    }
    closePort();
    openPort();
}

void SerialTool::togglePort() {
    if (m_ser && m_ser->isOpen())
        closePort();
    else
        openPort();
}

void SerialTool::openPort() {
    const QString port = getSelectedPort();
    if (port.isEmpty() || port == QStringLiteral("(无可用端口)")) {
        QMessageBox::warning(this, QStringLiteral("提示"), QStringLiteral("请先选择串口"));
        return;
    }
    bool okBaud = false;
    const int baud = m_cmbBaud->currentText().toInt(&okBaud);
    if (!okBaud) {
        QMessageBox::critical(this, QStringLiteral("打开失败"), QStringLiteral("波特率必须是数字"));
        return;
    }
    auto* ser = new QSerialPort(port, this);
    ser->setBaudRate(baud);
    if (m_databits == QStringLiteral("5")) ser->setDataBits(QSerialPort::Data5);
    else if (m_databits == QStringLiteral("6")) ser->setDataBits(QSerialPort::Data6);
    else if (m_databits == QStringLiteral("7")) ser->setDataBits(QSerialPort::Data7);
    else ser->setDataBits(QSerialPort::Data8);
    if (m_parity == QStringLiteral("Even")) ser->setParity(QSerialPort::EvenParity);
    else if (m_parity == QStringLiteral("Odd")) ser->setParity(QSerialPort::OddParity);
    else if (m_parity == QStringLiteral("Mark")) ser->setParity(QSerialPort::MarkParity);
    else if (m_parity == QStringLiteral("Space")) ser->setParity(QSerialPort::SpaceParity);
    else ser->setParity(QSerialPort::NoParity);
    if (m_stopbits == QStringLiteral("1.5")) ser->setStopBits(QSerialPort::OneAndHalfStop);
    else if (m_stopbits == QStringLiteral("2")) ser->setStopBits(QSerialPort::TwoStop);
    else ser->setStopBits(QSerialPort::OneStop);
    if (m_flow == QStringLiteral("RTS/CTS")) ser->setFlowControl(QSerialPort::HardwareControl);
    else if (m_flow == QStringLiteral("XON/XOFF")) ser->setFlowControl(QSerialPort::SoftwareControl);
    else ser->setFlowControl(QSerialPort::NoFlowControl);

    if (!ser->open(QIODevice::ReadWrite)) {
        QMessageBox::critical(this, QStringLiteral("打开失败"), ser->errorString());
        delete ser;
        return;
    }
    // 默认不拉高 DTR/RTS，避免开发板复位/进入下载模式
    ser->setDataTerminalReady(false);
    ser->setRequestToSend(false);
    connect(ser, &QSerialPort::readyRead, this, &SerialTool::onSerialData);
    connect(ser, &QSerialPort::errorOccurred, this, &SerialTool::onSerialError);
    connect(ser, &QSerialPort::bytesWritten, this, &SerialTool::onFileBytesWritten);
    m_ser = ser;
    ++m_rxEpoch;
    // 打开新串口前清掉待分包缓存，避免上个会话残留数据冒出
    m_rxBuf.clear();
    m_pktStartTs.clear();
    m_pktTimer->stop();
    m_btnOpen->setText(QStringLiteral("关闭串口"));
    m_btnOpen->setObjectName(QStringLiteral("btnDisconnect"));
    m_btnOpen->style()->unpolish(m_btnOpen);
    m_btnOpen->style()->polish(m_btnOpen);
    refreshStatus();
    addRecord(QStringLiteral("sys"),
              QStringLiteral("串口已打开: %1 @ %2").arg(port).arg(baud));
}

void SerialTool::closePort() {
    if (m_timerSend && m_timerSend->isActive())
        m_timerSend->stop();
    if (m_ser) {
        m_ser->disconnect(this);
        m_ser->close();
        m_ser->deleteLater();
        m_ser = nullptr;
    }
    // 代际号递增：关闭瞬间使旧串口排队的信号全部失效
    ++m_rxEpoch;
    // 丢弃尚未刷新的接收/发送事件缓冲
    m_pendingEvents.clear();
    // 关闭串口前把待分包缓存作为一条送出（避免最后一段数据丢失）
    flushRxPacket();
    m_btnOpen->setText(QStringLiteral("打开串口"));
    m_btnOpen->setObjectName(QStringLiteral("btnConnect"));
    m_btnOpen->style()->unpolish(m_btnOpen);
    m_btnOpen->style()->polish(m_btnOpen);
    refreshStatus();
    addRecord(QStringLiteral("sys"), QStringLiteral("串口已关闭"));
}

bool SerialTool::updateSplitPattern(const QString& text, QString& err) {
    bool ok = false;
    const QByteArray pat = sjj::parseEscapeBytes(text, ok, err);
    if (!ok)
        return false;
    m_splitPatternText = text;
    m_splitPattern = pat;
    return true;
}

void SerialTool::onSerialData() {
    const QByteArray data = m_ser->readAll();
    m_rxBytes += quint64(data.size());
    if (data.isEmpty())
        return;
    feedRxData(data);
}

void SerialTool::feedRxData(const QByteArray& data) {
    if (data.isEmpty())
        return;
    if (m_rxBuf.isEmpty())
        m_pktStartTs = sjj::nowTimestamp();   // 包首字节到达时刻作该条时间戳
    m_rxBuf += data;

    // 按字符分包：分隔符出现即把完整包切出，未收完的部分仍留在 m_rxBuf，
    // 因此分隔符被拆到两次 readyRead 里也能正确匹配。
    const bool byChar = m_splitByChar && !m_splitPattern.isEmpty();
    if (byChar)
        splitRxByPattern();

    // 按时间分包：有残留就重置空闲计时。两种都没启用时兜底按时间，
    // 避免数据一直攒在缓存里不显示。
    const bool byTime = m_splitByTime || !byChar;
    if (!m_rxBuf.isEmpty() && byTime)
        m_pktTimer->start(qMax(1, m_readTimeout.toInt()));
    else
        m_pktTimer->stop();

    // 防内存撑爆：缓存达到上限立即强制分包（即使未超时/未出现分隔符）
    if (m_rxBuf.size() >= MAX_RX_BUF)
        flushRxPacket();
}

void SerialTool::splitRxByPattern() {
    // 分隔符保留在包内（不丢字节）；一次读取可能含多个完整包，逐个切出。
    QVector<qsizetype> ends;
    qsizetype from = 0;
    while ((from = m_rxBuf.indexOf(m_splitPattern, from)) >= 0) {
        from += m_splitPattern.size();
        ends.append(from);
    }
    if (ends.isEmpty())
        return;
    QString ts = m_pktStartTs.isEmpty() ? sjj::nowTimestamp() : m_pktStartTs;
    qsizetype start = 0;
    for (const qsizetype end : ends) {
        emitRxPacket(m_rxBuf.mid(start, end - start), ts);
        // 同一批数据里后续包的首字节时刻无法精确还原，用切包时刻近似
        ts = sjj::nowTimestamp();
        start = end;
    }
    m_rxBuf = m_rxBuf.mid(start);
    m_pktStartTs = m_rxBuf.isEmpty() ? QString() : ts;
}

void SerialTool::emitRxPacket(const QByteArray& bytes, const QString& ts) {
    if (bytes.isEmpty())
        return;
    Record r;
    r.kind = QStringLiteral("rx");
    r.rawBytes = bytes;
    r.ts = ts;
    m_pendingEvents.append(r);
    if (m_pendingEvents.size() > sjj::MAX_RECORDS)
        m_pendingEvents = m_pendingEvents.mid(m_pendingEvents.size() - sjj::MAX_RECORDS);
}

void SerialTool::flushRxPacket() {
    m_pktTimer->stop();
    if (m_rxBuf.isEmpty())
        return;
    emitRxPacket(m_rxBuf, m_pktStartTs.isEmpty() ? sjj::nowTimestamp() : m_pktStartTs);
    m_rxBuf.clear();
    m_pktStartTs.clear();
}

void SerialTool::onSerialError(QSerialPort::SerialPortError err) {
    if (err == QSerialPort::NoError)
        return;
    if (!m_ser || !m_ser->isOpen())
        return;
    addRecord(QStringLiteral("sys"),
              QStringLiteral("[读取异常] %1").arg(m_ser->errorString()));
    closePort();
}

void SerialTool::refreshStatus() {
    QString port = getSelectedPort();
    if (port.isEmpty())
        port = QStringLiteral("-");
    const QString param = QStringLiteral("%1,%2,%3,%4,%5")
        .arg(m_cmbBaud->currentText(), m_databits, m_parity.left(1),
             m_stopbits, sjj::flowShort(m_flow));
    if (m_ser && m_ser->isOpen()) {
        m_lblStatus->setText(port + QStringLiteral(" 已打开 ") + param);
        m_lblStatus->setStyleSheet(
            QStringLiteral("color:%1;").arg(m_themeC.value(QStringLiteral("ok_color"))));
    } else {
        m_lblStatus->setText(port + QStringLiteral(" 已关闭 ") + param);
        m_lblStatus->setStyleSheet(
            QStringLiteral("color:%1;").arg(m_themeC.value(QStringLiteral("text_primary"))));
    }
}

// ================= 接收渲染 =================

QString SerialTool::decodeBytesByCurrent(const QByteArray& data) const {
    return sjj::decodeBytes(data, m_cmbEncoding->currentText());
}

QString SerialTool::rxBody(const QByteArray& data) const {
    if (m_chkShowHex->isChecked())
        return sjj::hexBody(data);
    return decodeBytesByCurrent(data);
}

QPair<QString, QString> SerialTool::renderParts(const Record& rec) const {
    const QString ts = rec.ts.isEmpty() ? sjj::nowTimestamp() : rec.ts;
    QString mark, text;
    if (rec.kind == QStringLiteral("rx")) {
        mark = m_chkShowTs->isChecked() ? (ts + QStringLiteral("< ")) : QStringLiteral("< ");
        // 接收数据原样呈现：不做 trim / 替换 / 补字符，收到的字节按当前
        // HEX / 编码设置如实显示（分隔符分包时也保留帧尾的完整字节）。
        text = rxBody(rec.rawBytes);
    } else if (rec.kind == QStringLiteral("tx")) {
        mark = m_chkShowTs->isChecked() ? (ts + QStringLiteral("> ")) : QStringLiteral("> ");
        if (!rec.txData.isEmpty()) {
            QByteArray body = rec.txData;
            if (!rec.txCs.isEmpty())
                body = rec.txData.left(rec.txData.size() - rec.txCs.size());
            if (m_chkShowHex->isChecked())
                text = sjj::spacedHex(body);
            else if (rec.hexInput)
                text = decodeBytesByCurrent(body);
            else
                text = rec.rawText;
            if (!rec.txCs.isEmpty())
                text += QStringLiteral(" [") + sjj::spacedHex(rec.txCs) + QStringLiteral("]");
        } else {
            text = rec.rawText;
        }
    } else { // sys
        mark = m_chkShowTs->isChecked() ? (ts + QLatin1Char(' ')) : QString();
        text = rec.rawText;
    }
    return {mark, text};
}

QString SerialTool::renderRecord(const Record& rec) const {
    const auto parts = renderParts(rec);
    return parts.first + parts.second + QLatin1Char('\n');
}

void SerialTool::addTxRecord(const QString& raw, const QByteArray& data,
                             const QByteArray& csBytes, bool hexInput) {
    Record r;
    r.kind = QStringLiteral("tx");
    r.rawText = raw;
    r.txData = data;
    r.txCs = csBytes;
    r.hexInput = hexInput;
    // 时间戳在发送时刻生成；事件进入统一队列，与接收保持真实顺序
    r.ts = sjj::nowTimestamp();
    m_pendingEvents.append(r);
}

void SerialTool::addRecord(const QString& kind, const QString& raw) {
    Record r;
    r.kind = kind;
    r.rawText = raw;
    r.ts = sjj::nowTimestamp();
    m_pendingEvents.append(r);
}

void SerialTool::appendToView(QVector<Record>& recs) {
    ScrollGuard guard(m_programScroll);
    // 暂停语义 = 不自动滚动：数据照常写入视图，滚动位置锁定不动。
    // 用户手动向下滚动即可看到新数据；滚到最底部时 onRecvScroll 自动解除暂停。
    // （裁剪删顶行引发的 range 收缩/clamp 由 onRecvScroll 的 range 检测过滤，不会误改暂停）
    // 超上限裁剪最旧（保持内存与视图一致）：
    // 条数上限 MAX_RECORDS + 文档总字符数上限 MAX_DOC_CHARS 双保险，
    // 防止大包堆积让 QTextDocument 膨胀到拖慢布局/滚动/HEX 重建
    if (m_records.size() > sjj::MAX_RECORDS ||
        m_txtRecv->document()->characterCount() > sjj::MAX_DOC_CHARS) {
        const int overflow = qMax(0, m_records.size() - sjj::MAX_RECORDS);
        const QVector<Record> removed = m_records.mid(0, overflow);
        m_records.remove(0, overflow);
        int delLines = 0;
        for (const auto& r : removed)
            delLines += renderRecord(r).count(QLatin1Char('\n'));
        // 字符数仍超限（大包场景）：继续丢最旧直到达标
        while (m_txtRecv->document()->characterCount() > sjj::MAX_DOC_CHARS
               && !m_records.isEmpty()) {
            const Record r = m_records.first();
            m_records.removeFirst();
            delLines += renderRecord(r).count(QLatin1Char('\n'));
        }
        if (m_btnFilter->isChecked()) {
            refreshView();
            return;
        }
        if (delLines > 0)
            deleteTopLines(delLines);
    }
    const QString needle = m_entrySearch->text();
    if (m_btnFilter->isChecked() && !needle.isEmpty()) {
        QVector<Record> filtered;
        filtered.reserve(recs.size());
        for (const auto& r : recs) {
            if (searchMatch(renderRecord(r)))
                filtered.append(r);
        }
        if (filtered.isEmpty())
            return;
        recs = filtered;
    }
    QTextCursor cursor = m_txtRecv->textCursor();
    cursor.movePosition(QTextCursor::End);
    // 批量插入：edit block 内所有 insertText 合并为一次文档重排/重绘
    cursor.beginEditBlock();
    const QColor accent(m_themeC.value(QStringLiteral("accent")));
    const QColor editFg(m_themeC.value(QStringLiteral("edit_fg")));
    for (const auto& r : recs) {
        const auto parts = renderParts(r);
        if (!parts.first.isEmpty()) {
            QTextCharFormat fmt;
            fmt.setForeground(accent);
            fmt.setFontWeight(QFont::Bold);
            cursor.insertText(parts.first, fmt);
        }
        QTextCharFormat plain;
        plain.setForeground(editFg);
        plain.setFontWeight(QFont::Normal);
        cursor.setCharFormat(plain);
        cursor.insertText(parts.second);
        cursor.insertText(QStringLiteral("\n"));
    }
    cursor.endEditBlock();
    if (!needle.isEmpty())
        applyHighlight();
    if (!m_chkPause->isChecked())
        m_txtRecv->verticalScrollBar()->setValue(m_txtRecv->verticalScrollBar()->maximum());
    else if (m_btnScrollBottom && !m_btnScrollBottom->isVisible()) {
        // 暂停刷新期间新数据积压：亮出"回到底部"按钮，随时一键恢复
        positionScrollBottomBtn();
        m_btnScrollBottom->show();
    }
}

void SerialTool::deleteTopLines(int n) {
    QTextCursor cursor(m_txtRecv->document());
    cursor.beginEditBlock();   // 合并多行删除为一次文档重排
    cursor.movePosition(QTextCursor::Start);
    for (int i = 0; i < n; ++i) {
        if (!cursor.movePosition(QTextCursor::Down, QTextCursor::KeepAnchor))
            break;
    }
    cursor.removeSelectedText();
    cursor.endEditBlock();
}

void SerialTool::refreshView() {
    ScrollGuard guard(m_programScroll);
    const bool filt = m_btnFilter->isChecked();
    const QColor accent(m_themeC.value(QStringLiteral("accent")));
    const QColor editFg(m_themeC.value(QStringLiteral("edit_fg")));
    m_txtRecv->blockSignals(true);
    m_txtRecv->clear();
    QTextCursor cursor = m_txtRecv->textCursor();
    cursor.movePosition(QTextCursor::End);
    // 全量重建：edit block 把上千次 insertText 合并为一次文档重排
    // （HEX 显示切换/主题切换走这里，文档大时避免逐条插入卡死 UI）
    cursor.beginEditBlock();
    for (const auto& r : m_records) {
        const QString rendered = renderRecord(r);
        if (filt && !searchMatch(rendered))
            continue;
        const auto parts = renderParts(r);
        if (!parts.first.isEmpty()) {
            QTextCharFormat fmt;
            fmt.setForeground(accent);
            fmt.setFontWeight(QFont::Bold);
            cursor.insertText(parts.first, fmt);
        }
        QTextCharFormat plain;
        plain.setForeground(editFg);
        plain.setFontWeight(QFont::Normal);
        cursor.setCharFormat(plain);
        cursor.insertText(parts.second + QStringLiteral("\n"));
    }
    cursor.endEditBlock();
    m_txtRecv->blockSignals(false);
    applyHighlight();
    if (!m_chkPause->isChecked())
        m_txtRecv->verticalScrollBar()->setValue(m_txtRecv->verticalScrollBar()->maximum());
    else if (m_btnScrollBottom && !m_btnScrollBottom->isVisible()) {
        // 暂停刷新期间新数据积压：亮出"回到底部"按钮，随时一键恢复
        positionScrollBottomBtn();
        m_btnScrollBottom->show();
    }
}

QString SerialTool::accentHex() const {
    return m_themeC.value(QStringLiteral("accent"));
}

void SerialTool::applyHighlight() {
    const QString needle = m_entrySearch->text();
    m_searchHits.clear();
    m_searchHitLens.clear();
    QList<QTextEdit::ExtraSelection> sels;
    if (!needle.isEmpty()) {
        const bool useRegex = m_btnRegex->isChecked();
        // Cc 亮=区分大小写；不亮=大小写不敏感
        const bool caseSensitive = m_btnCase->isChecked();
        // 普通匹配：黄色底黑字
        QTextCharFormat fmt;
        fmt.setBackground(QColor(QStringLiteral("#ffe066")));
        fmt.setForeground(QColor(QStringLiteral("#000000")));
        // 当前选中匹配：橙色底黑字
        QTextCharFormat curFmt;
        curFmt.setBackground(QColor(QStringLiteral("#fe640b")));
        curFmt.setForeground(QColor(QStringLiteral("#ffffff")));
        QTextDocument* doc = m_txtRecv->document();
        QTextCursor cursor(doc);
        int idx = 0;
        if (useRegex) {
            QRegularExpression re(needle);
            if (!caseSensitive)
                re.setPatternOptions(QRegularExpression::CaseInsensitiveOption);
            if (!re.isValid()) {
                // 正则非法：提示并返回（不渲染高亮）
                if (m_lblSearchCount) {
                    m_lblSearchCount->setText(QStringLiteral("正则✗"));
                    m_lblSearchCount->setStyleSheet(
                        QStringLiteral("color:#d22020;font-weight:bold;"));
                }
                m_txtRecv->setExtraSelections(sels);
                return;
            }
            while (true) {
                cursor = doc->find(re, cursor);
                if (cursor.isNull())
                    break;
                const int matchStart = cursor.selectionStart();
                const int len = cursor.selectionEnd() - matchStart;
                // 防止零长度匹配（如 "123|" 能匹配空串）导致无限循环
                if (len == 0) {
                    // 若已在文档末尾，推进后仍原地踏步 -> 死循环，必须 break
                    const int docEnd = doc->characterCount() - 1;
                    if (matchStart >= docEnd)
                        break;
                    // 推进 1 字符继续搜索，跳过空匹配
                    cursor.setPosition(matchStart + 1);
                    continue;
                }
                m_searchHits.append(matchStart);
                m_searchHitLens.append(len);
                QTextEdit::ExtraSelection sel;
                sel.format = (idx == m_searchIndex) ? curFmt : fmt;
                sel.cursor = cursor;
                sels.append(sel);
                ++idx;
            }
        } else {
            // 普通模式：大小写标志
            QTextDocument::FindFlags flags = QTextDocument::FindFlag(0);
            if (caseSensitive)
                flags |= QTextDocument::FindCaseSensitively;
            while (true) {
                cursor = doc->find(needle, cursor, flags);
                if (cursor.isNull())
                    break;
                m_searchHits.append(cursor.selectionStart());
                m_searchHitLens.append(needle.length());
                QTextEdit::ExtraSelection sel;
                sel.format = (idx == m_searchIndex) ? curFmt : fmt;
                sel.cursor = cursor;
                sels.append(sel);
                ++idx;
            }
        }
        // 索引越界保护（文本追加/裁剪后位置失效时重置）
        if (m_searchIndex >= m_searchHits.size() || m_searchIndex < 0)
            m_searchIndex = -1;
    } else {
        m_searchIndex = -1;
    }
    m_txtRecv->setExtraSelections(sels);
    updateSearchCountLabel();
}

void SerialTool::updateSearchCountLabel() {
    if (!m_lblSearchCount)
        return;
    // 复位样式（清除此前"正则✗"的红色），恢复主题色
    m_lblSearchCount->setStyleSheet(
        QStringLiteral("color:%1;").arg(m_themeC.value(QStringLiteral("text_secondary"))));
    const QString needle = m_entrySearch->text();
    const int total = m_searchHits.size();
    if (needle.isEmpty() || total == 0) {
        m_lblSearchCount->setText(QStringLiteral("0/0"));
    } else if (m_searchIndex < 0) {
        // 尚未跳转：只显示总数（引导点击箭头）
        m_lblSearchCount->setText(QStringLiteral("%1/%1").arg(total));
    } else {
        m_lblSearchCount->setText(QStringLiteral("%1/%2").arg(m_searchIndex + 1).arg(total));
    }
    m_btnSearchPrev->setEnabled(!needle.isEmpty() && total > 0);
    m_btnSearchNext->setEnabled(!needle.isEmpty() && total > 0);
}

void SerialTool::searchJump(int delta) {
    if (m_searchHits.isEmpty())
        return;
    const int total = m_searchHits.size();
    int idx;
    if (m_searchIndex < 0) {
        // 未定位：向下从第一个开始，向上从最后一个开始
        idx = (delta > 0) ? 0 : total - 1;
    } else {
        idx = (m_searchIndex + delta + total) % total;
    }
    m_searchIndex = idx;
    // 跳转并滚动到匹配位置（用实际匹配长度，正则时 != needle 长度）
    QTextCursor cursor(m_txtRecv->document());
    cursor.setPosition(m_searchHits[idx]);
    const int len = (idx < m_searchHitLens.size()) ? m_searchHitLens[idx]
                                                    : m_entrySearch->text().length();
    cursor.setPosition(m_searchHits[idx] + len, QTextCursor::KeepAnchor);
    m_txtRecv->setTextCursor(cursor);
    m_txtRecv->ensureCursorVisible();
    applyHighlight();   // 刷新当前匹配高亮与计数
}

void SerialTool::toggleSearchBar() {
    if (!m_searchBar)
        return;
    if (m_searchBar->isVisible()) {
        hideSearchBar();
        return;
    }
    m_searchBar->show();
    m_searchBar->raise();
    // 接收区有选中文字 → 优先填入（覆盖上次记忆）
    QString sel = m_txtRecv->textCursor().selectedText();
    sel.replace(QChar(0x2029), QLatin1Char('\n'));   // 段落分隔符→换行
    sel.replace(QChar(0x2028), QLatin1Char('\n'));   // 行分隔符→换行
    sel = sel.trimmed();
    if (!sel.isEmpty()) {
        m_searchText = sel;
        m_entrySearch->setText(sel);
        m_searchIndex = -1;
        refreshView();   // 立即按新词高亮，不等 200ms 防抖
    } else {
        // 否则恢复上一次搜索词（关闭时记忆，可能为空字符串）
        m_entrySearch->setText(m_searchText);
    }
    m_entrySearch->setFocus();
    // 光标移到末尾（不选中），确保“保留上次值”可见
    m_entrySearch->setCursorPosition(m_entrySearch->text().length());
}

void SerialTool::hideSearchBar() {
    if (!m_searchBar || !m_searchBar->isVisible())
        return;
    m_searchText = m_entrySearch->text();   // 记录本次搜索词，供下次重开恢复
    if (m_historyPopup)
        m_historyPopup->hide();
    m_searchBar->hide();
    // 清空搜索状态：移除高亮、复位计数，避免残留遮住接收区
    m_searchHits.clear();
    m_searchIndex = -1;
    m_txtRecv->setExtraSelections(QList<QTextEdit::ExtraSelection>());
    m_entrySearch->blockSignals(true);
    m_entrySearch->clear();
    m_entrySearch->blockSignals(false);
    updateSearchCountLabel();
    m_txtRecv->setFocus();
}

void SerialTool::onRecvContextMenu(const QPoint& pos) {
    if (!m_txtRecv)
        return;
    // 各选项与顶部按钮行共享同一组控件，菜单只做"切换/唤出"，状态实时同步
    QMenu menu(this);
    menu.setObjectName(QStringLiteral("recvMenu"));

    auto* actClear = menu.addAction(QStringLiteral("清除窗口"));
    connect(actClear, &QAction::triggered, this, &SerialTool::clearRecv);
    auto* actSave = menu.addAction(QStringLiteral("保存txt"));
    connect(actSave, &QAction::triggered, this, &SerialTool::saveRecv);
    menu.addSeparator();

    // 复制：选中文字时复制选中，未选中时复制全部
    auto* actCopy = menu.addAction(QStringLiteral("复制"));
    connect(actCopy, &QAction::triggered, this, [this] {
        QString sel = m_txtRecv->textCursor().selectedText();
        if (!sel.isEmpty())
            QApplication::clipboard()->setText(sel);
        else
            QApplication::clipboard()->setText(m_txtRecv->toPlainText());
    });
    auto* actCopyAll = menu.addAction(QStringLiteral("全部复制"));
    connect(actCopyAll, &QAction::triggered, this, [this] {
        QApplication::clipboard()->setText(m_txtRecv->toPlainText());
    });
    menu.addSeparator();

    auto* actHex = menu.addAction(QStringLiteral("HEX显示"));
    actHex->setCheckable(true);
    actHex->setChecked(m_chkShowHex->isChecked());
    connect(actHex, &QAction::triggered, this, [this] {
        m_chkShowHex->setChecked(!m_chkShowHex->isChecked());
    });

    auto* actTs = menu.addAction(QStringLiteral("时间戳"));
    actTs->setCheckable(true);
    actTs->setChecked(m_chkShowTs->isChecked());
    connect(actTs, &QAction::triggered, this, [this] {
        m_chkShowTs->setChecked(!m_chkShowTs->isChecked());
    });

    auto* actPause = menu.addAction(QStringLiteral("暂停刷新"));
    actPause->setCheckable(true);
    actPause->setChecked(m_chkPause->isChecked());
    connect(actPause, &QAction::triggered, this, [this] {
        m_chkPause->setChecked(!m_chkPause->isChecked());
    });
    menu.addSeparator();

    // 编码修改：子菜单列出全部编码，勾选当前项
    auto* encMenu = menu.addMenu(QStringLiteral("编码修改"));
    const QString curEnc = m_cmbEncoding->currentText();
    for (const QString& enc : sjj::ENCODINGS) {
        auto* a = encMenu->addAction(enc);
        a->setCheckable(true);
        a->setChecked(enc == curEnc);
        connect(a, &QAction::triggered, this, [this, enc] {
            m_cmbEncoding->setCurrentText(enc);
        });
    }
    menu.addSeparator();

    auto* actFind = menu.addAction(QStringLiteral("查找\tCtrl+F"));
    connect(actFind, &QAction::triggered, this, &SerialTool::toggleSearchBar);

    menu.exec(m_txtRecv->viewport()->mapToGlobal(pos));
}

void SerialTool::onSearchChange() {
    // 防抖：停止输入 200ms 后再刷新
    m_searchIndex = -1;   // 搜索词变化，定位索引复位
    if (m_searchTimer)
        m_searchTimer->stop();
    m_searchTimer = new QTimer(this);
    m_searchTimer->setSingleShot(true);
    connect(m_searchTimer, &QTimer::timeout, this, &SerialTool::refreshView);
    m_searchTimer->start(200);
}

void SerialTool::onSearchOptionChanged() {
    // 大小写敏感(Cc) 与 正则(.*) 互斥：开启其一即关闭另一（blockSignals 防递归触发）
    auto* src = qobject_cast<QToolButton*>(sender());
    if (src == m_btnRegex && m_btnRegex->isChecked()) {
        m_btnCase->blockSignals(true);
        m_btnCase->setChecked(false);
        m_btnCase->blockSignals(false);
    } else if (src == m_btnCase && m_btnCase->isChecked()) {
        m_btnRegex->blockSignals(true);
        m_btnRegex->setChecked(false);
        m_btnRegex->blockSignals(false);
    }
    refreshView();
}

void SerialTool::commitSearchHistory() {
    const QString t = m_entrySearch->text().trimmed();
    if (t.isEmpty())
        return;
    // 回车时：不管是否匹配成功都记录
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    // 去重：移除同文本旧条目
    for (int i = m_searchHistory.size() - 1; i >= 0; --i) {
        if (m_searchHistory[i].text == t)
            m_searchHistory.removeAt(i);
    }
    m_searchHistory.prepend({t, now});
    const int cap = 30;
    while (m_searchHistory.size() > cap)
        m_searchHistory.removeLast();
    saveParams(true);
}

void SerialTool::onSearchFocusOut() {
    // 失焦时：仅匹配成功才记录
    if (!m_searchHits.isEmpty())
        commitSearchHistory();
}

void SerialTool::refreshHistoryPopup() {
    if (!m_historyPopup)
        return;
    const QString needle = m_entrySearch->text();
    m_historyPopup->clear();
    for (const auto& h : m_searchHistory) {
        if (needle.isEmpty() || h.text.contains(needle, Qt::CaseInsensitive))
            m_historyPopup->addItem(h.text);
    }
}

void SerialTool::showSearchHistoryPopup() {
    if (!m_historyPopup || m_searchHistory.isEmpty())
        return;
    // 点击箭头时展示全量历史（不过滤），输入时才实时过滤
    m_historyPopup->clear();
    for (const auto& h : m_searchHistory)
        m_historyPopup->addItem(h.text);
    if (m_historyPopup->count() == 0) {
        m_historyPopup->hide();
        return;
    }
    const QPoint pos = m_entrySearch->mapToGlobal(
        QPoint(0, m_entrySearch->height() + 2));
    m_historyPopup->setFixedWidth(m_entrySearch->width());
    const int hgt = qMin(m_historyPopup->sizeHintForRow(0) * m_historyPopup->count() + 8, 260);
    m_historyPopup->setFixedHeight(hgt);
    m_historyPopup->move(pos);
    m_historyPopup->show();
}

bool SerialTool::searchMatch(const QString& text) const {
    const QString needle = m_entrySearch->text();
    if (needle.isEmpty())
        return true;
    const bool useRegex = m_btnRegex->isChecked();
    const bool caseSensitive = m_btnCase->isChecked();
    if (useRegex) {
        QRegularExpression re(needle);
        if (!caseSensitive)
            re.setPatternOptions(QRegularExpression::CaseInsensitiveOption);
        if (!re.isValid())
            return false;
        return re.match(text).hasMatch();
    }
    // 普通模式：大小写组合
    const Qt::CaseSensitivity cs = caseSensitive ? Qt::CaseSensitive : Qt::CaseInsensitive;
    return text.contains(needle, cs);
}

void SerialTool::onRecvScroll(int value) {
    auto* sb = m_txtRecv->verticalScrollBar();
    // 程序性滚动（搜索刷新/批量插入/清空）不勾选
    if (m_programScroll) {
        m_lastScrollMax = sb->maximum();
        return;
    }
    const int maxv = sb->maximum();
    // 仅 range 收缩（裁剪删顶行引发 value clamp）属于程序性变化，跳过联动；
    // range 增长（数据追加）不影响用户滚动判断，保证暂停中手动滚到底能即时解除暂停
    if (maxv < m_lastScrollMax) {
        m_lastScrollMax = maxv;
        return;
    }
    m_lastScrollMax = maxv;
    const bool atBottom = value >= maxv;
    m_chkPause->setChecked(!atBottom);
    // 离开底部 → 显示"回到底部"悬浮按钮；滚回底部 → 隐藏
    if (m_btnScrollBottom) {
        if (atBottom) {
            m_btnScrollBottom->hide();
        } else {
            positionScrollBottomBtn();
            m_btnScrollBottom->show();
        }
    }
}

void SerialTool::scrollRecvToBottom() {
    ScrollGuard guard(m_programScroll);   // 程序性滚动：不触发"自动勾选暂停刷新"
    m_txtRecv->verticalScrollBar()->setValue(m_txtRecv->verticalScrollBar()->maximum());
    m_chkPause->setChecked(false);        // 取消勾选"暂停刷新"，恢复自动滚动
    if (m_btnScrollBottom)
        m_btnScrollBottom->hide();
}

void SerialTool::positionScrollBottomBtn() {
    if (!m_btnScrollBottom || !m_recvBox)
        return;
    auto* sb = m_txtRecv->verticalScrollBar();
    int sbw = sb->isVisible() ? sb->width() : 0;
    if (sbw <= 0)
        sbw = sb->sizeHint().width();
    if (sbw <= 0)
        sbw = 16;   // 兜底：Windows 默认滚动条宽度
    const int m = 4;
    const QRect tr = m_txtRecv->geometry();
    m_btnScrollBottom->move(tr.right() - sbw - m_btnScrollBottom->width() - m,
                            tr.bottom() - m_btnScrollBottom->height() - m);
    m_btnScrollBottom->raise();
}

void SerialTool::drainRx() {
    // 50ms 周期：把统一事件队列（rx/tx/sys 按发生顺序）刷新到接收区
    if (!m_pendingEvents.isEmpty()) {
        QVector<Record> recs = m_pendingEvents;
        m_pendingEvents.clear();
        m_records += recs;
        appendToView(recs);
    }
    // 状态栏节流 500ms
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    if (now - m_lastStatusTs >= 500) {
        m_lastStatusTs = now;
        const QString sr = QStringLiteral("S:%1  R:%2").arg(m_txBytes).arg(m_rxBytes);
        if (sr != m_lastSrText) {
            m_lblSr->setText(sr);
            m_lastSrText = sr;
        }
        if (m_ser && m_ser->isOpen()) {
            const auto sigs = m_ser->pinoutSignals();
            const QString hs = QStringLiteral("CTS=%1 DSR=%2 RLSD=%3")
                .arg(sigs & QSerialPort::ClearToSendSignal ? 1 : 0)
                .arg(sigs & QSerialPort::DataSetReadySignal ? 1 : 0)
                .arg(sigs & QSerialPort::DataCarrierDetectSignal ? 1 : 0);
            if (hs != m_lastHsText) {
                m_lblHandshake->setText(hs);
                m_lastHsText = hs;
            }
        } else if (m_lastHsText != QStringLiteral("CTS=0 DSR=0 RLSD=0")) {
            m_lblHandshake->setText(QStringLiteral("CTS=0 DSR=0 RLSD=0"));
            m_lastHsText = QStringLiteral("CTS=0 DSR=0 RLSD=0");
        }
    }
}

void SerialTool::clearRecv() {
    ScrollGuard guard(m_programScroll);
    m_txtRecv->clear();
    m_records.clear();
    m_pendingEvents.clear();   // 丢弃队列中尚未刷新的事件（否则清空后残留又会显示）
    // 清屏语义：待分包缓存一并丢弃，避免超时后残留数据又冒出来
    m_rxBuf.clear();
    m_pktStartTs.clear();
    if (m_pktTimer)
        m_pktTimer->stop();
    m_searchHits.clear();      // 文本已清空，搜索定位/计数一并复位
    m_searchIndex = -1;
    updateSearchCountLabel();
}

void SerialTool::saveRecv() {
    const QString path = QFileDialog::getSaveFileName(
        this, QStringLiteral("保存数据"), QString(),
        QStringLiteral("文本文件 (*.txt);;所有文件 (*.*)"));
    if (path.isEmpty())
        return;
    QFile f(path);
    if (!f.open(QIODevice::WriteOnly)) {
        QMessageBox::critical(this, QStringLiteral("保存失败"), f.errorString());
        return;
    }
    f.write(m_txtRecv->toPlainText().toUtf8());
    f.close();
    QMessageBox::information(this, QStringLiteral("保存"), QStringLiteral("已保存到\n%1").arg(path));
}

// ================= 校验 =================

QByteArray SerialTool::computeChecksumFor(const QByteArray& data,
                                          const QString& algorithm) const {
    return sjj::computeChecksum(data, algorithm);
}

QByteArray SerialTool::checksumRangeFor(const QByteArray& data) const {
    int start = m_entryCsStart->text().toInt();
    int endOff = m_entryCsEnd->text().toInt();
    if (start < 1)
        start = 1;
    if (endOff > 0)
        endOff = 0;
    return sjj::checksumRange(data, start, endOff);
}

void SerialTool::updateChecksumHighlight() {
    if (!m_csGroup)
        return;
    const bool on = m_cmbChecksum->currentText() != QStringLiteral("None");
    if (m_csGroup->property("cs_highlight").toBool() == on)
        return;
    m_csGroup->setProperty("cs_highlight", on);
    m_csGroup->style()->unpolish(m_csGroup);
    m_csGroup->style()->polish(m_csGroup);
}

QPair<QByteArray, QByteArray> SerialTool::applyChecksum(const QByteArray& data) {
    if (data.isEmpty()) {
        m_lblCsResult->clear();
        return {data, {}};
    }
    if (m_cmbChecksum->currentText() == QStringLiteral("None")) {
        m_lblCsResult->clear();
        return {data, {}};
    }
    const QByteArray head = checksumRangeFor(data);
    if (head.isEmpty()) {
        m_lblCsResult->clear();
        return {data, {}};
    }
    const QByteArray cs = computeChecksumFor(head, m_cmbChecksum->currentText());
    if (cs.isEmpty()) {
        m_lblCsResult->clear();
        return {data, {}};
    }
    m_lblCsResult->setText(sjj::spacedHex(cs));
    return {data + cs, cs};
}

void SerialTool::updateCsPreview() {
    // 防抖 100ms 后根据发送框内容实时预览校验结果
    if (m_csPreviewTimer)
        m_csPreviewTimer->stop();
    m_csPreviewTimer = new QTimer(this);
    m_csPreviewTimer->setSingleShot(true);
    connect(m_csPreviewTimer, &QTimer::timeout, this, &SerialTool::doCsPreview);
    m_csPreviewTimer->start(100);
}

void SerialTool::doCsPreview() {
    const QString raw = m_txtSend->toPlainText();
    if (raw.isEmpty()) {
        m_lblCsResult->clear();
        return;
    }
    bool ok = false;
    QString err;
    QByteArray data = encodeSendText(raw, ok, err);
    if (!ok) {
        m_lblCsResult->clear();
        return;
    }
    // 与发送一致：先加回车换行，再算校验（CRLF 参与校验计算）
    if (m_chkAddCrlf->isChecked())
        data += "\r\n";
    applyChecksum(data);
}

// ================= 发送 =================

QByteArray SerialTool::encodeSendText(const QString& raw, bool& ok, QString& err) const {
    if (m_chkSendHex->isChecked())
        return sjj::parseHexString(raw, ok, err);
    ok = true;
    return sjj::encodeString(raw, QStringLiteral("UTF-8"));
}

void SerialTool::send() {
    doSend(m_txtSend->toPlainText());
}

void SerialTool::doSend(const QString& raw) {
    if (!m_ser || !m_ser->isOpen()) {
        QMessageBox::warning(this, QStringLiteral("提示"), QStringLiteral("请先打开串口"));
        return;
    }
    if (raw.isEmpty())
        return;
    recordSendHistory(raw);
    bool ok = false;
    QString err;
    QByteArray data = encodeSendText(raw, ok, err);
    if (!ok) {
        QMessageBox::critical(this, QStringLiteral("HEX错误"), err);
        return;
    }
    // 先加回车换行，再算校验码（CRLF 参与校验计算，作为帧的一部分）
    if (m_chkAddCrlf->isChecked())
        data += "\r\n";
    const auto [out, cs] = applyChecksum(data);
    const qint64 n = m_ser->write(out);
    if (n < 0) {
        QMessageBox::critical(this, QStringLiteral("发送失败"), m_ser->errorString());
        return;
    }
    m_txBytes += quint64(n);
    echoSend(raw, out, cs);
    saveCurrentSendSlot();
}

void SerialTool::echoSend(const QString& raw, const QByteArray& data,
                          const QByteArray& csBytes) {
    addTxRecord(raw, data, csBytes, m_chkSendHex->isChecked());
}

void SerialTool::recordSendHistory(const QString& raw) {
    if (raw.isEmpty())
        return;
    m_sendHistory.removeAll(raw);
    m_sendHistory.prepend(raw);
    while (m_sendHistory.size() > 50)
        m_sendHistory.removeLast();
    refreshHistoryList();
}

void SerialTool::clearSend() {
    m_txtSend->clear();
}

void SerialTool::saveCurrentSendSlot() {
    const QString raw = m_txtSend->toPlainText();
    if (m_chkSendHex->isChecked())
        m_lastSendHex = raw;
    else
        m_lastSendText = raw;
}

void SerialTool::loadSendSlot() {
    const QString content = m_chkSendHex->isChecked() ? m_lastSendHex : m_lastSendText;
    m_txtSend->blockSignals(true);
    m_txtSend->setPlainText(content);
    m_txtSend->blockSignals(false);
    updateCsPreview();
}

void SerialTool::onSendHexToggle() {
    const bool newMode = m_chkSendHex->isChecked();
    const QString raw = m_txtSend->toPlainText();
    if (m_sendHexPrev)
        m_lastSendHex = raw;
    else
        m_lastSendText = raw;
    m_sendHexPrev = newMode;
    loadSendSlot();
}

// ================= 定时发送 =================

void SerialTool::onTimerToggle() {
    if (m_chkTimer->isChecked()) {
        bool ok = false;
        int interval = m_entryInterval->text().toInt(&ok);
        if (!ok || interval < 10)
            interval = 1000;
        m_entryInterval->setText(QString::number(interval));
        if (m_ser && m_ser->isOpen())
            m_timerSend->start(interval);
    } else {
        m_timerSend->stop();
    }
}

void SerialTool::timerSendTick() {
    if (!m_ser || !m_ser->isOpen()) {
        m_timerSend->stop();
        m_chkTimer->setChecked(false);
        return;
    }
    send();
}

// ================= 多字符串发送 =================

void SerialTool::refreshMsRows() {
    // 整体重建分组选项卡：确保改名 / 增删分组 / 命令状态一致
    // block currentChanged：removeTab 过程中 Qt 自动切 tab 会触发 currentChanged
    // 把 m_activeMsGroup 改成别的值，导致新建分组后定位错误。
    m_msTabs->blockSignals(true);
    while (m_msTabs->count() > 0) {
        QWidget* w = m_msTabs->widget(m_msTabs->count() - 1);
        m_msTabs->removeTab(m_msTabs->count() - 1);
        if (w)
            w->deleteLater();
    }

    if (m_activeMsGroup < 0 || m_activeMsGroup >= m_msGroups.size())
        m_activeMsGroup = m_msGroups.isEmpty() ? 0 : m_msGroups.size() - 1;

    for (int g = 0; g < m_msGroups.size(); ++g) {
        MsGroup& grp = m_msGroups[g];
        auto* page = new QWidget(m_msTabs);
        auto* lay = new QVBoxLayout(page);
        lay->setContentsMargins(4, 4, 4, 4);
        lay->setSpacing(4);

        // 分组下标不能按构建时的 g 捕获：拖动选项卡排序后页面控件会跟着移动，但捕获
        // 的旧下标不会变，改名 / 删组 / 加命令就会作用到别的分组（表现为「改名后对不上」）。
        // 统一按 page 在选项卡里的实时位置解析，tab 下标与 m_msGroups 下标始终同序。
        auto groupIndex = [this, page] { return m_msTabs->indexOf(page); };

        // ---- 分组头：组名 / 加命令 / 删组 ----
        auto* header = new QHBoxLayout;
        header->setSpacing(4);
        auto* nameEdit = new QLineEdit(grp.name);
        nameEdit->setPlaceholderText(QStringLiteral("分组名（如 测零 / 防坠）"));
        nameEdit->setMinimumWidth(80);
        connect(nameEdit, &QLineEdit::textChanged, this,
                [this, groupIndex](const QString& t) {
                    const int g = groupIndex();
                    if (g >= 0)
                        renameMsGroup(g, t);
                });
        connect(nameEdit, &QLineEdit::editingFinished, this, [this, groupIndex] {
            const int g = groupIndex();
            if (g >= 0)
                setActiveMsGroup(g);
        });
        header->addWidget(nameEdit, 1);
        auto* addCmd = new QPushButton(QStringLiteral("\uFF0B 命令"), page);
        addCmd->setFixedWidth(72);
        addCmd->setToolTip(QStringLiteral("在本组新增命令"));
        connect(addCmd, &QPushButton::clicked, this, [this, groupIndex] {
            const int g = groupIndex();
            if (g < 0)
                return;
            m_activeMsGroup = g;
            newMsEntry();
        });
        header->addWidget(addCmd);
        auto* delGrp = new QPushButton(QStringLiteral("\u2715 删组"), page);
        delGrp->setFixedWidth(60);
        delGrp->setToolTip(QStringLiteral("删除该分组"));
        connect(delGrp, &QPushButton::clicked, this, [this, groupIndex] {
            const int g = groupIndex();
            if (g >= 0)
                deleteMsGroup(g);
        });
        header->addWidget(delGrp);
        lay->addLayout(header);

        // ---- 组内命令 ----
        auto* scroll = new QScrollArea(page);
        scroll->setWidgetResizable(true);
        auto* inner = new QWidget;
        auto* rows = new QVBoxLayout(inner);
        rows->setContentsMargins(2, 2, 2, 2);
        rows->setSpacing(2);
        rows->addStretch(1);
        for (int i = 0; i < grp.entries.size(); ++i) {
            const int ei = i;
            auto* row = new QHBoxLayout;
            row->setSpacing(2);
            auto* chk = new ThemeCheckBox(QStringLiteral("HEX"), page);
            chk->setThemeColors(m_themeC);
            chk->setChecked(grp.entries[i].hex);
            connect(chk, &QCheckBox::toggled, this, [this, groupIndex, ei](bool v) {
                const int g = groupIndex();
                if (g >= 0)
                    msSetHex(g, ei, v);
            });
            row->addWidget(chk);
            auto* ent = new QLineEdit(grp.entries[i].content);
            connect(ent, &QLineEdit::textChanged, this, [this, groupIndex, ei](const QString& t) {
                const int g = groupIndex();
                if (g >= 0)
                    msSetContent(g, ei, t);
            });
            row->addWidget(ent, 1);
            auto* btn = new QPushButton(grp.entries[i].label);
            btn->setFixedWidth(96);
            connect(btn, &QPushButton::clicked, this, [this, groupIndex, ei] {
                const int g = groupIndex();
                if (g >= 0)
                    sendMsEntry(g, ei);
            });
            btn->setContextMenuPolicy(Qt::CustomContextMenu);
            connect(btn, &QWidget::customContextMenuRequested, this,
                    [this, groupIndex, ei](const QPoint&) {
                        const int g = groupIndex();
                        if (g >= 0)
                            renameMsEntry(g, ei);
                    });
            row->addWidget(btn);
            auto* xbtn = new QPushButton(QStringLiteral("\u2715"), page);
            xbtn->setFixedWidth(28);
            connect(xbtn, &QPushButton::clicked, this, [this, groupIndex, ei] {
                const int g = groupIndex();
                if (g >= 0)
                    deleteMsEntry(g, ei);
            });
            row->addWidget(xbtn);
            rows->insertLayout(rows->count() - 1, row);
        }
        scroll->setWidget(inner);
        lay->addWidget(scroll, 1);

        m_msTabs->addTab(page, grp.name);
    }
    // 选中当前活动分组选项卡，并刷新活动组高亮
    if (m_msGroups.isEmpty())
        m_msTabs->setCurrentIndex(0);
    else
        m_msTabs->setCurrentIndex(m_activeMsGroup);
    applyMsActiveStyle();
    // 恢复信号，手动触发一次 currentChanged 同步状态
    m_msTabs->blockSignals(false);
}

void SerialTool::applyMsActiveStyle() {
    QTabBar* bar = m_msTabs->tabBar();
    if (!bar)
        return;
    const QString accent = m_themeC.value(QStringLiteral("accent"));
    const QString normal = m_themeC.value(QStringLiteral("text_secondary"));
    const QColor accentCol(accent);
    const QColor normalCol(normal);
    for (int i = 0; i < m_msTabs->count(); ++i) {
        const bool active = (i == m_activeMsGroup);
        // 活动分组选项卡：主题强调色文字；非活动：次级文字色
        bar->setTabTextColor(i, active ? accentCol : normalCol);
    }
}

void SerialTool::cycleMsTab(int dir) {
    // 选项卡按顺序切换
    const int n = m_msTabs->count();
    if (n <= 1)
        return;
    int idx = m_msTabs->currentIndex() + dir;
    if (idx < 0)
        idx = n - 1;          // 循环到末尾
    else if (idx >= n)
        idx = 0;              // 循环到开头
    m_msTabs->setCurrentIndex(idx);
}

void SerialTool::hideTabPopup() {
    if (!m_tabPopup)
        return;
    m_tabPopup->close();
    m_tabPopup = nullptr;
    // 记录收起时刻：紧随其后的这次点击（press 收起 → release 触发 clicked）不再重新弹出
    m_tabPopupClosedAt = QDateTime::currentMSecsSinceEpoch();
    if (m_tabDropdownBtn) {
        m_tabDropdownBtn->setChecked(false);
        m_tabDropdownBtn->setDown(false);
        m_tabDropdownBtn->setAttribute(Qt::WA_UnderMouse, false);
        m_tabDropdownBtn->update();
    }
}

void SerialTool::showTabDropdown() {
    // 浮层刚因「点外部/再点按钮」被收起时，这次点击只作收起用，不能再立刻弹出一次
    const qint64 nowMs = QDateTime::currentMSecsSinceEpoch();
    if (m_tabPopupClosedAt && nowMs - m_tabPopupClosedAt < 250) {
        m_tabPopupClosedAt = 0;
        m_tabDropdownBtn->setChecked(false);
        return;
    }

    // 再次点击：已经展开则收起（按钮高亮同步复位）
    if (m_tabPopup) {
        hideTabPopup();
        return;
    }

    const int n = m_msTabs->count();
    if (n == 0) {
        m_tabDropdownBtn->setChecked(false);
        return;
    }

    QStringList items;
    items.reserve(n);
    for (int i = 0; i < n; ++i) {
        QString label = m_msTabs->tabText(i);
        if (label.isEmpty())
            label = QStringLiteral("Tab %1").arg(i);
        items << label;
    }

    auto* pop = new ListPopup(items, m_msTabs->currentIndex(), m_themeC, this);
    pop->onPick = [this](int i) {
        if (i >= 0 && i < m_msTabs->count())
            m_msTabs->setCurrentIndex(i);
    };
    m_tabPopup = pop;
    m_tabDropdownBtn->setChecked(true);
    // 浮层关闭（选中/点外部/再次点按钮）后复位按钮，避免高亮或按下态残留
    connect(pop, &QObject::destroyed, this, [this] {
        m_tabPopup = nullptr;
        if (m_tabDropdownBtn) {
            m_tabDropdownBtn->setChecked(false);
            m_tabDropdownBtn->setDown(false);
            m_tabDropdownBtn->setAttribute(Qt::WA_UnderMouse, false);
            m_tabDropdownBtn->update();
        }
    });
    pop->adjustSize();

    // 浮层整体（含四周留白）落在按钮左侧，绝不压住按钮，否则点按钮会落在浮层窗口上
    const QPoint gp = m_tabDropdownBtn->mapToGlobal(QPoint(0, 0));
    int px = gp.x() - pop->width();
    int py = gp.y() - ListPopup::kPad;
    if (QScreen* s = screen() ? screen() : QGuiApplication::primaryScreen()) {
        const QRect a = s->availableGeometry();
        px = qBound(a.left() - ListPopup::kPad, px,
                    a.right() + 1 - pop->width());
        py = qBound(a.top() - ListPopup::kPad, py,
                    a.bottom() + 1 - pop->height() + ListPopup::kPad);
    }
    pop->move(px, py);
    pop->show();
}

void SerialTool::setActiveMsGroup(int g) {
    if (g < 0 || g >= m_msGroups.size())
        return;
    m_activeMsGroup = g;
    if (g >= 0 && g < m_msTabs->count())
        m_msTabs->setCurrentIndex(g);
    applyMsActiveStyle();
}

void SerialTool::newMsGroup() {
    MsGroup g;
    g.name = QStringLiteral("分组%1").arg(m_msGroups.size() + 1);
    m_msGroups.append(g);
    m_activeMsGroup = m_msGroups.size() - 1;
    refreshMsRows();
    saveParams(true);
}

void SerialTool::deleteMsGroup(int g) {
    if (g < 0 || g >= m_msGroups.size())
        return;
    const int n = m_msGroups[g].entries.size();
    if (n == 0 || QMessageBox::question(this, QStringLiteral("删除分组"),
            QStringLiteral("确定删除分组「%1」及其 %2 条命令？")
                .arg(m_msGroups[g].name, QString::number(n)),
            QMessageBox::Yes | QMessageBox::No) == QMessageBox::Yes) {
        m_msGroups.removeAt(g);
        if (m_activeMsGroup >= m_msGroups.size())
            m_activeMsGroup = m_msGroups.isEmpty() ? 0 : m_msGroups.size() - 1;
        refreshMsRows();
        saveParams(true);
    }
}

void SerialTool::renameMsGroup(int g, const QString& name) {
    if (g < 0 || g >= m_msGroups.size())
        return;
    m_msGroups[g].name = name;
    // 实时更新对应选项卡标题（不重建，避免输入框失焦）
    if (g >= 0 && g < m_msTabs->count())
        m_msTabs->setTabText(g, name.isEmpty() ? QStringLiteral("分组") : name);
    scheduleQuickCmdSave();   // 防抖保存（不重建，避免输入框失焦）
}

void SerialTool::newMsEntry() {
    if (m_msGroups.isEmpty()) {
        MsGroup g;
        g.name = QStringLiteral("分组1");
        m_msGroups.append(g);
    }
    if (m_activeMsGroup < 0 || m_activeMsGroup >= m_msGroups.size())
        m_activeMsGroup = m_msGroups.size() - 1;
    const int g = m_activeMsGroup;
    MsEntry e;
    e.label = QStringLiteral("发送%1").arg(m_msGroups[g].entries.size() + 1);
    m_msGroups[g].entries.append(e);
    refreshMsRows();
    saveParams(true);
}

void SerialTool::msSetHex(int g, int i, bool val) {
    if (g >= 0 && g < m_msGroups.size() && i >= 0 && i < m_msGroups[g].entries.size()) {
        m_msGroups[g].entries[i].hex = val;
        saveParams(true);   // 快捷命令变化 → 立即保存（触发其他实例同步）
    }
}

void SerialTool::msSetContent(int g, int i, const QString& t) {
    if (g >= 0 && g < m_msGroups.size() && i >= 0 && i < m_msGroups[g].entries.size()) {
        m_msGroups[g].entries[i].content = t;
        scheduleQuickCmdSave();   // 内容编辑防抖保存（300ms）
    }
}

// 快捷命令内容编辑防抖保存
void SerialTool::scheduleQuickCmdSave() {
    if (m_cfgSaveTimer)
        m_cfgSaveTimer->start(300);
}

// 配置文件被其他实例修改：仅同步快捷命令（ms_groups），其他配置不动
void SerialTool::onConfigFileChanged(const QString&) {
    QFile f(configPath());
    if (!f.open(QIODevice::ReadOnly))
        return;
    const QJsonDocument doc = QJsonDocument::fromJson(f.readAll());
    f.close();
    if (!doc.isObject())
        return;
    const QJsonArray arr = doc.object().value(QStringLiteral("ms_groups")).toArray();
    // 与当前内容比较：相同（自身保存触发/未变化）则跳过，避免无谓刷新与回环
    if (QJsonDocument(arr).toJson(QJsonDocument::Compact)
        == QJsonDocument(msGroupsToData()).toJson(QJsonDocument::Compact))
        return;
    // 应用文件中的快捷命令（分组）
    m_msGroups = msGroupsFromArray(arr);
    if (m_activeMsGroup >= m_msGroups.size())
        m_activeMsGroup = m_msGroups.isEmpty() ? 0 : m_msGroups.size() - 1;
    refreshMsRows();
}

QVector<MsGroup> SerialTool::msGroupsFromArray(const QJsonArray& arr) const {
    QVector<MsGroup> groups;
    for (const QJsonValue& gv : arr) {
        const QJsonObject go = gv.toObject();
        MsGroup grp;
        grp.name = go.value(QStringLiteral("name")).toString(QStringLiteral("分组"));
        if (grp.name.isEmpty())
            grp.name = QStringLiteral("分组");
        grp.collapsed = go.value(QStringLiteral("collapsed")).toBool(false);
        const QJsonArray ea = go.value(QStringLiteral("entries")).toArray();
        for (const QJsonValue& ev : ea) {
            const QJsonObject eo = ev.toObject();
            MsEntry me;
            me.hex = eo.value(QStringLiteral("hex")).toBool(false);
            me.content = eo.value(QStringLiteral("content")).toString();
            me.label = eo.value(QStringLiteral("label")).toString(QStringLiteral("发送"));
            if (me.label.isEmpty())
                me.label = QStringLiteral("发送");
            grp.entries.append(me);
        }
        groups.append(grp);
    }
    return groups;
}

void SerialTool::deleteMsEntry(int g, int i) {
    if (g >= 0 && g < m_msGroups.size() && i >= 0 && i < m_msGroups[g].entries.size()) {
        m_msGroups[g].entries.removeAt(i);
        refreshMsRows();
        saveParams(true);   // 同步到其他实例
    }
}

void SerialTool::renameMsEntry(int g, int i) {
    if (g < 0 || g >= m_msGroups.size() || i < 0 || i >= m_msGroups[g].entries.size())
        return;
    bool ok = false;
    const QString newLabel = QInputDialog::getText(
        this, QStringLiteral("修改按钮文字"),
        QStringLiteral("请输入第 %1 组第 %2 行的按钮显示文字：").arg(g + 1).arg(i + 1),
        QLineEdit::Normal, m_msGroups[g].entries[i].label, &ok);
    if (ok && !newLabel.trimmed().isEmpty()) {
        m_msGroups[g].entries[i].label = newLabel.trimmed();
        refreshMsRows();
        saveParams(true);   // 同步到其他实例
    }
}

void SerialTool::sendMsEntry(int g, int i) {
    if (!m_ser || !m_ser->isOpen()) {
        QMessageBox::warning(this, QStringLiteral("提示"), QStringLiteral("请先打开串口"));
        return;
    }
    if (g < 0 || g >= m_msGroups.size() || i < 0 || i >= m_msGroups[g].entries.size())
        return;
    m_activeMsGroup = g;
    applyMsActiveStyle();
    const MsEntry& e = m_msGroups[g].entries[i];
    const QString raw = e.content;
    if (raw.isEmpty())
        return;
    QByteArray data;
    if (e.hex) {
        bool ok = false;
        QString err;
        data = sjj::parseHexString(raw, ok, err);
        if (!ok) {
            QMessageBox::critical(this, QStringLiteral("HEX错误"),
                                  QStringLiteral("第 %1 组第 %2 行 %3").arg(g + 1).arg(i + 1).arg(err));
            return;
        }
    } else {
        data = sjj::encodeString(raw, QStringLiteral("UTF-8"));
    }
    // 先加回车换行，再算校验码（CRLF 参与校验计算，作为帧的一部分）
    if (m_chkAddCrlf->isChecked())
        data += "\r\n";
    const auto [out, cs] = applyChecksum(data);
    const qint64 n = m_ser->write(out);
    if (n < 0) {
        QMessageBox::critical(this, QStringLiteral("发送失败"), m_ser->errorString());
        return;
    }
    m_txBytes += quint64(n);
    addTxRecord(raw, out, cs, e.hex);
    recordSendHistory(raw);
}

void SerialTool::startMsLoop() {
    if (!m_ser || !m_ser->isOpen()) {
        QMessageBox::warning(this, QStringLiteral("提示"), QStringLiteral("请先打开串口"));
        return;
    }
    if (m_msGroups.isEmpty()) {
        QMessageBox::information(this, QStringLiteral("提示"),
                                 QStringLiteral("还没有分组，请先新建分组与命令"));
        return;
    }
    if (m_activeMsGroup < 0 || m_activeMsGroup >= m_msGroups.size())
        m_activeMsGroup = 0;
    if (m_msGroups[m_activeMsGroup].entries.isEmpty()) {
        QMessageBox::information(this, QStringLiteral("提示"),
            QStringLiteral("当前分组「%1」没有命令，请先新建")
                .arg(m_msGroups[m_activeMsGroup].name));
        return;
    }
    bool ok = false;
    int interval = m_entryMsInterval->text().toInt(&ok);
    if (!ok || interval < 10)
        interval = 1000;
    m_entryMsInterval->setText(QString::number(interval));
    m_msLooping = true;
    m_msLoopGroup = m_activeMsGroup;
    m_msLoopIndex = 0;
    m_btnStopMs->setEnabled(true);
    m_msLoopTimer->start(interval);
}

void SerialTool::msLoopTick() {
    if (!m_msLooping) {
        m_msLoopTimer->stop();
        return;
    }
    if (!m_ser || !m_ser->isOpen()) {
        stopMsLoop();
        return;
    }
    if (m_msLoopGroup < 0 || m_msLoopGroup >= m_msGroups.size()) {
        stopMsLoop();
        return;
    }
    const QVector<MsEntry>& es = m_msGroups[m_msLoopGroup].entries;
    if (m_msLoopIndex >= es.size()) {
        if (m_chkMsLoop->isChecked())
            m_msLoopIndex = 0;
        else {
            stopMsLoop();
            return;
        }
    }
    sendMsEntry(m_msLoopGroup, m_msLoopIndex);
    ++m_msLoopIndex;
}

void SerialTool::stopMsLoop() {
    m_msLooping = false;
    m_msLoopTimer->stop();
    m_btnStopMs->setEnabled(false);
}

// ================= 文件发送 =================

void SerialTool::pickFile() {
    const QString path = QFileDialog::getOpenFileName(this, QStringLiteral("选择文件"),
                                                      QString(),
                                                      QStringLiteral("所有文件 (*.*)"));
    if (!path.isEmpty()) {
        m_btnSelectFile->setText(QFileInfo(path).fileName());
        m_btnSelectFile->setToolTip(path);
        m_selectedFilePath = path;
    }
}

void SerialTool::sendFile() {
    if (m_fileSending) {
        stopSendFile();
        return;
    }
    if (!m_ser || !m_ser->isOpen()) {
        QMessageBox::warning(this, QStringLiteral("提示"), QStringLiteral("请先打开串口"));
        return;
    }
    if (m_selectedFilePath.isEmpty() || !QFileInfo::exists(m_selectedFilePath)) {
        QMessageBox::warning(this, QStringLiteral("提示"),
                             QStringLiteral("请先点击「选择文件」选择要发送的文件"));
        return;
    }
    QFile f(m_selectedFilePath);
    if (!f.open(QIODevice::ReadOnly)) {
        QMessageBox::critical(this, QStringLiteral("读取失败"), f.errorString());
        return;
    }
    QByteArray data = f.readAll();
    f.close();
    // 先加回车换行（文本文件），再算校验码（.bin/.hex 不加 CRLF）
    const QString lower = m_selectedFilePath.toLower();
    if (m_chkAddCrlf->isChecked() && !lower.endsWith(QStringLiteral(".bin"))
        && !lower.endsWith(QStringLiteral(".hex")))
        data += "\r\n";
    m_fileData = applyChecksum(data).first;
    m_fileOffset = 0;
    m_fileWaitingWrite = false;
    m_fileSending = true;
    m_btnSendFile->setText(QStringLiteral("停止发送"));
    m_btnSendFile->setObjectName(QStringLiteral("btnStopSend"));
    m_btnSendFile->style()->unpolish(m_btnSendFile);
    m_btnSendFile->style()->polish(m_btnSendFile);
    addRecord(QStringLiteral("sys"),
              QStringLiteral("开始发送文件: %1 (%2 字节)")
                  .arg(QFileInfo(m_selectedFilePath).fileName()).arg(m_fileData.size()));
    QTimer::singleShot(0, this, &SerialTool::sendFileChunk);
}

// 文件发送由 QSerialPort::bytesWritten 信号驱动：一块数据真正从串口发出后
// 才写入下一块。完成判定精确（最后一块的 bytesWritten 到达 = 全部发出），
// 且天然限流，不会瞬间灌爆内部缓冲。不用 clear()（该 API 中止异步写后
// Qt 内部写状态不恢复，会导致后续 write 只进缓冲、永远发不出去）。
void SerialTool::sendFileChunk() {
    if (!m_fileSending) {
        endSendFile();
        return;
    }
    if (!m_ser || !m_ser->isOpen()) {
        endSendFile();
        return;
    }
    if (m_fileWaitingWrite)
        return;   // 上一块仍在等待 bytesWritten 确认
    const QByteArray chunk = m_fileData.mid(m_fileOffset, 512);
    if (chunk.isEmpty()) {
        // 所有块均已 write 且已被 bytesWritten 确认发出 → 真正完成
        endSendFile();
        addRecord(QStringLiteral("sys"), QStringLiteral("文件发送完成"));
        return;
    }
    const qint64 n = m_ser->write(chunk);
    if (n < 0) {
        QMessageBox::critical(this, QStringLiteral("发送失败"), m_ser->errorString());
        endSendFile();
        return;
    }
    if (n == 0) {
        // 内部缓冲满未写入：稍后重试同一块（默认缓冲无限，正常不会走到）
        QTimer::singleShot(2, this, &SerialTool::sendFileChunk);
        return;
    }
    m_txBytes += quint64(n);
    m_fileOffset += n;
    m_fileWaitingWrite = true;   // 等 bytesWritten 信号再发下一块
}

void SerialTool::onFileBytesWritten(qint64) {
    // bytesWritten 对一切 write 都触发（含手动/定时发送），仅文件发送期间推进
    if (!m_fileSending)
        return;
    m_fileWaitingWrite = false;
    sendFileChunk();
}

void SerialTool::stopSendFile() {
    m_fileSending = false;
    endSendFile();
    addRecord(QStringLiteral("sys"), QStringLiteral("文件发送已停止"));
}

void SerialTool::endSendFile() {
    m_fileSending = false;
    m_fileWaitingWrite = false;
    m_fileData.clear();
    m_fileOffset = 0;
    m_btnSendFile->setText(QStringLiteral("发送文件"));
    m_btnSendFile->setObjectName(QStringLiteral("btnSendFile"));
    m_btnSendFile->style()->unpolish(m_btnSendFile);
    m_btnSendFile->style()->polish(m_btnSendFile);
}

// ================= 历史记录 =================

void SerialTool::refreshHistoryList() {
    if (!m_histList)
        return;   // 历史记录选项卡已移除，仅保留数据记录
    m_histList->clear();
    for (const QString& item : m_sendHistory) {
        QString disp = item;
        disp.replace(QLatin1Char('\n'), QStringLiteral("\u23CE "));
        if (disp.size() > 80)
            disp = disp.left(80) + QStringLiteral("\u2026");
        m_histList->addItem(disp);
    }
}

void SerialTool::histLoad(QListWidgetItem* item) {
    const int idx = m_histList->row(item);
    if (idx >= 0 && idx < m_sendHistory.size())
        m_txtSend->setPlainText(m_sendHistory[idx]);
}

void SerialTool::histDelete() {
    QVector<int> rows;
    const auto sels = m_histList->selectedItems();
    for (auto* it : sels)
        rows.append(m_histList->row(it));
    std::sort(rows.begin(), rows.end(), std::greater<int>());
    for (int i : rows) {
        if (i >= 0 && i < m_sendHistory.size())
            m_sendHistory.removeAt(i);
    }
    refreshHistoryList();
}

void SerialTool::histClear() {
    if (m_sendHistory.isEmpty())
        return;
    if (QMessageBox::question(this, QStringLiteral("确认"),
                              QStringLiteral("清空全部发送历史？")) == QMessageBox::Yes) {
        m_sendHistory.clear();
        refreshHistoryList();
    }
}

// ================= 面板切换 =================

void SerialTool::toggleWindow(int tabIndex) {
    if (m_msPanel->isVisible()) {
        if (m_msTabs->currentIndex() == tabIndex) {
            m_msPanel->hide();
            return;
        }
        m_msTabs->setCurrentIndex(tabIndex);
    } else {
        m_msPanel->show();
        if (tabIndex < 0 || tabIndex >= m_msTabs->count())
            tabIndex = 0;
        m_msTabs->setCurrentIndex(tabIndex);
    }
}

// ================= GitHub =================

void SerialTool::openGithub() {
    QDesktopServices::openUrl(QUrl(sjj::GITHUB_URL));
}

// ================= 配置 =================

QString SerialTool::configPath() const {
    // 配置文件跟随 exe 名：实时解析当前 exe 的真实文件名（不含扩展名），
    // 获取失败时回退到 "SuperCOM"，保证任何情况下都有确定的配置文件。
    QString name = QFileInfo(QCoreApplication::applicationFilePath()).completeBaseName();
    if (name.isEmpty())
        name = QStringLiteral("SuperCOM");
    return QCoreApplication::applicationDirPath() + QStringLiteral("/")
           + name + QStringLiteral("_config.json");
}

QJsonArray SerialTool::msGroupsToData() const {
    QJsonArray arr;
    for (const auto& grp : m_msGroups) {
        QJsonObject go;
        go[QStringLiteral("name")] = grp.name;
        go[QStringLiteral("collapsed")] = grp.collapsed;
        QJsonArray ea;
        for (const auto& e : grp.entries) {
            QJsonObject eo;
            eo[QStringLiteral("hex")] = e.hex;
            eo[QStringLiteral("content")] = e.content;
            eo[QStringLiteral("label")] = e.label;
            ea.append(eo);
        }
        go[QStringLiteral("entries")] = ea;
        arr.append(go);
    }
    return arr;
}

void SerialTool::saveParams(bool silent) {
    saveCurrentSendSlot();
    QJsonObject o;
    o[QStringLiteral("port")] = getSelectedPort();
    o[QStringLiteral("baud")] = m_cmbBaud->currentText();
    o[QStringLiteral("databits")] = m_databits;
    o[QStringLiteral("stopbits")] = m_stopbits;
    o[QStringLiteral("parity")] = m_parity;
    o[QStringLiteral("flow")] = m_flow;
    o[QStringLiteral("read_timeout_ms")] = m_readTimeout;
    o[QStringLiteral("split_by_time")] = m_splitByTime;
    o[QStringLiteral("split_by_char")] = m_splitByChar;
    o[QStringLiteral("split_pattern")] = m_splitPatternText;
    o[QStringLiteral("show_hex")] = m_chkShowHex->isChecked();
    o[QStringLiteral("send_hex")] = m_chkSendHex->isChecked();
    o[QStringLiteral("add_crlf")] = m_chkAddCrlf->isChecked();
    o[QStringLiteral("interval_ms")] = m_entryInterval->text();
    o[QStringLiteral("show_ts")] = m_chkShowTs->isChecked();
    o[QStringLiteral("ms_groups")] = msGroupsToData();
    o[QStringLiteral("send_history")] = QJsonArray::fromStringList(m_sendHistory);
    // 搜索历史：带时间戳，按时间倒序
    {
        QJsonArray shArr;
        for (const auto& h : m_searchHistory) {
            QJsonObject ho;
            ho[QStringLiteral("text")] = h.text;
            ho[QStringLiteral("ts")] = h.timestamp;
            shArr.append(ho);
        }
        o[QStringLiteral("search_history")] = shArr;
    }
    o[QStringLiteral("search_text")] = m_searchText;
    o[QStringLiteral("checksum")] = m_cmbChecksum->currentText();
    o[QStringLiteral("cs_start")] =
        m_entryCsStart->text().trimmed().isEmpty() ? QStringLiteral("1")
                                                   : m_entryCsStart->text().trimmed();
    o[QStringLiteral("cs_end")] =
        m_entryCsEnd->text().trimmed().isEmpty() ? QStringLiteral("0")
                                                 : m_entryCsEnd->text().trimmed();
    o[QStringLiteral("encoding")] = m_cmbEncoding->currentText();
    o[QStringLiteral("last_send_text")] = m_lastSendText;
    o[QStringLiteral("last_send_hex")] = m_lastSendHex;
    o[QStringLiteral("window_w")] = width();
    o[QStringLiteral("window_h")] = height();
    o[QStringLiteral("panel_visible")] = !m_msTabs->isHidden();
    o[QStringLiteral("panel_tab")] = m_msTabs->currentIndex();
    o[QStringLiteral("theme")] = m_theme;
    o[QStringLiteral("ignore_update_version")] = m_ignoreUpdateVersion;
    QFile f(configPath());
    if (!f.open(QIODevice::WriteOnly)) {
        if (!silent)
            QMessageBox::critical(this, QStringLiteral("保存失败"), f.errorString());
        return;
    }
    f.write(QJsonDocument(o).toJson(QJsonDocument::Indented));
    f.close();
}

void SerialTool::loadParams(bool silent) {
    m_suppressApply = true;   // 配置加载期间不触发"自动重开"
    loadParamsInner(silent);
    m_suppressApply = false;
}

void SerialTool::loadParamsInner(bool silent) {
    QFile f(configPath());
    if (!f.exists())
        return;
    if (!f.open(QIODevice::ReadOnly)) {
        if (!silent)
            QMessageBox::critical(this, QStringLiteral("加载失败"), f.errorString());
        return;
    }
    const QJsonDocument doc = QJsonDocument::fromJson(f.readAll());
    f.close();
    if (!doc.isObject())
        return;
    const QJsonObject o = doc.object();

    // 主题优先应用（其余控件样式依赖全局 QSS）
    QString themeName = o.value(QStringLiteral("theme")).toString(QStringLiteral("light"));
    if (themeName != QStringLiteral("dark"))
        themeName = QStringLiteral("light");
    applyTheme(themeName);
    m_ignoreUpdateVersion = o.value(QStringLiteral("ignore_update_version")).toString();
    m_cmbBaud->setCurrentText(o.value(QStringLiteral("baud")).toString(QStringLiteral("115200")));
    m_databits = o.value(QStringLiteral("databits")).toString(QStringLiteral("8"));
    m_stopbits = o.value(QStringLiteral("stopbits")).toString(QStringLiteral("1"));
    m_parity = o.value(QStringLiteral("parity")).toString(QStringLiteral("None"));
    m_flow = o.value(QStringLiteral("flow")).toString(QStringLiteral("None"));
    m_readTimeout = o.value(QStringLiteral("read_timeout_ms")).toString(QStringLiteral("50"));
    // 接收分包：默认只按空闲超时；两种可同时启用
    m_splitByTime = o.value(QStringLiteral("split_by_time")).toBool(true);
    m_splitByChar = o.value(QStringLiteral("split_by_char")).toBool(false);
    {
        QString err;
        if (!updateSplitPattern(o.value(QStringLiteral("split_pattern"))
                                    .toString(QStringLiteral("\\r\\n")), err)) {
            // 配置里的分隔符非法：回退到默认 \r\n 并关掉按字符分包
            QString defErr;
            updateSplitPattern(QStringLiteral("\\r\\n"), defErr);
            m_splitByChar = false;
        }
    }
    m_chkShowHex->setChecked(o.value(QStringLiteral("show_hex")).toBool(false));
    m_chkSendHex->setChecked(o.value(QStringLiteral("send_hex")).toBool(false));
    m_chkAddCrlf->setChecked(o.value(QStringLiteral("add_crlf")).toBool(false));
    m_entryInterval->setText(o.value(QStringLiteral("interval_ms")).toString(QStringLiteral("1000")));
    m_chkShowTs->setChecked(o.value(QStringLiteral("show_ts")).toBool(true));
    // 端口匹配
    const QString target = o.value(QStringLiteral("port")).toString();
    for (auto it = m_portMap.cbegin(); it != m_portMap.cend(); ++it) {
        if (it.value() == target) {
            m_cmbPort->setCurrentText(it.key());
            break;
        }
    }
    // 多字符串面板（分组）：优先读 ms_groups；无则把旧 ms_entries / quick_cmds
    // 迁移进一个默认分组，保证老配置不丢
    m_msGroups.clear();
    QJsonArray grpArr = o.value(QStringLiteral("ms_groups")).toArray();
    if (!grpArr.isEmpty()) {
        m_msGroups = msGroupsFromArray(grpArr);
    } else {
        // ---- 旧字段迁移 ----
        QJsonArray msArr = o.value(QStringLiteral("ms_entries")).toArray();
        if (msArr.isEmpty()) {
            QStringList seen;
            const QJsonArray quick = o.value(QStringLiteral("quick_cmds")).toArray();
            const QJsonArray hist = o.value(QStringLiteral("send_history")).toArray();
            if (!quick.isEmpty() || !hist.isEmpty()) {
                for (const QJsonValue& v : quick) {
                    const QString s = v.toString();
                    if (!s.isEmpty() && !seen.contains(s))
                        seen.append(s);
                }
                for (const QJsonValue& v : hist) {
                    const QString s = v.toString();
                    if (!s.isEmpty() && !seen.contains(s))
                        seen.append(s);
                }
                for (const QString& s : seen) {
                    QJsonObject e;
                    e[QStringLiteral("hex")] = false;
                    e[QStringLiteral("content")] = s;
                    QString label = s.left(8);
                    label.replace(QLatin1Char('\n'), QLatin1Char(' '));
                    e[QStringLiteral("label")] = label.isEmpty() ? QStringLiteral("导入") : label;
                    msArr.append(e);
                }
            }
        }
        if (!msArr.isEmpty()) {
            MsGroup def;
            def.name = QStringLiteral("默认");
            for (const QJsonValue& v : msArr) {
                const QJsonObject e = v.toObject();
                MsEntry me;
                me.hex = e.value(QStringLiteral("hex")).toBool(false);
                me.content = e.value(QStringLiteral("content")).toString();
                me.label = e.value(QStringLiteral("label")).toString(QStringLiteral("发送"));
                if (me.label.isEmpty())
                    me.label = QStringLiteral("发送");
                def.entries.append(me);
            }
            m_msGroups.append(def);
        }
    }
    if (m_activeMsGroup >= m_msGroups.size())
        m_activeMsGroup = m_msGroups.isEmpty() ? 0 : m_msGroups.size() - 1;
    refreshMsRows();
    m_sendHistory.clear();
    const QJsonArray histArr = o.value(QStringLiteral("send_history")).toArray();
    for (const QJsonValue& v : histArr)
        m_sendHistory.append(v.toString());
    while (m_sendHistory.size() > 50)
        m_sendHistory.removeLast();
    refreshHistoryList();
    // 搜索历史（带时间戳）
    m_searchHistory.clear();
    const QJsonArray shArr = o.value(QStringLiteral("search_history")).toArray();
    for (const QJsonValue& v : shArr) {
        SearchHistoryEntry e;
        if (v.isString()) {
            // 兼容旧版纯字符串格式
            e.text = v.toString();
            e.timestamp = 0;
        } else if (v.isObject()) {
            const QJsonObject ho = v.toObject();
            e.text = ho.value(QStringLiteral("text")).toString();
            e.timestamp = qint64(ho.value(QStringLiteral("ts")).toVariant().toLongLong());
        }
        if (!e.text.isEmpty())
            m_searchHistory.append(e);
    }
    // 按时间戳倒序排列（最近的在前）
    std::sort(m_searchHistory.begin(), m_searchHistory.end(),
              [](const SearchHistoryEntry& a, const SearchHistoryEntry& b) {
                  return a.timestamp > b.timestamp;
              });
    while (m_searchHistory.size() > 30)
        m_searchHistory.removeLast();
    m_searchText = o.value(QStringLiteral("search_text")).toString();
    const QString ck = o.value(QStringLiteral("checksum")).toString(QStringLiteral("None"));
    if (sjj::CHECKSUMS.contains(ck))
        m_cmbChecksum->setCurrentText(ck);
    bool okN = false;
    const int csStart = o.value(QStringLiteral("cs_start")).toString(QStringLiteral("1")).toInt(&okN);
    m_entryCsStart->setText(QString::number(okN ? qMax(1, csStart) : 1));
    const int csEnd = o.value(QStringLiteral("cs_end")).toString(QStringLiteral("0")).toInt(&okN);
    m_entryCsEnd->setText(QString::number(okN ? qMin(0, csEnd) : 0));
    const QString enc = o.value(QStringLiteral("encoding")).toString(QStringLiteral("UTF-8"));
    if (sjj::ENCODINGS.contains(enc))
        m_cmbEncoding->setCurrentText(enc);
    m_lastSendText = o.value(QStringLiteral("last_send_text")).toString();
    m_lastSendHex = o.value(QStringLiteral("last_send_hex")).toString();
    m_sendHexPrev = m_chkSendHex->isChecked();
    loadSendSlot();
    // 窗口大小恢复（校验最小值与屏幕范围）
    const int w = o.value(QStringLiteral("window_w")).toInt(0);
    const int h = o.value(QStringLiteral("window_h")).toInt(0);
    if (w >= minimumWidth() && h >= minimumHeight()) {
        int ww = w, hh = h;
        if (screen()) {
            const QRect geo = screen()->availableGeometry();
            if (ww > geo.width())
                ww = geo.width();
            if (hh > geo.height())
                hh = geo.height();
        }
        resize(ww, hh);
    }
    // 命令面板（快捷命令）显示状态恢复
    if (o.value(QStringLiteral("panel_visible")).toBool(false)) {
        m_msPanel->show();
        const int idx = o.value(QStringLiteral("panel_tab")).toInt(0);
        if (idx >= 0 && idx < m_msTabs->count())
            m_msTabs->setCurrentIndex(idx);
    }
    refreshStatus();
}

void SerialTool::autoloadConfig() {
    if (QFileInfo::exists(configPath()))
        loadParams(true);
    else
        saveParams(true);
}
