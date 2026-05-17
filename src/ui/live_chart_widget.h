#ifndef LIVE_CHART_WIDGET_H
#define LIVE_CHART_WIDGET_H

#include <QWidget>
#include <QColor>
#include <QVector>
#include <QString>

class LiveChartWidget : public QWidget
{
    Q_OBJECT

public:
    explicit LiveChartWidget(QWidget *parent = nullptr);

    // Single series mode (backwards compatible)
    void setTitle(const QString &title);
    void setUnit(const QString &unit);
    void setColor(const QColor &color);
    void setYRange(double min, double max);
    void setMaxPoints(int count);
    void addValue(double value);
    void clearData();
    void clearSeries();

    // Multi series mode
    int addSeries(const QString &name, const QColor &color);
    void addDataPoint(int seriesIndex, double value);

    double currentValue() const { return m_currentValue; }
    QString chartTitle()   const { return m_title; }
    QString chartUnit()    const { return m_unit; }
    double minYValue()     const { return m_minY; }
    double maxYValue()     const { return m_maxY; }
    QColor chartColor()    const { return m_singleColor; }

protected:
    void paintEvent(QPaintEvent *event) override;
    QSize minimumSizeHint() const override { return QSize(200, 120); }
    QSize sizeHint() const override { return QSize(300, 160); }

private:
    struct SeriesData {
        QString name;
        QColor color;
        QVector<double> data;
    };

    void drawBackground(QPainter &p, const QRect &r);
    void drawGrid(QPainter &p, const QRect &r);
    void drawLines(QPainter &p, const QRect &r);
    void drawLineAndFill(QPainter &p, const QRect &r, const QVector<double> &data, const QColor &color);
    void drawLabels(QPainter &p, const QRect &r);
    void drawLegend(QPainter &p, const QRect &r);

    QString m_title;
    QString m_unit;
    QColor m_singleColor;
    int m_maxPoints = 120;
    double m_minY = 0.0, m_maxY = 100.0;
    double m_currentValue = 0.0;
    QVector<double> m_singleData;
    QVector<SeriesData> m_series;
    bool m_multiSeries = false;
};

#endif // LIVE_CHART_WIDGET_H
