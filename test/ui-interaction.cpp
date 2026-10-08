// SPDX-License-Identifier: MIT
#include "meshviz-window.h"

#include <QtTest>
#include <QtWidgets>

/** Exercise the public UI through mouse, keyboard, and timer events. */
class InteractionTest : public QObject
{
    Q_OBJECT
  private slots:

    void aggregateSelection()
    {
        QFile file(qEnvironmentVariable("MESHVIZ_TEST_TRACE"));
        QVERIFY(file.open(QIODevice::ReadOnly));
        QJsonObject largest;
        QMap<quint64, QSet<int>> expected;
        while (!file.atEnd())
        {
            auto object = QJsonDocument::fromJson(file.readLine()).object();
            if (object["type"] == "ppdu" &&
                object["packets"].toArray().size() > largest["packets"].toArray().size())
            {
                largest = object;
            }
            if (object["type"] == "hop")
            {
                expected[quint64(object["packet"].toDouble())].insert(object["hop"].toInt());
            }
        }
        if (largest["packets"].toArray().size() < 2)
        {
            return;
        }
        MeshvizWindow window(qEnvironmentVariable("MESHVIZ_TEST_TRACE"));
        window.show();
        QTest::qWait(120);
        auto* tabs = window.findChild<QTabWidget*>();
        tabs->setCurrentIndex(tabs->count() - 1);
        auto* list = window.findChild<QTableWidget*>("jumpList");
        auto* packets = window.findChild<QComboBox*>("jumpPackets");
        auto* hops = window.findChild<QTableWidget*>("jumpHops");
        int row = -1;
        for (int r = 0; r < list->rowCount(); ++r)
        {
            if (list->item(r, 0)->text().toULongLong() == quint64(largest["id"].toDouble()))
            {
                row = r;
            }
        }
        QVERIFY(row >= 0);
        list->scrollToItem(list->item(row, 0));
        QTest::mouseClick(list->viewport(),
                          Qt::LeftButton,
                          Qt::NoModifier,
                          list->visualItemRect(list->item(row, 0)).center());
        QCOMPARE(packets->count(), largest["packets"].toArray().size());
        packets->setFocus();
        QTest::keyClick(packets, Qt::Key_End);
        auto id = packets->currentData().toULongLong();
        QCOMPARE(id, quint64(largest["packets"].toArray().last().toDouble()));
        QCOMPARE(hops->rowCount(), expected.value(id).size());
        QVERIFY(
            window.findChild<QLabel*>("jumpPath")->text().contains(QString("数据包 #%1").arg(id)));
        auto* next = window.findChild<QPushButton*>("jumpNext");
        QTest::mouseClick(next, Qt::LeftButton);
        QCOMPARE(packets->currentData().toULongLong(), id);
    }

    void inspectAndReplay()
    {
        // Reproduce a dark host theme; the viewer must override its text palette.
        QPalette dark;
        dark.setColor(QPalette::Text, Qt::white);
        dark.setColor(QPalette::WindowText, Qt::white);
        QApplication::setPalette(dark);
        MeshvizWindow window(qEnvironmentVariable("MESHVIZ_TEST_TRACE"));
        window.show();
        QTest::qWait(120);
        auto* tabs = window.findChild<QTabWidget*>();
        QVERIFY(tabs);
        int jump = -1;
        for (int i = 0; i < tabs->count(); ++i)
        {
            if (tabs->tabText(i) == "jump")
            {
                jump = i;
            }
        }
        QVERIFY(jump >= 0);
        QTest::mouseClick(tabs->tabBar(),
                          Qt::LeftButton,
                          Qt::NoModifier,
                          tabs->tabBar()->tabRect(jump).center());
        auto* hops = window.findChild<QTableWidget*>("jumpHops");
        auto* list = window.findChild<QTableWidget*>("jumpList");
        auto* details = window.findChild<QTextBrowser*>("jumpDetails");
        auto* packets = window.findChild<QComboBox*>("jumpPackets");
        QVERIFY(hops->rowCount() >= 2);
        QVERIFY(packets->count() > 0);
        auto packet = packets->currentData();
        QVERIFY(list->palette().color(QPalette::Text).lightness() < 150);
        QVERIFY(details->palette().color(QPalette::Text).lightness() < 150);
        auto* next = window.findChild<QPushButton*>("jumpNext");
        auto* play = window.findChild<QPushButton*>("jumpPlay");
        auto initial = list->item(list->currentRow(), 0)->text();
        QTest::mouseClick(next, Qt::LeftButton);
        QVERIFY(initial != list->item(list->currentRow(), 0)->text());
        QCOMPARE(packets->currentData(), packet);
        QTest::mouseClick(play, Qt::LeftButton);
        QCOMPARE(play->text(), QString("暂停"));
        auto before = list->item(list->currentRow(), 0)->text();
        QTest::qWait(760);
        QVERIFY(before != list->item(list->currentRow(), 0)->text());
        if (play->text() == "暂停")
        {
            QTest::mouseClick(play, Qt::LeftButton);
        }
        int last = hops->rowCount() - 1;
        QTest::mouseClick(hops->viewport(),
                          Qt::LeftButton,
                          Qt::NoModifier,
                          hops->visualItemRect(hops->item(last, 0)).center());
        QCOMPARE(hops->currentRow(), last);
        QVERIFY(details->toPlainText().contains("MAC 重传"));
        QVERIFY(details->toPlainText().contains("SNR"));
        QVERIFY(details->toPlainText().contains("IP 逐跳事件"));
        auto* search = window.findChild<QLineEdit*>("jumpSearch");
        QTest::keyClicks(search, "CTL_ACK");
        int visible = 0;
        for (int r = 0; r < list->rowCount(); ++r)
        {
            if (!list->isRowHidden(r))
            {
                ++visible;
                QVERIFY(list->item(r, 2)->text().contains("CTL_ACK"));
            }
        }
        QVERIFY(visible > 0);
        for (int r = 0; r < list->rowCount(); ++r)
        {
            if (!list->isRowHidden(r))
            {
                list->scrollToItem(list->item(r, 0));
                QTest::mouseClick(list->viewport(),
                                  Qt::LeftButton,
                                  Qt::NoModifier,
                                  list->visualItemRect(list->item(r, 0)).center());
                break;
            }
        }
        QCOMPARE(packets->count(), 0);
        QCOMPARE(hops->rowCount(), 0);
        QVERIFY(details->toPlainText().contains("PPDU #"));
        QVERIFY(window.findChild<QLabel*>("jumpPath")->text().contains("不推断跨跳路径"));
        search->clear();

        tabs->setCurrentIndex(0);
        QTest::qWait(20);
        for (const char* name : {"totalChart", "hopChart", "delayChart"})
        {
            auto* chart = window.findChild<QWidget*>(name);
            QVERIFY(chart);
            QPoint point(chart->width() / 2, chart->height() / 2);
            QTest::mouseMove(chart, point);
            QTRY_VERIFY(QToolTip::text().contains("s："));
            QVERIFY(QToolTip::text().contains(QString(name) == "delayChart" ? "ms" : "Mbps"));
            QVERIFY(QToolTip::text().contains("["));
            if (QString(name) == "delayChart")
            {
                QVERIFY(QToolTip::text().contains("个样本"));
            }
            auto original = chart->grab().toImage();
            QWheelEvent wheel(point,
                              chart->mapToGlobal(point),
                              QPoint(),
                              QPoint(0, 120),
                              Qt::NoButton,
                              Qt::NoModifier,
                              Qt::NoScrollPhase,
                              false);
            QApplication::sendEvent(chart, &wheel);
            QVERIFY(original != chart->grab().toImage());
            QTest::mouseDClick(chart, Qt::LeftButton, Qt::NoModifier, point);
            QCOMPARE(original, chart->grab().toImage());
        }
        tabs->setCurrentIndex(1);
        QTest::qWait(20);
        auto* topology = window.findChild<QGraphicsView*>("topology");
        auto transform = topology->transform();
        QPoint point = topology->viewport()->rect().center();
        QWheelEvent wheel(point,
                          topology->viewport()->mapToGlobal(point),
                          QPoint(),
                          QPoint(0, 120),
                          Qt::NoButton,
                          Qt::NoModifier,
                          Qt::NoScrollPhase,
                          false);
        QApplication::sendEvent(topology->viewport(), &wheel);
        QVERIFY(transform != topology->transform());
        QTest::mouseDClick(topology->viewport(), Qt::LeftButton, Qt::NoModifier, point);
        QCOMPARE(transform, topology->transform());
    }
};
QTEST_MAIN(InteractionTest)
#include "ui-interaction.moc"
