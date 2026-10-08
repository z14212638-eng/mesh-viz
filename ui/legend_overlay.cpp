#include "legend_overlay.h"
#include <QPainter>

LegendOverlay::LegendOverlay(QWidget *parent)
    : QWidget(parent,
              Qt::Tool |
              Qt::FramelessWindowHint |
              Qt::BypassWindowManagerHint)
{
    setFixedSize(340, 160);
    setAttribute(Qt::WA_TranslucentBackground);
    setAttribute(Qt::WA_ShowWithoutActivating);
}

void LegendOverlay::paintEvent(QPaintEvent *)
{
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);

    p.setPen(Qt::NoPen);
    p.setBrush(QColor(0, 0, 0, 80));
    p.drawRoundedRect(rect().adjusted(0, 0, -1, -1), 8, 8);

    p.setPen(QColor(220, 220, 220));
    int y = 24;

    auto drawItem = [&](const QColor &c, const QString &txt)
    {
        p.setBrush(c);
        p.drawRect(12, y - 10, 14, 10);
        p.drawText(36, y, txt);
        y += 22;
    };

    auto drawArrowItem = [&](const QString &txt)
    {
        // Draw horizontal arrow example (indicating data flow)
        p.setPen(QPen(QColor(255, 255, 255, 220), 2));
        p.setBrush(QColor(255, 255, 255, 220));
        // Draw arrow line
        p.drawLine(12, y - 5, 22, y - 5);
        // Draw arrow head (pointing right)
        QPolygonF arrowHead;
        arrowHead << QPointF(22, y - 5)
                  << QPointF(18, y - 8)
                  << QPointF(18, y - 2);
        p.drawPolygon(arrowHead);
        p.drawText(36, y, txt);
        y += 22;
    };

    // Title
    p.setPen(QColor(255, 255, 255));
    p.setFont(QFont("Arial", 10, QFont::Bold));
    p.drawText(12, 18, "Legend");
    p.setFont(QFont("Arial", 9));
    p.setPen(QColor(220, 220, 220));
    y = 36;

    // PPDU colors
    drawItem(QColor("#4C72B0"), "Same physical node = same color");
    drawItem(QColor("#DB4437"), "Overlapping airtime (not collision proof)");
    drawItem(QColor("#7AA6D8"), "Hovered / selected PPDU");

    // Data flow
    drawArrowItem("Data Flow");
}
