#pragma once
// SuperCOM 主窗口（C++/Qt6 版，功能与 PySide6 版完全一致）

#include <QWidget>
#include <QSerialPort>
#include <QSerialPortInfo>
#include <QVector>
#include <QStringList>
#include <QHash>
#include <QSet>
#include <QTimer>
#include <QPair>
#include <QVariantMap>
#include <QJsonArray>
#include <QPointer>

#include "themes.h"
#include "combobox.h"
#include "themechkbox.h"
#include "titlebar.h"

class QTextEdit;
class QLabel;
class QLineEdit;
class QPushButton;
class QListWidget;
class QListWidgetItem;
class QTabWidget;
class QTabBar;
class QScrollArea;
class QProgressBar;
class QToolButton;
class QDialog;
class QFrame;
class QMenu;
class QGroupBox;
class QVBoxLayout;
class QHBoxLayout;
class QFileSystemWatcher;
class QListWidget;

class UpdateChecker;
class ExeDownloader;

// 一条接收/发送/系统记录（渲染时按当前 HEX/编码/时间戳开关格式化）
struct Record {
    QString kind;          // "rx" | "tx" | "sys"
    QByteArray rawBytes;   // rx: 原始字节
    QString rawText;       // sys 文本 / tx 原文
    QByteArray txData;     // tx: 完整发送数据（含校验字节）
    QByteArray txCs;       // tx: 校验字节
    bool hexInput = false; // tx: 发送源是否 HEX 文本
    QString ts;            // 固定时间戳 [HH:MM:SS.mmm]
};

// 快捷命令条目
struct MsEntry {
    bool hex = false;
    QString content;
    QString label;
};

// 搜索历史条目（带时间戳，按时间倒序）
struct SearchHistoryEntry {
    QString text;
    qint64 timestamp = 0;   // Unix epoch ms
};

// 快捷命令分组（可命名、可折叠）
struct MsGroup {
    QString name;                       // 分组名（如「测零」「防坠」）
    bool collapsed = false;             // 是否折叠
    QVector<MsEntry> entries;           // 组内命令
};

class SerialTool : public QWidget {
    Q_OBJECT
public:
    explicit SerialTool(QWidget* parent = nullptr);

protected:
    void paintEvent(QPaintEvent* e) override;
    void showEvent(QShowEvent* e) override;
    void closeEvent(QCloseEvent* e) override;
    bool eventFilter(QObject* obj, QEvent* event) override;

private:
    // ---------- UI 构建 ----------
    void buildUi();

    // ---------- 主题 ----------
    void applyTheme(const QString& themeName);
    void toggleTheme();
    QString themeTextColor() const;   // 当前主题 text_primary

    // ---------- 更新检查 / 下载 ----------
    void checkUpdateAuto();
    void checkUpdateManual();
    void onUpdateCheckResult(const QVariantMap& info, bool manual);
    void showUpdateDialog(const QVariantMap& info);
    void startUpdateDownload(const QVariantMap& info);
    void onDownloadProgress(qint64 done, qint64 total, int speedKbps);
    void onUpdateDownloaded(const QString& tmp);
    void onUpdateDownloadFailed(const QString& err);
    void closeDownloadUi();
    void performUpdateRestart();

    // ---------- 端口 / 串口 ----------
    void refreshPorts();
    QString getSelectedPort() const;
    void showSettingsDialog();
    void applySettings(QDialog* dlg, QComboBox* cmbBaud, QComboBox* cmbDatabits,
                       QComboBox* cmbStopbits, QComboBox* cmbParity,
                       QComboBox* cmbFlow, QLineEdit* entryTimeout);
    void onPortChanged(int idx);
    void onBaudChanged(int idx);
    void applySerialSettings();
    void togglePort();
    void openPort();
    void closePort();
    void onSerialData();
    void onSerialError(QSerialPort::SerialPortError err);
    void onFileBytesWritten(qint64 bytes);
    void refreshStatus();

    // ---------- 接收渲染 ----------
    QString rxBody(const QByteArray& data) const;
    QString decodeBytesByCurrent(const QByteArray& data) const;
    QPair<QString, QString> renderParts(const Record& rec) const;
    QString renderRecord(const Record& rec) const;
    void addTxRecord(const QString& raw, const QByteArray& data,
                     const QByteArray& csBytes = QByteArray(), bool hexInput = false);
    void addRecord(const QString& kind, const QString& raw);
    void appendToView(QVector<Record>& recs);
    void deleteTopLines(int n);
    void refreshView();
    QString accentHex() const;
    void applyHighlight();
    void onSearchChange();
    void onSearchOptionChanged();        // 任一选项变化：立即刷新高亮/筛选（VS Code 风格：3 个开关相互独立，可任意组合）
    void commitSearchHistory();          // 回车/失焦：匹配成功时把当前搜索词记入历史
    void onSearchFocusOut();             // 搜索框失焦：匹配成功时记录历史
    void showSearchHistoryPopup();       // 点击下拉箭头：弹出/刷新搜索历史下拉
    void refreshHistoryPopup();          // 按当前输入框内容过滤并重填历史下拉
    bool searchMatch(const QString& text) const; // 按 正则/大小写 判定文本是否命中
    void searchJump(int delta);          // 跳转到上一个/下一个匹配（+1 下一个 / -1 上一个）
    void updateSearchCountLabel();       // 刷新匹配计数标签与箭头可用状态
    void toggleSearchBar();              // Ctrl+F：显示/隐藏接收区右上角搜索条
    void hideSearchBar();                // 关闭搜索条并清空搜索状态
    void onRecvContextMenu(const QPoint& pos); // 接收区右键菜单：清除/HEX/时间戳/暂停/编码/查找
    void onRecvScroll(int value);
    void scrollRecvToBottom();       // 一键回到底部并恢复刷新（右下角悬浮按钮）
    void positionScrollBottomBtn();  // 把悬浮按钮定位到滚动条下方（跟随窗口缩放）
    void drainRx();
    void flushRxPacket();   // 按读超时空闲分包：把待分包缓存作为一条 rx 记录送出
    void clearRecv();
    void saveRecv();

    // ---------- 校验 ----------
    QByteArray computeChecksumFor(const QByteArray& data, const QString& algorithm) const;
    QByteArray checksumRangeFor(const QByteArray& data) const;
    void updateChecksumHighlight();
    QPair<QByteArray, QByteArray> applyChecksum(const QByteArray& data);
    void updateCsPreview();
    void doCsPreview();

    // ---------- 发送 ----------
    QByteArray encodeSendText(const QString& raw, bool& ok, QString& err) const;
    void send();
    void doSend(const QString& raw);
    void echoSend(const QString& raw, const QByteArray& data, const QByteArray& csBytes);
    void recordSendHistory(const QString& raw);
    void clearSend();
    void saveCurrentSendSlot();
    void loadSendSlot();
    void onSendHexToggle();

    // ---------- 定时发送 ----------
    void onTimerToggle();
    void timerSendTick();

    // ---------- 多字符串发送（分组） ----------
    void refreshMsRows();
    void msSetHex(int g, int i, bool val);
    void msSetContent(int g, int i, const QString& t);
    void newMsEntry();                   // 在活动分组新建命令
    void newMsGroup();                   // 新建分组
    void deleteMsEntry(int g, int i);
    void renameMsEntry(int g, int i);
    void sendMsEntry(int g, int i);
    void deleteMsGroup(int g);
    void renameMsGroup(int g, const QString& name);
    void setActiveMsGroup(int g);        // 设定活动分组（循环发送目标 + 高亮）
    void applyMsActiveStyle();           // 重绘分组高亮边框
    void cycleMsTab(int dir);            // 选项卡按顺序切换（dir=-1前/1后）
    void showTabDropdown();             // 展开/收起选项卡列表浮层（显示在按钮左侧）
    void hideTabPopup();                // 收起选项卡列表浮层并复位按钮状态
    void startMsLoop();
    void msLoopTick();
    void stopMsLoop();
    // 快捷命令多实例实时同步（仅快捷命令；其他配置不实时同步）
    void scheduleQuickCmdSave();              // 内容编辑防抖保存（300ms）
    void onConfigFileChanged(const QString& path);   // 配置文件被其他实例修改

    // ---------- 文件发送 ----------
    void pickFile();
    void sendFile();
    void sendFileChunk();
    void stopSendFile();
    void endSendFile();

    // ---------- 历史记录 ----------
    void refreshHistoryList();
    void histLoad(QListWidgetItem* item);
    void histDelete();
    void histClear();

    // ---------- 面板切换 ----------
    void toggleWindow(int tabIndex);

    // ---------- GitHub ----------
    void openGithub();

    // ---------- 配置 ----------
    QString configPath() const;
    QJsonArray msGroupsToData() const;
    QVector<MsGroup> msGroupsFromArray(const QJsonArray& arr) const;  // 从 JSON 还原分组
    void saveParams(bool silent);
    void loadParams(bool silent);
    void loadParamsInner(bool silent);
    void autoloadConfig();

    // ---------- 无边框窗口缩放 ----------
    void installResizeFilter();
    int detectResizeEdge(const QPoint& pos) const;
    Qt::CursorShape resizeCursor(int edges) const;
    void applyResize(const QPoint& gpos);
    void enableDwmEffects();

    // ============ 控件 ============
    TitleBar* m_titleBar = nullptr;

    QTextEdit* m_txtRecv = nullptr;
    QGroupBox* m_recvBox = nullptr;      // 接收区容器（滚动条下方悬浮按钮的定位基准）
    QPushButton* m_btnScrollBottom = nullptr; // 接收区右下角"回到底部"悬浮按钮（离开底部时显示）
    QTabWidget* m_msTabs = nullptr;
    QToolButton* m_tabDropdownBtn = nullptr;       // 选项卡右侧下拉箭头
    QPointer<QWidget> m_tabPopup;                  // 当前展开的选项卡列表浮层（再次点击收起）
    qint64 m_tabPopupClosedAt = 0;                 // 浮层最近一次关闭时刻（ms），用于吞掉紧随其后的重复点击
    QListWidget* m_histList = nullptr;
    QWidget* m_msPanel = nullptr;        // 右侧面板容器（顶部工具栏 + 选项卡）

    ThemeCheckBox* m_chkShowHex = nullptr;
    ThemeCheckBox* m_chkShowTs = nullptr;
    ThemeCheckBox* m_chkPause = nullptr;
    StyledComboBox* m_cmbEncoding = nullptr;
    QWidget* m_searchBar = nullptr;         // 接收区右上角悬浮搜索条（默认隐藏）
    QLineEdit* m_entrySearch = nullptr;
    QLabel* m_lblSearchCount = nullptr;   // 搜索匹配计数 "x/y"
    QToolButton* m_btnSearchPrev = nullptr;  // 上一个匹配
    QToolButton* m_btnSearchNext = nullptr;  // 下一个匹配
    QToolButton* m_btnSearchClose = nullptr; // 关闭搜索条
    QToolButton* m_btnRegex = nullptr;      // 正则匹配按钮（文本 ".*"）
    QToolButton* m_btnCase = nullptr;       // 大小写敏感按钮（文本 "Cc"，亮起=区分大小写）
    QToolButton* m_btnFilter = nullptr;     // 筛选按钮（图标）
    QToolButton* m_btnClearSearch = nullptr; // 清空当前输入（图标 ✕）
    QListWidget* m_historyPopup = nullptr;   // 搜索历史下拉（顶层 ToolTip，箭头唤出）
    QString m_searchText;                    // 上一次搜索词（关闭时记忆，重开恢复）
    QVector<SearchHistoryEntry> m_searchHistory; // 搜索历史（带时间戳，时间倒序）
    PortComboBox* m_cmbPort = nullptr;
    StyledComboBox* m_cmbBaud = nullptr;
    QPushButton* m_btnOpen = nullptr;

    ThemeCheckBox* m_chkSendHex = nullptr;
    ThemeCheckBox* m_chkAddCrlf = nullptr;
    ThemeCheckBox* m_chkTimer = nullptr;
    QLineEdit* m_entryInterval = nullptr;
    QTextEdit* m_txtSend = nullptr;

    QPushButton* m_btnSelectFile = nullptr;
    QPushButton* m_btnSendFile = nullptr;
    QFrame* m_csGroup = nullptr;
    QLineEdit* m_entryCsStart = nullptr;
    QLineEdit* m_entryCsEnd = nullptr;
    StyledComboBox* m_cmbChecksum = nullptr;
    QLabel* m_lblCsResult = nullptr;

    QWidget* m_statusBar = nullptr;
    QHBoxLayout* m_sbLay = nullptr;
    QLabel* m_lblStatus = nullptr;
    QWidget* m_progressContainer = nullptr;
    QProgressBar* m_progressBar = nullptr;
    QLabel* m_lblSpeed = nullptr;
    QToolButton* m_btnCancelDl = nullptr;
    QToolButton* m_btnRestart = nullptr;
    QLabel* m_lblSr = nullptr;
    QLabel* m_lblHandshake = nullptr;
    QLabel* m_lblApp = nullptr;

    ThemeCheckBox* m_chkMsLoop = nullptr;
    QLineEdit* m_entryMsInterval = nullptr;
    QPushButton* m_btnStartMs = nullptr;
    QPushButton* m_btnStopMs = nullptr;

    // ============ 状态 ============
    QSerialPort* m_ser = nullptr;
    quint64 m_rxBytes = 0;
    quint64 m_txBytes = 0;
    QVector<Record> m_records;                        // 全部记录（上限 MAX_RECORDS）
    // 统一收发事件队列：rx/tx/sys 均按"真实发生顺序"入队，
    // 50ms 定时器按队首到尾顺序刷新——保证显示顺序与实际收发顺序一致
    QVector<Record> m_pendingEvents;
    QStringList m_sendHistory;                        // 发送历史（去重、置顶、上限 50）
    QVector<MsGroup> m_msGroups;                      // 快捷命令分组
    int m_activeMsGroup = 0;                          // 当前活动分组（循环发送目标）
    QHash<QString, QString> m_portMap;                // 显示 label → 设备名
    bool m_msLooping = false;
    int m_msLoopIndex = 0;                            // 循环发送在活动组内的下标
    int m_msLoopGroup = 0;                            // 循环发送锁定到的分组
    bool m_fileSending = false;
    QByteArray m_fileData;
    qsizetype m_fileOffset = 0;
    bool m_fileWaitingWrite = false; // 文件发送：上一块已入缓冲，等 bytesWritten 确认后再发下一块
    QString m_selectedFilePath;
    QDialog* m_settingsWin = nullptr;
    bool m_suppressApply = false;    // 程序化修改串口参数时抑制"自动重开"
    bool m_programScroll = false;    // 程序性滚动期间抑制"自动勾选暂停刷新"
    int m_lastScrollMax = 0;         // 上次 onRecvScroll 记录的滚动条最大值（识别 range 收缩 clamp）
    qint64 m_lastStatusTs = 0;
    QString m_lastSrText;
    QString m_lastHsText;
    QString m_lastSendText;
    QString m_lastSendHex;
    bool m_sendHexPrev = false;
    QTimer* m_csPreviewTimer = nullptr;
    QTimer* m_searchTimer = nullptr;
    QVector<int> m_searchHits;          // 搜索匹配起点（字符偏移），配合计数/跳转
    QVector<int> m_searchHitLens;       // 每个匹配的长度（正则时 != needle 长度）
    int m_searchIndex = -1;             // 当前选中匹配下标（-1 未定位）
    QTimer* m_flushTimer = nullptr;
    QTimer* m_timerSend = nullptr;
    QTimer* m_msLoopTimer = nullptr;
    QFileSystemWatcher* m_cfgWatcher = nullptr;   // 监听配置文件（快捷命令同步）
    QTimer* m_cfgSaveTimer = nullptr;             // 快捷命令保存防抖
    QString m_theme = QStringLiteral("light");
    QString m_winBg;
    sjj::ThemeColors m_themeC;
    int m_rxEpoch = 0;                // 串口代际号：关闭后丢弃旧信号
    UpdateChecker* m_updateChecker = nullptr;
    ExeDownloader* m_downloadThread = nullptr;
    QString m_ignoreUpdateVersion;
    QString m_pendingUpdateVersion;
    QString m_updateTmp;

    // 串口参数（与 Python 版 var_* 对应）
    QString m_databits = QStringLiteral("8");
    QString m_stopbits = QStringLiteral("1");
    QString m_parity = QStringLiteral("None");
    QString m_flow = QStringLiteral("None");
    QString m_readTimeout = QStringLiteral("50");   // 读超时(ms)：接收空闲超时分包阈值

    // 按读超时空闲分包状态：字节先攒进缓存，数据流停顿超过 m_readTimeout 才作为一条
    // （防内存撑爆：缓存达到 MAX_RX_BUF 立即强制分包，无论是否空闲）
    QByteArray m_rxBuf;
    QString m_pktStartTs;                 // 当前包首字节到达时刻（作该条时间戳）
    QTimer* m_pktTimer = nullptr;         // 分包空闲定时器（singleShot）
    QTimer* m_portPollTimer = nullptr;   // 串口热插拔轮询（2s 周期）
    QStringList m_lastPortDevs;           // 上次检测到的端口名集合（用于变更比较）
    // 分包上限 64KB：单条记录变小，50ms 刷入更均匀，
    // 避免 512KB 大包一次插入 1.5MB HEX 文本把 UI 卡死
    static constexpr qsizetype MAX_RX_BUF = 64 * 1024;

    // 无边框缩放
    int m_resizeMargin = 8;
    int m_resizeEdge = 0;
    QPoint m_resizeStartPos;
    QRect m_resizeStartGeo;
    QSet<QObject*> m_filterInstalled;

    enum EdgeMask { EdgeLeft = 1, EdgeRight = 2, EdgeTop = 4, EdgeBottom = 8 };
};
