#pragma once
// 示波器：对接收区每个分包取一个数值（文本正则 / 字节区间），按「分包序号 → 数值」画折线。
// 每个表达式一条折线，可勾选启用、配置乘除倍率、保存、删除。

#include <QWidget>
#include <QVector>
#include <QString>
#include <QByteArray>
#include <QPair>
#include <QRegularExpression>

#include <functional>

#include "themes.h"

class QLineEdit;
class QCheckBox;
class QScrollArea;
class QVBoxLayout;
class QDialog;
class QPushButton;
class QToolButton;
class QPaintEvent;
class QEvent;
class QShowEvent;
class QTimer;
class TitleBar;
class QJsonArray;
class QJsonObject;
class QWheelEvent;
class QMouseEvent;
class QContextMenuEvent;

// 字节取值的帧校验：不满足条件的分包直接跳过（用于日志与协议混流）
struct OscFrameCheck {
    int lenExact = -1;                  // 包长必须等于（-1=不校验）
    int lenMin = 0;                     // 包长下限（0=不校验）
    int sum8 = -1;                      // 累加和低 8 位期望值（-1=不校验）
    int xor8 = -1;                      // 异或和期望值（-1=不校验）
    bool crc16Modbus = false;           // Modbus CRC16 整包校验（含 CRC 时为 0）
    QVector<QPair<int, int>> byteEq;    // (第几字节, 期望值)：1 基
};

// 字节取值写法：b2:3 / b2:3le / i2:3 / b5，可用 # 追加帧校验
struct OscByteSpec {
    int from = 1;
    int to = 1;
    bool bigEndian = true;
    bool sign = false;
    OscFrameCheck check;
};

// 一条折线：表达式 + 倍率 + 已提取的数据
struct OscSeries {
    bool enabled = true;
    QString pattern;
    QString scaleText = QStringLiteral("1");   // 用户原文，如 "1" / "0.1" / "/100"
    double scale = 1.0;                        // 解析后的实际倍率
    QRegularExpression regex;
    // 字节取值（pattern 写成 b2:3 这类写法时生效，1 基、含首尾）
    bool byteMode = false;
    OscByteSpec byte;
    QVector<double> values;      // 提取到的值（未乘倍率）
    QVector<int> indices;        // 每个值对应的采样序号（分包序号），与 values 等长
};

// 当前视图范围：横轴窗口 + 纵轴范围（手动/自动）
struct WaveView {
    int xSpan = 0;          // 0 = 全部；>0 = 只显示最近 xSpan 个点
    int xRight = -1;        // -1 = 跟随最新；否则窗口右端（采样序号）
    bool yAuto = true;      // 纵轴自动按窗口内数据取范围
    double yMin = 0;        // yAuto=false 时生效
    double yMax = 1;
};

// 自绘折线图：无 Qt Charts 依赖，横坐标=采样序号，纵坐标=数值
class WaveChart : public QWidget {
public:
    explicit WaveChart(QWidget* parent = nullptr);
    void bindSeries(const QVector<OscSeries>* series);
    void setThemeColors(const sjj::ThemeColors& t);

    const WaveView& view() const { return m_view; }
    void setView(const WaveView& v);
    void resetView();                         // 全部数据 + 跟随最新 + 纵轴自动

    // 左键功能：框选放大 / 拖拽平移 / 选点十字线读数
    enum class LeftMode { Box, Pan, Pick };
    LeftMode leftMode() const { return m_leftMode; }
    void setLeftMode(LeftMode m);

protected:
    void paintEvent(QPaintEvent* e) override;
    void wheelEvent(QWheelEvent* e) override;
    void mousePressEvent(QMouseEvent* e) override;
    void mouseMoveEvent(QMouseEvent* e) override;
    void mouseReleaseEvent(QMouseEvent* e) override;
    void mouseDoubleClickEvent(QMouseEvent* e) override;
    void contextMenuEvent(QContextMenuEvent* e) override;

private:
    int dataCount() const;                    // 现有采样序号跨度（末序号-首序号+1）
    int dataFirst() const;                    // 现有数据的最小采样序号（无数据返回 0）
    int indexAtX(int x) const;                // 屏幕 x → 采样序号（按上次绘制的窗口）
    double valueAtY(int y) const;             // 屏幕 y → 数值（按上次绘制的纵轴）

    // 一个像素列的包络：该序号区间内的首/末值与极值
    struct ColAgg {
        int firstIdx = 0;
        int lastIdx = 0;
        double firstV = 0, lastV = 0, minV = 0, maxV = 0;
        int count = 0;
    };
    // 每条折线的包络缓存：左端锚定的等宽序号桶，末尾追加时可增量更新
    struct SeriesEnvelope {
        bool valid = false;
        int i0 = 0;               // 缓存对应的窗口左端序号
        int ppc = 1;              // 每列点数（桶宽）
        int cols = 0;             // 绘图区像素列数
        int firstIdxCached = -1;  // 缓存时该折线的首个序号（裁剪/清空检测）
        int coveredUpTo = -1;     // 已聚合到的最大序号
        QVector<ColAgg> buckets;
    };
    const QVector<ColAgg>& ensureEnvelope(int seriesIdx, const OscSeries& s,
                                          int i0, int i1, int cols);

    const QVector<OscSeries>* m_series = nullptr;
    QVector<SeriesEnvelope> m_env;           // 与 m_series 一一对应的包络缓存
    sjj::ThemeColors m_tc;
    WaveView m_view;

    // 上次绘制的结果，供鼠标交互换算使用
    QRect m_plot;
    int m_i0 = 0;            // 窗口左端索引
    int m_i1 = -1;           // 窗口右端索引（-1 表示还没数据）
    double m_y0 = 0, m_y1 = 1;

    bool m_selActive = false;                // 正在拖拽框选
    QPoint m_selFrom, m_selTo;

    LeftMode m_leftMode = LeftMode::Box;     // 左键当前功能
    bool m_panActive = false;                // 正在拖拽平移
    QPoint m_panOrigin;                      // 按下时的鼠标位置
    int m_panStartRight = -1;                // 按下时窗口右端序号
    int m_panSpan = 0;                       // 按下时窗口点数

    bool m_pickValid = false;                // 选点十字线是否显示
    bool m_pickPinned = false;               // 十字线已固定：不再跟随鼠标，再次点击恢复
    QPoint m_pickPos;                        // 十字线位置（已夹到绘图区内）
};

class Oscilloscope : public QWidget {
    Q_OBJECT
public:
    explicit Oscilloscope(QWidget* parent = nullptr);

    void setThemeColors(const sjj::ThemeColors& t);
    // 一包接收数据：text 为解码后的文本（正则用），raw 为原始字节（字节取值用）
    void feed(const QString& text, const QByteArray& raw = QByteArray());
    QJsonArray toJson() const;                 // 保存表达式配置（不含采样数据）
    void fromJson(const QJsonArray& arr);
    void clearData();
    QVector<double> samples(int seriesIndex) const;   // 只读：原始值（未乘倍率）
    QJsonObject viewToJson() const;                   // 视图范围（横轴窗口 + 纵轴范围）
    void viewFromJson(const QJsonObject& o);

private slots:
    void addSeries();
    void rebuildRows();
    void showRegexHelp();                      // 正则说明 + 可一键复制的示例
    void openConfig();                         // 打开/收起表达式配置弹窗
    void showHelp();                           // 右上角「?」：示波器操作提示

private:
    void buildConfigDialog();                  // 懒创建配置弹窗（非模态）
    void updateCfgButton();                    // 主界面入口按钮的「启用/总数」文字
    void scheduleChartUpdate();                // 刷新节流（约 30fps）
    bool eventFilter(QObject* watched, QEvent* e) override;   // 未激活窗口时首次单击即进入编辑 + 边缘缩放
    void showEvent(QShowEvent* e) override;    // DWM 阴影 + Win11 圆角
    void paintEvent(QPaintEvent* e) override;  // 无边框窗口背景（主题 win_bg）
    void installResizeSupport();               // 无边框窗口：安装边缘缩放（鼠标跟踪 + 事件过滤）
    int detectResizeEdge(const QPoint& pos) const;
    static Qt::CursorShape resizeCursor(int edges);
    void applyResize(const QPoint& gpos);

    struct Row {
        QWidget* widget = nullptr;
        QCheckBox* check = nullptr;
        QLineEdit* pattern = nullptr;
        QLineEdit* scale = nullptr;
    };

    static bool parseScale(const QString& text, double& out);
    void applyPattern(int idx, const QString& text);
    void applyScale(int idx, const QString& text);

    QVector<OscSeries> m_series;
    QVector<Row> m_rows;
    QDialog* m_cfgDlg = nullptr;              // 表达式配置弹窗（关闭仅隐藏）
    QScrollArea* m_rowsScroll = nullptr;      // 弹窗内的表达式行容器
    QVBoxLayout* m_rowsLay = nullptr;
    QPushButton* m_btnCfg = nullptr;          // 主界面「提取表达式（启用/总数）」入口
    QPushButton* m_btnModeBox = nullptr;      // 左键：框选放大
    QPushButton* m_btnModePan = nullptr;      // 左键：拖拽平移
    QPushButton* m_btnModePick = nullptr;     // 左键：选点（十字线读数）
    void syncModeButtons();                   // 按钮勾选状态与图表左键模式同步
    TitleBar* m_titleBar = nullptr;           // 自绘标题栏（含「?」帮助按钮）
    WaveChart* m_chart = nullptr;
    sjj::ThemeColors m_tc;

    // 无边框窗口边缘缩放
    enum EdgeMask { EdgeLeft = 1, EdgeRight = 2, EdgeTop = 4, EdgeBottom = 8 };
    int m_resizeMargin = 6;
    int m_resizeEdge = 0;
    QPoint m_resizeStartPos;
    QRect m_resizeStartGeo;

    // 每条折线保留的点数上限；超出后按 kTrimBatch 批量丢弃最旧的点，
    // 避免每次追加都做 O(n) 整体搬移（10 万点时尤其重要）
    static constexpr int kMaxPoints = 100000;  // 每条折线保留的点数上限
    static constexpr int kTrimBatch = 10000;   // 批量裁剪粒度
    int m_sampleSeq = 0;                      // 下一个分包序号（所有折线共用，用于横轴对齐）
    QTimer* m_paintTimer = nullptr;           // 刷新节流：高频数据下最多约 30fps 重绘
};
