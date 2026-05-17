#include "live_chart_widget.h"
#include <QPainter>
#include <QPainterPath>
#include <QtMath>

LiveChartWidget::LiveChartWidget(QWidget *parent)
    : QWidget(parent)
{
    setMinimumSize(200, 120);
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
}

void LiveChartWidget::setTitle(const QString &title) { m_title = title; update(); }
void LiveChartWidget::setUnit(const QString &unit)   { m_unit = unit; update(); }
void LiveChartWidget::setColor(const QColor &color)   { m_singleColor = color; update(); }
void LiveChartWidget::setMaxPoints(int count)        { m_maxPoints = count; }
void LiveChartWidget::setYRange(double min, double max) { m_minY = min; m_maxY = max; }

void LiveChartWidget::addValue(double value)
{
    m_currentValue = value;
    m_singleData.append(value);
    while (m_singleData.size() > m_maxPoints)
        m_singleData.removeFirst();
    update();
}

int LiveChartWidget::addSeries(const QString &name, const QColor &color)
{
    m_multiSeries = true;
    SeriesData sd;
    sd.name = name;
    sd.color = color;
    m_series.append(sd);
    return m_series.size() - 1;
}

void LiveChartWidget::addDataPoint(int seriesIndex, double value)
{
    if (seriesIndex < 0 || seriesIndex >= m_series.size()) return;
    m_series[seriesIndex].data.append(value);
    while (m_series[seriesIndex].data.size() > m_maxPoints)
        m_series[seriesIndex].data.removeFirst();
    // Update current value to the average of latest points
    double sum = 0;
    for (const auto &s : m_series)
        if (!s.data.isEmpty()) sum += s.data.last();
    m_currentValue = m_series.isEmpty() ? 0 : sum / m_series.size();
    update();
}

void LiveChartWidget::clearData()
{
    m_singleData.clear();
    for (auto &s : m_series)
        s.data.clear();
    m_currentValue = 0;
    update();
}

void LiveChartWidget::clearSeries()
{
    m_series.clear();
    m_multiSeries = false;
    m_singleData.clear();
    m_currentValue = 0;
    update();
}

void LiveChartWidget::paintEvent(QPaintEvent *)
{
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);

    const int mL = 45, mR = 12, mT = 28, mB = 4;
    QRect chart(mL, mT, width() - mL - mR, height() - mT - mB);

    drawBackground(p, chart);
    drawGrid(p, chart);

    if (m_multiSeries) {
        for (const auto &s : m_series) {
            if (s.data.size() >= 2)
                drawLineAndFill(p, chart, s.data, s.color);
        }
        drawLegend(p, chart);
    } else {
        if (m_singleData.size() >= 2)
            drawLineAndFill(p, chart, m_singleData, m_singleColor);
    }

    drawLabels(p, chart);
}

void LiveChartWidget::drawBackground(QPainter &p, const QRect &r)
{
    p.save();
    p.fillRect(rect(), QColor("#1e1e2e"));
    p.setBrush(QColor("#181825"));
    p.setPen(QPen(QColor("#313244"), 1));
    p.drawRoundedRect(r.adjusted(-2, -2, 2, 2), 4, 4);
    p.restore();
}

void LiveChartWidget::drawGrid(QPainter &p, const QRect &r)
{
    p.save();
    p.setPen(QPen(QColor("#313244"), 1));
    for (int i = 0; i <= 4; i++) {
        int y = r.top() + r.height() * i / 4;
        p.drawLine(r.left(), y, r.right(), y);
    }
    p.restore();
}

void LiveChartWidget::drawLineAndFill(QPainter &p, const QRect &r,
                                       const QVector<double> &data, const QColor &color)
{
    double yRange = m_maxY - m_minY;
    if (yRange <= 0) yRange = 1;
    const int startIdx = qMax(0, data.size() - m_maxPoints);
    const double stepX = static_cast<double>(r.width()) / (m_maxPoints - 1);

    // Build path
    QPainterPath path;
    bool first = true;
    for (int i = startIdx; i < data.size(); i++) {
        double x = r.left() + (i - startIdx) * stepX;
        double y = r.bottom() - (data[i] - m_minY) / yRange * r.height();
        y = qBound(static_cast<double>(r.top()), y, static_cast<double>(r.bottom()));
        if (first) { path.moveTo(x, y); first = false; }
        else       path.lineTo(x, y);
    }

    if (!m_multiSeries) {
        // Fill under line (only for single series)
        QPainterPath fillPath = path;
        fillPath.lineTo(r.right(), r.bottom());
        fillPath.lineTo(r.left(), r.bottom());
        fillPath.closeSubpath();

        QLinearGradient grad(r.topLeft(), r.bottomLeft());
        QColor base = color;
        base.setAlpha(50);
        grad.setColorAt(0.0, base);
        base.setAlpha(6);
        grad.setColorAt(1.0, base);
        p.save();
        p.setBrush(grad);
        p.setPen(Qt::NoPen);
        p.drawPath(fillPath);
        p.restore();
    }

    // Draw the line
    p.save();
    p.setPen(QPen(color, m_multiSeries ? 1.5 : 2));
    p.setBrush(Qt::NoBrush);
    p.drawPath(path);
    p.restore();
}

void LiveChartWidget::drawLabels(QPainter &p, const QRect &r)
{
    p.save();

    // Title
    QFont titleFont = font();
    titleFont.setPixelSize(12);
    titleFont.setBold(true);
    p.setFont(titleFont);
    p.setPen(QColor("#cdd6f4"));
    QString titleText = m_title;
    if (!m_unit.isEmpty())
        titleText += " (" + m_unit + ")";
    p.drawText(r.left(), 4, r.width() - 70, 20, Qt::AlignLeft | Qt::AlignBottom, titleText);

    // Current value
    QString valText = QString::number(m_currentValue, 'f', 1);

    QFont valFont = font();
    valFont.setPixelSize(18);
    valFont.setBold(true);
    p.setFont(valFont);
    p.setPen(m_multiSeries ? QColor("#cdd6f4") : m_singleColor);
    p.drawText(r.right() - 60, 4, 60, 20, Qt::AlignRight | Qt::AlignBottom, valText);

    // Y axis labels
    QFont axisFont = font();
    axisFont.setPixelSize(9);
    p.setFont(axisFont);
    p.setPen(QColor("#6c7086"));
    for (int i = 0; i <= 4; i++) {
        int y = r.top() + r.height() * i / 4;
        double val = m_maxY - (m_maxY - m_minY) * i / 4;
        QString label = QString::number(static_cast<int>(val));
        p.drawText(2, y - 6, 40, 12, Qt::AlignRight | Qt::AlignVCenter, label);
    }

    p.restore();
}

void LiveChartWidget::drawLegend(QPainter &p, const QRect &r)
{
    if (m_series.isEmpty()) return;
    p.save();

    QFont f = font();
    f.setPixelSize(9);
    p.setFont(f);

    // Draw legend in bottom-right of chart area
    int legendX = r.right() - 10;
    int legendY = r.bottom() - 10;

    for (int i = m_series.size() - 1; i >= 0; i--) {
        const auto &s = m_series[i];
        QString text = s.name;
        // Don't show data count suffix, just the name
        int tw = p.fontMetrics().horizontalAdvance(text) + 16;
        int th = 14;
        legendY -= th;

        QRect bg(legendX - tw, legendY, tw, th);
        p.fillRect(bg, QColor(0, 0, 0, 120));
        p.setPen(s.color);
        p.drawRect(bg.adjusted(2, 2, -2, -2));
        p.drawText(bg.adjusted(16, 0, -2, 0), Qt::AlignVCenter, text);
    }

    p.restore();
}
