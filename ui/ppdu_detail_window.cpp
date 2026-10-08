#include "ppdu_detail_window.h"

#include <QFrame>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QVBoxLayout>

PpduDetailWindow::PpduDetailWindow(QWidget* parent)
    : QWidget(parent)
{
    setMinimumWidth(280);
    setMinimumHeight(450);
    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(16, 10, 12, 10);
    root->setSpacing(6);
    auto* title = new QLabel("PPDU Inspector");
    title->setStyleSheet("font-size:19px;font-weight:700;color:#203c5b;");
    root->addWidget(title);
    auto make = [&]() {
        auto* l = new QLabel("—");
        l->setTextFormat(Qt::PlainText);
        l->setTextInteractionFlags(Qt::TextSelectableByMouse);
        l->setStyleSheet("color:#203c5b;border:0;background:transparent;");
        return l;
    };
    m_frameType = make();
    m_statusBadge = make();
    auto* head = new QHBoxLayout;
    head->addWidget(m_frameType, 1);
    head->addWidget(m_statusBadge);
    root->addLayout(head);
    m_senderTitle = make();
    m_senderMac = make();
    m_receiverTitle = make();
    m_receiverMac = make();
    for (auto* l : {m_senderTitle, m_senderMac, m_receiverTitle, m_receiverMac})
    {
        root->addWidget(l);
    }
    auto* grid = new QGridLayout;
    grid->setVerticalSpacing(7);
    int r = 0;
    auto field = [&](const QString& label, QLabel** out) {
        auto* key = new QLabel(label);
        key->setStyleSheet("color:#6a7f97;");
        *out = make();
        grid->addWidget(key, r, 0);
        grid->addWidget(*out, r++, 1);
    };
    field("信道", &m_channelValue);
    field("物理节点", &m_nodeValue);
    field("开始", &m_startValue);
    field("结束", &m_endValue);
    field("时长", &m_durationValue);
    field("PSDU 大小", &m_sizeValue);
    field("MPDU 聚合数", &m_mpduValue);
    field("MCS", &m_mcsValue);
    field("空口发送速率", &m_throughputValue);
    field("接收 SNR", &m_snrValue);
    root->addLayout(grid);
    m_collisionValue = make();
    m_collisionValue->setWordWrap(true);
    root->addWidget(m_collisionValue);
    root->addStretch();
    clearDetails();
}

void
PpduDetailWindow::clearDetails()
{
    showDetails("Sender",
                "N/A",
                "Receiver",
                "N/A",
                "No PPDU Selected",
                "N/A",
                "#5C6D7E",
                "N/A",
                "N/A",
                "N/A",
                "N/A",
                "N/A",
                "N/A",
                "N/A",
                "N/A",
                "N/A",
                "N/A",
                "N/A");
}

void
PpduDetailWindow::showDetails(const QString& senderTitle,
                              const QString& senderMac,
                              const QString& receiverTitle,
                              const QString& receiverMac,
                              const QString& frameType,
                              const QString& statusText,
                              const QString& statusAccent,
                              const QString& channelText,
                              const QString& nodeText,
                              const QString& startText,
                              const QString& endText,
                              const QString& durationText,
                              const QString& sizeText,
                              const QString& mpduText,
                              const QString& mcsText,
                              const QString& throughputText,
                              const QString& snrText,
                              const QString& collisionText)
{
    m_senderTitle->setText(senderTitle);
    m_senderMac->setText(senderMac);
    m_receiverTitle->setText(receiverTitle);
    m_receiverMac->setText(receiverMac);
    m_frameType->setText(frameType);
    m_statusBadge->setText(statusText);
    m_statusBadge->setStyleSheet(QString("QLabel {"
                                         "  color: white;"
                                         "  background: %1;"
                                         "  border-radius: 10px;"
                                         "  padding: 4px 10px;"
                                         "  font-size: 11px;"
                                         "  font-weight: 700;"
                                         "}")
                                     .arg(statusAccent));

    m_channelValue->setText(channelText);
    m_nodeValue->setText(nodeText);
    m_startValue->setText(startText);
    m_endValue->setText(endText);
    m_durationValue->setText(durationText);
    m_sizeValue->setText(sizeText);
    m_mpduValue->setText(mpduText);
    m_mcsValue->setText(mcsText);
    m_throughputValue->setText(throughputText);
    m_snrValue->setText(snrText);
    m_collisionValue->setText(collisionText);
}
