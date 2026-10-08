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
class QTextBrowser;
class QTimer;
class QPushButton;

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
    /** Build the searchable PPDU and per-hop inspection page. */
    void createJumpPage();
    /** Synchronize jump with the selected PPDU and tracked packet. */
    void updateJump();
    /** Show all captured fields for the selected hop. */
    void showJumpDetails();
    /** Advance within the selected packet's chronological PPDU sequence.
     * @param direction Signed step direction.
     */
    void stepJump(int direction);
    QString nodeName(int id) const;
    QString endpoint(quint64 mac) const;
    QString hopName(int id) const;
    QJsonObject m_run;
    QMap<int, QJsonObject> m_nodes, m_links, m_devices;
    QMap<quint64, QJsonObject> m_macDevices, m_ppdus, m_packetInfo;
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
    QWidget* m_jumpPage = nullptr;         ///< Jump tab.
    QComboBox* m_jumpPackets = nullptr;    ///< Aggregate member selector.
    QTableWidget* m_jumpList = nullptr;    ///< Searchable PPDU inventory.
    QTableWidget* m_jumpHops = nullptr;    ///< One row per observed IP hop.
    QTextBrowser* m_jumpDetails = nullptr; ///< Full hop and radio events.
    QLabel* m_jumpPath = nullptr;          ///< Selected packet path.
    QTimer* m_replay = nullptr;            ///< Offline step playback timer.
    QPushButton* m_play = nullptr;         ///< Playback toggle.
    QSet<int> m_selectedHops;
    int m_activeHop = -1; ///< Hop currently inspected in jump.
    quint64 m_selectedPpdu = 0;
    bool m_valid = false;
};
