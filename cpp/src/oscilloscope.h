#pragma once
// 示波器：对接收区每个分包取一个数值（文本正则 / 字节区间），按「分包序号 → 数值」画折线。
// 每个表达式一条折线，可勾选启用、配置乘除倍率、保存、删除。

#include <QWidget>
#include <QVector>
#include <QString>
#include <QByteArray>
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
class TitleBar;
class QJsonArray;
class QJsonObject;
class QWheelEvent;
class QMouseEvent;
class QContextMenuEvent;

// 一条折线：表达式 + 倍率 + 已提取的数据
struct OscSeries {
    bool enabled = true;
    QString pattern;
    QString scaleText = QStringLiteral("1");   // 用户原文，如 "1" / "0.1" / "/100"
    double scale = 1.0;                        // 解析后的实际倍率
    QRegularExpression regex;
    // 字节取值（pattern 写成 b2:3 这类写法时生效，1 基、含首尾）
    bool byteMode = false;
    int byteFrom = 1;
    int byteTo = 1;
    bool byteBigEndian = true;
    bool byteSigned = false;
    QVector<double> values;
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

protected:
    void paintEvent(QPaintEvent* e) override;
    void wheelEvent(QWheelEvent* e) override;
    void mousePressEvent(QMouseEvent* e) override;
    void mouseMoveEvent(QMouseEvent* e) override;
    void mouseReleaseEvent(QMouseEvent* e) override;
    void mouseDoubleClickEvent(QMouseEvent* e) override;
    void contextMenuEvent(QContextMenuEvent* e) override;

private:
    int dataCount() const;                    // 各折线中最长的点数
    int indexAtX(int x) const;                // 屏幕 x → 采样序号（按上次绘制的窗口）
    double valueAtY(int y) const;             // 屏幕 y → 数值（按上次绘制的纵轴）

    const QVector<OscSeries>* m_series = nullptr;
    sjj::ThemeColors m_tc;
    WaveView m_view;

    // 上次绘制的结果，供鼠标交互换算使用
    QRect m_plot;
    int m_i0 = 0;            // 窗口左端索引
    int m_i1 = -1;           // 窗口右端索引（-1 表示还没数据）
    double m_y0 = 0, m_y1 = 1;

    bool m_selActive = false;                // 正在拖拽框选
    QPoint m_selFrom, m_selTo;
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
    TitleBar* m_titleBar = nullptr;           // 自绘标题栏（含「?」帮助按钮）
    WaveChart* m_chart = nullptr;
    sjj::ThemeColors m_tc;

    // 无边框窗口边缘缩放
    enum EdgeMask { EdgeLeft = 1, EdgeRight = 2, EdgeTop = 4, EdgeBottom = 8 };
    int m_resizeMargin = 6;
    int m_resizeEdge = 0;
    QPoint m_resizeStartPos;
    QRect m_resizeStartGeo;

    static constexpr int kMaxPoints = 2000;   // 每条折线保留的点数上限
};
