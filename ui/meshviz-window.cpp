// SPDX-License-Identifier: MIT
#include "meshviz-window.h"

#include "mcs_distribution_chart.h"
#include "ppdu_detail_window.h"
#include "ppdu_timeline_view.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QtWidgets>
#include <algorithm>
#include <cmath>
#include <stdexcept>

namespace
{
QString
macText(quint64 v)
{
    QStringList p;
    for (int i = 5; i >= 0; --i)
    {
        p << QString("%1").arg((v >> (8 * i)) & 255, 2, 16, QChar('0'));
    }
    return p.join(':');
}

quint64
number(const QJsonObject& o, const char* key)
{
    return static_cast<quint64>(o[key].toDouble());
}

QColor
color(int i)
{
    static const QColor c[] = {QColor("#2878d4"),
                               QColor("#18a999"),
                               QColor("#a96bd4"),
                               QColor("#e69a35"),
                               QColor("#db6674")};
    return c[std::abs(i) % 5];
}

QTableWidget*
table(const QStringList& cols)
{
    auto* t = new QTableWidget(0, cols.size());
    t->setHorizontalHeaderLabels(cols);
    t->horizontalHeader()->setSectionResizeMode(QHeaderView::ResizeToContents);
    t->horizontalHeader()->setStretchLastSection(true);
    t->verticalHeader()->hide();
    t->setEditTriggers(QAbstractItemView::NoEditTriggers);
    t->setSelectionBehavior(QAbstractItemView::SelectRows);
    t->setAlternatingRowColors(true);
    return t;
}

void
row(QTableWidget* t, const QStringList& vals)
{
    int r = t->rowCount();
    t->insertRow(r);
    for (int c = 0; c < vals.size(); ++c)
    {
        t->setItem(r, c, new QTableWidgetItem(vals[c]));
    }
}

class TopologyView : public QGraphicsView
{
  protected:
    void resizeEvent(QResizeEvent* event) override
    {
        QGraphicsView::resizeEvent(event);
        if (scene())
        {
            fitInView(scene()->sceneRect(), Qt::KeepAspectRatio);
        }
    }

    void showEvent(QShowEvent* event) override
    {
        QGraphicsView::showEvent(event);
        if (scene())
        {
            fitInView(scene()->sceneRect(), Qt::KeepAspectRatio);
        }
    }
};

class SeriesChart : public QWidget
{
  public:
    QString title, unit, note;
    QMap<int, QVector<QPointF>> data;
    QMap<int, QString> labels;

    explicit SeriesChart(QWidget* p = nullptr)
        : QWidget(p)
    {
        setMinimumHeight(220);
        setMinimumWidth(270);
    }

    void paintEvent(QPaintEvent*) override
    {
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing);
        p.fillRect(rect(), Qt::white);
        p.setPen(QColor("#20364e"));
        auto f = p.font();
        f.setBold(true);
        f.setPointSize(11);
        p.setFont(f);
        p.drawText(QRectF(16, 10, width() - 32, 24), title);
        f.setBold(false);
        f.setPointSize(8);
        p.setFont(f);
        p.setPen(QColor("#748296"));
        p.drawText(QRectF(16, 35, width() - 32, 34), Qt::TextWordWrap, note);
        QRectF plot(55, 84, width() - 75, height() - 126);
        if (plot.height() < 30)
        {
            return;
        }
        double xmin = 1e100, xmax = -1e100, ymax = 0;
        for (auto points : data)
        {
            for (auto v : points)
            {
                xmin = std::min(xmin, v.x());
                xmax = std::max(xmax, v.x());
                if (std::isfinite(v.y()))
                {
                    ymax = std::max(ymax, v.y());
                }
            }
        }
        if (xmin > xmax)
        {
            p.drawText(plot, Qt::AlignCenter, "No samples");
            return;
        }
        if (xmin == xmax)
        {
            xmax = xmin + 0.1;
        }
        ymax = std::max(0.001, ymax * 1.12);
        for (int i = 0; i < 5; ++i)
        {
            double y = plot.bottom() - i * plot.height() / 4;
            p.setPen(QColor("#e9eef5"));
            p.drawLine(QPointF(plot.left(), y), QPointF(plot.right(), y));
            p.setPen(QColor("#718399"));
            p.drawText(QRectF(0, y - 8, 48, 16),
                       Qt::AlignRight,
                       QString::number(i * ymax / 4, 'g', 3));
        }
        p.drawText(QRectF(width() - 62, 14, 48, 15), Qt::AlignRight, unit);
        p.drawText(QRectF(plot.left(), plot.bottom() + 8, 100, 16),
                   QString::number(xmin, 'f', 2) + " s");
        p.drawText(QRectF(plot.right() - 90, plot.bottom() + 8, 90, 16),
                   Qt::AlignRight,
                   QString::number(xmax, 'f', 2) + " s");
        int legend = 0;
        for (auto it = data.begin(); it != data.end(); ++it)
        {
            QPainterPath path;
            bool first = true;
            QPointF last;
            for (auto v : it.value())
            {
                if (!std::isfinite(v.y()))
                {
                    first = true;
                    continue;
                }
                QPointF q(plot.left() + (v.x() - xmin) / (xmax - xmin) * plot.width(),
                          plot.bottom() - v.y() / ymax * plot.height());
                if (first)
                {
                    path.moveTo(q);
                    first = false;
                }
                else
                {
                    path.lineTo(q);
                }
                last = q;
            }
            p.setPen(QPen(color(it.key()), 2));
            p.drawPath(path);
            if (it.value().size() == 1 && std::isfinite(it.value()[0].y()))
            {
                p.setBrush(color(it.key()));
                p.drawEllipse(last, 3, 3);
            }
            p.drawText(QRectF(16 + legend * 145, 65, 142, 16), labels.value(it.key()));
            ++legend;
        }
    }
};
} // namespace

QString
MeshvizWindow::nodeName(int id) const
{
    return m_nodes.value(id)["name"].toString(QString("Node %1").arg(id));
}

QString
MeshvizWindow::endpoint(quint64 mac) const
{
    if (mac == 0xffffffffffffULL)
    {
        return "Broadcast";
    }
    auto d = m_macDevices.value(mac);
    if (d.isEmpty())
    {
        return macText(mac);
    }
    return QString("%1 / if%2 · %3")
        .arg(nodeName(d["node"].toInt()))
        .arg(d["device"].toInt())
        .arg(macText(mac));
}

QString
MeshvizWindow::hopName(int id) const
{
    auto e = m_links.value(id);
    return nodeName(e["from"].toInt()) + " → " + nodeName(e["to"].toInt());
}

MeshvizWindow::MeshvizWindow(const QString& file)
{
    setWindowTitle("MeshViz · " + QFileInfo(file).fileName());
    resize(1520, 1000);
    QFile input(file);
    if (!input.open(QIODevice::ReadOnly))
    {
        throw std::runtime_error(input.errorString().toStdString());
    }
    bool ended = false;
    qint64 line = 0;
    while (!input.atEnd())
    {
        QByteArray bytes = input.readLine();
        ++line;
        if (bytes.trimmed().isEmpty())
        {
            continue;
        }
        QJsonParseError error;
        auto doc = QJsonDocument::fromJson(bytes, &error);
        if (error.error != QJsonParseError::NoError || !doc.isObject())
        {
            throw std::runtime_error(QString("Invalid trace line %1: %2")
                                         .arg(line)
                                         .arg(error.errorString())
                                         .toStdString());
        }
        auto o = doc.object();
        auto type = o["type"].toString();
        if (type == "run")
        {
            if (!m_run.isEmpty() || o["schema"].toInt() != 1)
            {
                throw std::runtime_error("Expected exactly one schema-1 run");
            }
            m_run = o;
        }
        else if (type == "node")
        {
            m_nodes[o["node"].toInt()] = o;
        }
        else if (type == "device")
        {
            m_macDevices[number(o, "mac")] = o;
        }
        else if (type == "link")
        {
            m_links[o["id"].toInt()] = o;
        }
        else if (type == "ppdu")
        {
            auto id = number(o, "id");
            m_ppdus[id] = o;
            for (auto packet : o["packets"].toArray())
            {
                m_packetPpdus[quint64(packet.toDouble())].push_back(id);
            }
        }
        else if (type == "hop")
        {
            m_hops[number(o, "packet")].push_back(o);
        }
        else if (type == "rx" || type == "rx-drop")
        {
            m_receives[number(o, "ppdu")].push_back(o);
        }
        else if (type == "metric")
        {
            m_metrics.push_back(o);
        }
        else if (type == "total")
        {
            m_totals.push_back(o);
        }
        else if (type == "end")
        {
            ended = true;
            m_run["skippedPpdus"] = o["skippedPpdus"];
        }
    }
    if (m_run.isEmpty() || !ended)
    {
        throw std::runtime_error("Incomplete trace: run/end record missing");
    }
    m_valid = true;
    auto* center = new QWidget;
    setCentralWidget(center);
    auto* layout = new QVBoxLayout(center);
    layout->setSpacing(12);
    auto* heading = new QHBoxLayout;
    auto* title = new QLabel("MeshViz  /  单次仿真结果");
    title->setStyleSheet("font-size:22px;font-weight:700;color:#213c59");
    heading->addWidget(title);
    heading->addStretch();
    auto* exportButton = new QPushButton("导出当前视图 PNG");
    heading->addWidget(exportButton);
    layout->addLayout(heading);
    m_status = new QLabel(
        QString("ns-3.48  ·  %1 个物理节点  ·  %2 条链路  ·  %3 个 PPDU  ·  采集 %4–%5 s / 统计 "
                "%6–%7 s%8")
            .arg(m_nodes.size())
            .arg(m_links.size())
            .arg(m_ppdus.size())
            .arg(m_run["captureStartNs"].toDouble() / 1e9, 0, 'f', 3)
            .arg(m_run["captureEndNs"].toDouble() / 1e9, 0, 'f', 3)
            .arg(m_run["startNs"].toDouble() / 1e9, 0, 'f', 2)
            .arg(m_run["endNs"].toDouble() / 1e9, 0, 'f', 2)
            .arg(number(m_run, "skippedPpdus") ? "  ·  PPDU 上限已触发，详细路径可能不完整" : ""));
    layout->addWidget(m_status);
    m_tabs = new QTabWidget;
    layout->addWidget(m_tabs, 1);
    auto* timelinePage = new QWidget;
    auto* tl = new QVBoxLayout(timelinePage);
    auto* split = new QSplitter;
    m_timeline = new PpduTimelineView;
    m_detail = new PpduDetailWindow;
    m_timeline->setDetailWindow(m_detail);
    m_detail->setMinimumWidth(300);
    m_detail->setMaximumWidth(330);
    split->addWidget(m_timeline);
    split->addWidget(m_detail);
    split->setStretchFactor(0, 5);
    split->setStretchFactor(1, 1);
    tl->addWidget(split, 3);
    auto* charts = new QHBoxLayout;
    auto* total = new SeriesChart;
    total->title = "总吞吐 · 应用有效负载";
    total->unit = "Mbps";
    total->note = "所有主业务 PacketSink 合计；不重复累计中继流量";
    total->labels[0] = "Delivered goodput";
    for (auto o : m_totals)
    {
        total->data[0].append(QPointF(o["timeNs"].toDouble() / 1e9, o["mbps"].toDouble()));
    }
    auto* throughput = new SeriesChart;
    throughput->title = "每一跳吞吐";
    throughput->unit = "Mbps";
    throughput->note = "下一跳 IPv4 接收字节，含 IP 头；100 ms 分桶";
    auto* delay = new SeriesChart;
    delay->title = "每一跳时延";
    delay->unit = "ms";
    delay->note = "本跳 IPv4 Tx → Rx，含排队及重传；分桶均值";
    for (auto o : m_metrics)
    {
        int h = o["hop"].toInt();
        throughput->labels[h] = hopName(h);
        delay->labels[h] = hopName(h);
        double x = o["timeNs"].toDouble() / 1e9;
        throughput->data[h].append(QPointF(x, o["mbps"].toDouble()));
        delay->data[h].append(
            QPointF(x, o["meanDelayMs"].isNull() ? NAN : o["meanDelayMs"].toDouble()));
    }
    charts->addWidget(total);
    charts->addWidget(throughput);
    charts->addWidget(delay);
    tl->addLayout(charts, 2);
    m_tabs->addTab(timelinePage, "PPDU 时序 / 逐跳统计");
    auto* topologyPage = new QWidget;
    auto* topologyLayout = new QVBoxLayout(topologyPage);
    topologyLayout->addWidget(new QLabel("自动布局：由本次仿真的真实链路生成。实线 = 有线；虚线 = "
                                         "Wi-Fi；橙色 = OBSS；蓝色粗线 = 所选包已观测路径。"));
    m_topology = new TopologyView;
    m_topology->setRenderHint(QPainter::Antialiasing);
    topologyLayout->addWidget(m_topology, 1);
    auto* topologyTable = table({"链路", "节点", "介质", "发送接口 MAC", "接收接口 MAC"});
    topologyTable->setMaximumHeight(210);
    for (auto e : m_links)
    {
        row(topologyTable,
            {QString::number(e["id"].toInt()),
             hopName(e["id"].toInt()),
             e["wireless"].toBool() ? "Wi-Fi" : "Ethernet",
             macText(number(e, "sender")),
             macText(number(e, "receiver"))});
    }
    topologyLayout->addWidget(topologyTable);
    m_tabs->addTab(topologyPage, "网络布局");
    auto* life = new QWidget;
    auto* ll = new QVBoxLayout(life);
    auto* desc = new QLabel(
        "PPDU 生命周期 / 跨跳数据包路径\n每跳产生独立 PPDU；通过数据包追踪 ID 串联转发。有线跳显示 "
        "IP 事件。TCP 重传是新的 IP 发送，MAC 重传保留同一追踪 ID。");
    ll->addWidget(desc);
    m_packets = new QComboBox;
    ll->addWidget(m_packets);
    m_path = new QLabel("在时序图点击 PPDU，或选择数据包查看实际路径。");
    m_path->setWordWrap(true);
    m_path->setTextFormat(Qt::PlainText);
    m_path->setStyleSheet("padding:16px;background:#eaf2ff;font-weight:600");
    ll->addWidget(m_path);
    m_hopTable =
        table({"跳", "路径", "介质", "事件", "时间 (ms)", "本跳时延 (ms)", "字节", "分片偏移"});
    ll->addWidget(m_hopTable, 1);
    m_eventTable = table({"PPDU",
                          "发送端",
                          "接收端",
                          "开始 (ms)",
                          "时长 (µs)",
                          "MCS",
                          "MPDU",
                          "重传",
                          "接收结果 / SNR"});
    ll->addWidget(m_eventTable, 1);
    m_tabs->addTab(life, "生命周期");
    auto* radioPage = new QWidget;
    auto* radioLayout = new QVBoxLayout(radioPage);
    radioLayout->addWidget(new QLabel("捕获窗口内的 PPDU 按 MCS 分布；含管理、控制和 OBSS "
                                      "帧，Legacy 单独计数。筛选仅影响显示。"));
    auto* radioFilter = new QComboBox;
    radioFilter->addItem("全部物理节点", -1);
    for (auto id : m_nodes.keys())
    {
        radioFilter->addItem(nodeName(id), id);
    }
    radioLayout->addWidget(radioFilter);
    auto* mcsChart = new McsDistributionChartWidget;
    radioLayout->addWidget(mcsChart, 1);
    auto updateMcs = [this, radioFilter, mcsChart] {
        mcsChart->reset();
        const int node = radioFilter->currentData().toInt();
        for (auto p : m_ppdus)
        {
            if (node < 0 || p["node"].toInt() == node)
            {
                PpduVisualItem v{};
                v.mcs = p["mcs"].toInt(-1);
                mcsChart->appendPpdu(v);
            }
        }
    };
    connect(radioFilter, qOverload<int>(&QComboBox::currentIndexChanged), this, [updateMcs](int) {
        updateMcs();
    });
    updateMcs();
    m_tabs->addTab(radioPage, "MCS 分布");
    auto* openLife = new QPushButton("查看选中 PPDU 的生命周期 →");
    tl->addWidget(openLife);
    connect(openLife, &QPushButton::clicked, this, [this] { m_tabs->setCurrentIndex(2); });
    for (auto it = m_hops.begin(); it != m_hops.end(); ++it)
    {
        m_packets->addItem(
            QString("数据包 #%1 · %2 条逐跳事件").arg(it.key()).arg(it.value().size()),
            QVariant::fromValue(it.key()));
    }
    connect(m_packets, qOverload<int>(&QComboBox::currentIndexChanged), this, [this] {
        selectPacket();
    });
    for (auto it = m_macDevices.begin(); it != m_macDevices.end(); ++it)
    {
        auto d = it.value();
        auto name = nodeName(d["node"].toInt());
        auto mac = it.key();
        m_timeline->setIdentity(mac, d["node"].toInt(), name, d["device"].toInt());
        PpduVisualItem v{};
        v.recordType = RecordType::DeviceRole;
        v.nodeId = d["node"].toInt();
        v.roleMac = mac;
        v.nodeMac = mac;
        v.deviceRole = name.contains("STA") ? DeviceRole::Sta : DeviceRole::Ap;
        m_timeline->append(v);
    }
    for (auto o : m_ppdus)
    {
        PpduVisualItem v{};
        v.id = number(o, "id");
        v.nodeId = o["node"].toInt();
        v.linkId = o["device"].toInt();
        v.nodeMac = number(o, "sender");
        v.sender = number(o, "sender");
        v.receiver = number(o, "receiver");
        v.txStartNs = number(o, "startNs");
        v.txEndNs = number(o, "endNs");
        v.durationNs = v.txEndNs - v.txStartNs;
        v.channel_number = o["channel"].toInt();
        v.mcs = o["mcs"].toInt(-1);
        v.frameType = o["frame"].toString().toStdString();
        v.size = o["bytes"].toInt();
        v.mpduAggregation = o["mpdus"].toInt();
        v.phyState = PhyStateKind::Tx;
        v.throughputMbpsX100 = v.durationNs ? uint32_t(v.size * 8.0 * 1e5 / v.durationNs) : 0;
        auto rx = m_receives.value(v.id);
        if (!rx.isEmpty())
        {
            auto r = rx.first();
            v.rxState = r["failed"].toInt() ? RxState::DecodeFail : RxState::Success;
            v.snrDbX10 = std::clamp(int(r["snrDb"].toDouble() * 10), 0, 65535);
        }
        m_timeline->append(v);
    }
    connect(m_timeline, &PpduTimelineView::ppduSelected, this, &MeshvizWindow::selectPpdu);
    connect(exportButton, &QPushButton::clicked, this, [this] {
        auto file = QFileDialog::getSaveFileName(this, "导出视图", "meshviz.png", "PNG (*.png)");
        if (file.isEmpty())
        {
            return;
        }
        if (!m_tabs->currentWidget()->grab().save(file))
        {
            QMessageBox::warning(this, "导出失败", file);
        }
    });
    setStyleSheet(
        "QMainWindow,QWidget{font-family:'Noto Sans CJK SC','Sans Serif';font-size:12px;} "
        "QMainWindow{background:#f1f5fa;} QTabWidget::pane{border:1px solid "
        "#d9e3ef;background:white;} QTabBar::tab{padding:10px 20px;background:#e8eef6;} "
        "QTabBar::tab:selected{background:white;color:#216cc6;} QPushButton{padding:8px "
        "16px;border:1px solid #cfdaea;border-radius:6px;background:#f8fbff;color:#254c75;} "
        "QTableWidget{background:white;alternate-background-color:#f6f9fe;gridline-color:#e0e7f0;"
        "border:1px solid #dbe4f0;} "
        "QHeaderView::section{padding:6px;background:#eaf0f8;border:0;font-weight:600;}");
    drawTopology();
    selectPacket();
    QTimer::singleShot(50, this, [this] {
        m_timeline->fitInitial();
        quint64 best = 0;
        int score = -1;
        for (auto it = m_hops.begin(); it != m_hops.end(); ++it)
        {
            QSet<int> received;
            for (auto h : it.value())
            {
                if (h["event"] == "RX")
                {
                    received.insert(h["hop"].toInt());
                }
            }
            if (received.size() > score && !m_packetPpdus.value(it.key()).isEmpty())
            {
                best = it.key();
                score = received.size();
            }
        }
        if (best)
        {
            m_timeline->selectId(m_packetPpdus.value(best).first());
        }
    });
}

void
MeshvizWindow::selectPpdu(uint32_t id)
{
    m_selectedPpdu = id;
    const auto ids = m_ppdus.value(id)["packets"].toArray();
    m_packets->blockSignals(true);
    m_packets->clear();
    for (auto p : ids)
    {
        m_packets->addItem(QString("PPDU #%1 内的数据包 #%2").arg(id).arg(quint64(p.toDouble())),
                           QVariant::fromValue(quint64(p.toDouble())));
    }
    m_packets->blockSignals(false);
    selectPacket();
    if (ids.isEmpty())
    {
        m_path->setText(
            QString("PPDU #%1：%2 → %3\n该帧不承载已追踪的主业务数据（可能是管理帧、ACK、探测或 "
                    "OBSS）。没有可推断的跨跳路径。")
                .arg(id)
                .arg(endpoint(number(m_ppdus[id], "sender")))
                .arg(endpoint(number(m_ppdus[id], "receiver"))));
    }
}

void
MeshvizWindow::selectPacket()
{
    auto id = m_packets->currentData().toULongLong();
    m_hopTable->setRowCount(0);
    m_eventTable->setRowCount(0);
    m_selectedHops.clear();
    auto hops = m_hops.value(id);
    std::sort(hops.begin(), hops.end(), [](auto a, auto b) {
        return number(a, "timeNs") < number(b, "timeNs");
    });
    QStringList path;
    QSet<int> seen;
    for (auto h : hops)
    {
        int hid = h["hop"].toInt();
        auto e = m_links.value(hid);
        m_selectedHops.insert(hid);
        if (!seen.contains(hid))
        {
            seen.insert(hid);
            if (path.isEmpty() || path.last() != endpoint(number(e, "sender")))
            {
                path << endpoint(number(e, "sender"));
            }
            path << endpoint(number(e, "receiver"));
        }
        row(m_hopTable,
            {QString::number(hid),
             hopName(hid),
             e["wireless"].toBool() ? "Wi-Fi" : "Ethernet",
             h["event"].toString(),
             QString::number(h["timeNs"].toDouble() / 1e6, 'f', 6),
             h["delayMs"].toDouble() < 0 ? "—" : QString::number(h["delayMs"].toDouble(), 'f', 6),
             QString::number(h["bytes"].toInt()),
             QString::number(h["fragment"].toInt())});
    }
    m_path->setText(QString("数据包 #%1 · 已观测路径（采集窗口边缘可能不完整）\n%2")
                        .arg(id)
                        .arg(path.isEmpty() ? "暂无主业务逐跳事件" : path.join("\n→ ")));
    QVector<quint64> ppdus = m_packetPpdus.value(id);
    if (ppdus.isEmpty() && m_selectedPpdu)
    {
        ppdus.append(m_selectedPpdu);
    }
    for (auto pid : ppdus)
    {
        auto p = m_ppdus.value(pid);
        QStringList result;
        for (auto r : m_receives.value(pid))
        {
            result << QString("%1: %2 OK / %3 fail · %4 dB%5")
                          .arg(nodeName(r["node"].toInt()))
                          .arg(r["ok"].toInt())
                          .arg(r["failed"].toInt())
                          .arg(r["snrDb"].isNull() ? QString("N/A")
                                                   : QString::number(r["snrDb"].toDouble(), 'f', 1))
                          .arg(r.contains("reason") ? " · " + r["reason"].toString() : "");
        }
        row(m_eventTable,
            {QString::number(pid),
             endpoint(number(p, "sender")),
             endpoint(number(p, "receiver")),
             QString::number(p["startNs"].toDouble() / 1e6, 'f', 6),
             QString::number((p["endNs"].toDouble() - p["startNs"].toDouble()) / 1e3, 'f', 2),
             p["mcs"].toInt() < 0 ? "Legacy" : QString::number(p["mcs"].toInt()),
             QString::number(p["mpdus"].toInt()),
             p["retry"].toBool() ? "Yes" : "No",
             result.isEmpty() ? "未观测接收结果" : result.join("; ")});
    }
    drawTopology();
}

void
MeshvizWindow::drawTopology()
{
    auto* scene = new QGraphicsScene(m_topology);
    auto* old = m_topology->scene();
    m_topology->setScene(scene);
    delete old;
    scene->setBackgroundBrush(Qt::white);
    // Stable topological layers; disconnected OBSS components get their own rows.
    QMap<int, int> depth;
    QMap<int, QVector<int>> children;
    QSet<int> targets;
    for (auto e : m_links)
    {
        children[e["from"].toInt()].append(e["to"].toInt());
        targets.insert(e["to"].toInt());
    }
    QVector<int> queue;
    for (auto id : m_nodes.keys())
    {
        if (!targets.contains(id))
        {
            depth[id] = 0;
            queue.append(id);
        }
    }
    for (int i = 0; i < queue.size(); ++i)
    {
        for (int next : children.value(queue[i]))
        {
            if (!depth.contains(next))
            {
                depth[next] = depth[queue[i]] + 1;
                queue.append(next);
            }
        }
    }
    QMap<int, QPointF> pos;
    QSet<int> visiting;
    double nextRow = 0;
    std::function<double(int)> place = [&](int id) -> double {
        if (pos.contains(id))
        {
            return pos[id].y();
        }
        if (visiting.contains(id))
        {
            return nextRow * 155;
        }
        visiting.insert(id);
        auto kids = children.value(id);
        double y = 0;
        if (kids.isEmpty())
        {
            y = 110 + 155 * nextRow++;
        }
        else
        {
            double first = place(kids.first()), last = first;
            for (int k = 1; k < kids.size(); ++k)
            {
                last = place(kids[k]);
            }
            y = (first + last) / 2;
        }
        pos[id] = QPointF(100 + depth.value(id, 0) * 290, y);
        visiting.remove(id);
        return y;
    };
    for (auto id : m_nodes.keys())
    {
        if (!targets.contains(id))
        {
            place(id);
            nextRow += 0.35;
        }
    }
    for (auto id : m_nodes.keys())
    {
        if (!pos.contains(id))
        {
            place(id);
        }
    }

    for (auto e : m_links)
    {
        auto a = pos[e["from"].toInt()], b = pos[e["to"].toInt()];
        int id = e["id"].toInt();
        QColor c = m_selectedHops.contains(id)
                       ? QColor("#2878d4")
                       : (e["background"].toBool() ? QColor("#e69a35") : QColor("#7e8c9d"));
        QPen pen(c,
                 m_selectedHops.contains(id) ? 3.5 : 2,
                 e["wireless"].toBool() ? Qt::DashLine : Qt::SolidLine);
        scene->addLine(QLineF(a, b), pen);
        auto* text = scene->addText(
            QString("%1 · hop %2").arg(e["wireless"].toBool() ? "Wi-Fi" : "Ethernet").arg(id));
        text->setDefaultTextColor(c);
        text->setPos((a + b) / 2 + QPointF(-55, -28));
    }
    for (auto id : m_nodes.keys())
    {
        auto p = pos[id];
        auto* box = scene->addRect(QRectF(p.x() - 66, p.y() - 28, 132, 58),
                                   QPen(QColor("#b6cbe1")),
                                   QBrush(QColor("#f3f8ff")));
        auto* t = scene->addText(nodeName(id));
        QFont f = t->font();
        f.setBold(true);
        f.setPointSize(12);
        t->setFont(f);
        t->setDefaultTextColor(QColor("#234b75"));
        t->setPos(p.x() - t->boundingRect().width() / 2, p.y() - 19);
        auto n = m_nodes[id];
        QStringList macs;
        for (auto d : m_macDevices)
        {
            if (d["node"].toInt() == id)
            {
                macs << QString("if%1 %2").arg(d["device"].toInt()).arg(macText(number(d, "mac")));
            }
        }
        auto* sub = scene->addText(QString("Node %1 · %2 interfaces").arg(id).arg(macs.size()));
        sub->setDefaultTextColor(QColor("#74869b"));
        sub->setPos(p.x() - 75, p.y() + 32);
        box->setToolTip(macs.join('\n'));
        t->setToolTip(macs.join('\n'));
    }
    scene->setSceneRect(scene->itemsBoundingRect().adjusted(-45, -50, 45, 50));
    QTimer::singleShot(0, this, [this] {
        m_topology->fitInView(m_topology->scene()->sceneRect(), Qt::KeepAspectRatio);
    });
}

bool
MeshvizWindow::exportViews(const QString& directory)
{
    QDir d;
    if (!d.mkpath(directory))
    {
        return false;
    }
    for (int i = 0; i < m_tabs->count(); ++i)
    {
        m_tabs->setCurrentIndex(i);
        QApplication::processEvents();
        if (i == 1)
        {
            m_topology->fitInView(m_topology->scene()->sceneRect(), Qt::KeepAspectRatio);
        }
        QApplication::processEvents();
        if (!grab().save(directory + QString("/view-%1.png").arg(i)))
        {
            return false;
        }
    }
    return true;
}
