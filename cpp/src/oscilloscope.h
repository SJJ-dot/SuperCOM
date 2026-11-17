#pragma once
// 示波器：从接收文本里按正则提取数字，按「提取次数 → 数值」画折线。
// 每个表达式一条折线，可勾选启用、配置乘除倍率、保存、删除。

#include <QWidget>
#include <QVector>
#include <QString>
#include <QRegularExpression>

#include "themes.h"

class QLineEdit;
class QCheckBox;
class QScrollArea;
class QVBoxLayout;
class QDialog;
class QPushButton;
class QPaintEvent;
class QEvent;
class QJsonArray;

// 一条折线：表达式 + 倍率 + 已提取的数据
struct OscSeries {
    bool enabled = true;
    QString pattern;
    QString scaleText = QStringLiteral("1");   // 用户原文，如 "1" / "0.1" / "/100"
    double scale = 1.0;                        // 解析后的实际倍率
    QRegularExpression regex;
    QVector<double> values;
};

// 自绘折线图：无 Qt Charts 依赖，横坐标=提取次数，纵坐标=数值
class WaveChart : public QWidget {
public:
    explicit WaveChart(QWidget* parent = nullptr);
    void bindSeries(const QVector<OscSeries>* series);
    void setThemeColors(const sjj::ThemeColors& t);

protected:
    void paintEvent(QPaintEvent* e) override;

private:
    const QVector<OscSeries>* m_series = nullptr;
    sjj::ThemeColors m_tc;
};

class Oscilloscope : public QWidget {
    Q_OBJECT
public:
    explicit Oscilloscope(QWidget* parent = nullptr);

    void setThemeColors(const sjj::ThemeColors& t);
    void feed(const QString& text);            // 一段接收文本 → 各表达式提取数字
    QJsonArray toJson() const;                 // 保存表达式配置（不含采样数据）
    void fromJson(const QJsonArray& arr);
    void clearData();
    QVector<double> samples(int seriesIndex) const;   // 只读：原始值（未乘倍率）

private slots:
    void addSeries();
    void rebuildRows();
    void showRegexHelp();                      // 正则说明 + 可一键复制的示例
    void openConfig();                         // 打开/收起表达式配置弹窗

private:
    void buildConfigDialog();                  // 懒创建配置弹窗（非模态）
    void updateCfgButton();                    // 主界面入口按钮的「启用/总数」文字
    bool eventFilter(QObject* watched, QEvent* e) override;   // 未激活窗口时首次单击即进入编辑

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
    WaveChart* m_chart = nullptr;
    sjj::ThemeColors m_tc;

    static constexpr int kMaxPoints = 2000;   // 每条折线保留的点数上限
};
