// SPDX-License-Identifier: MIT
#pragma once
#include <QJsonObject>
#include <QMainWindow>
#include <QMap>
#include <QSet>
#include <QVector>
class PpduTimelineView;
class PpduDetailWindow;
class QComboBox;
class QLabel;
class QTableWidget;
class QTabWidget;
class QGraphicsView;

class MeshvizWindow : public QMainWindow
{
  public:
    explicit MeshvizWindow(const QString& file);

    bool isValid() const
    {
        return m_valid;
    }

    bool exportViews(const QString& directory);

  private:
    void selectPpdu(uint32_t id);
    void selectPacket();
    void drawTopology();
    QString nodeName(int id) const;
    QString endpoint(quint64 mac) const;
    QString hopName(int id) const;
    QJsonObject m_run;
    QMap<int, QJsonObject> m_nodes, m_links, m_devices;
    QMap<quint64, QJsonObject> m_macDevices, m_ppdus;
    QMap<quint64, QVector<QJsonObject>> m_hops, m_receives;
    QMap<quint64, QVector<quint64>> m_packetPpdus;
    QVector<QJsonObject> m_metrics, m_totals;
    PpduTimelineView* m_timeline;
    PpduDetailWindow* m_detail;
    QComboBox* m_packets;
    QLabel* m_path;
    QLabel* m_status;
    QTableWidget* m_hopTable;
    QTableWidget* m_eventTable;
    QGraphicsView* m_topology;
    QTabWidget* m_tabs;
    QSet<int> m_selectedHops;
    quint64 m_selectedPpdu = 0;
    bool m_valid = false;
};
