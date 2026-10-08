/*
 * MESH_TEST version notes
 * -----------------------
 * MESH_TEST.cc
 *   - Baseline script for 8 mesh topology modes and 3 STA association modes.
 *   - Reports end-to-end downlink TCP throughput from ONT to STA.
 *
 * MESH_TEST_2.cc
 *   - Incremental development version based on MESH_TEST.cc.
 *   - Keeps the same topology construction, traffic model, and end-to-end throughput logic.
 *   - Adds per-hop throughput statistics for the active forwarding path.
 *   - Per-hop throughput is measured at the IPv4 receive side of each hop receiver interface,
 *     filtered to the forward TCP data packets destined to the STA.
 *   - Aligns BE A-MPDU/A-MSDU sizes and Wi-Fi MAC queue size with the single AP/STA
 *     high-throughput reference configuration.
 *   - This version is intended for path-level diagnosis, while MESH_TEST.cc remains the
 *     reference baseline script.
 *
 * mesh_test_obss_metrics.cc
 *   - Incremental diagnostics version based on mesh_test_obss.cc.
 *   - Keeps the same topology, traffic, OBSS and throughput calculation logic.
 *   - Adds summary-level theory-model metrics for each active hop: MCS, PHY/data rate,
 *     MPDU/PPDU success ratio, estimated downlink TX airtime, aggregation size, RSSI/SNR.
 *   - Adds global TCP ACK/RTT counters and AP-side observations of OBSS frames.
 *   - Intended for validating metric reasonableness before running the full grid scan.
 */

#include "ns3/applications-module.h"
#include "ns3/meshviz-helper.h"
#include <memory>
#include "ns3/core-module.h"
#include "ns3/csma-module.h"
#include "ns3/internet-module.h"
#include "ns3/mobility-module.h"
#include "ns3/network-module.h"
#include "ns3/seq-ts-header.h"
#include "ns3/tcp-header.h"
#include "ns3/tcp-socket-base.h"
#include "ns3/wifi-module.h"
#include "ns3/yans-wifi-helper.h"

#include <algorithm>
#include <cmath>
#include <fstream>
#include <filesystem>
#include <iomanip>
#include <limits>
#include <map>
#include <numeric>
#include <sstream>
#include <string>
#include <tuple>
#include <vector>

using namespace ns3;

NS_LOG_COMPONENT_DEFINE("MESH_TEST_OBSS_METRICS");

static constexpr uint32_t kDefaultBeMaxAmpduSize = 15523200;
static constexpr uint16_t kDefaultBeMaxAmsduSize = 11398;

enum class TopologyType
{
  STAR_L1,
  CHAIN_L2,
};

struct ModeConfig
{
  TopologyType topology;
  bool linkAIsWired;
  bool linkBIsWired;
  std::string name;
};

static uint32_t g_mode = 1;
static std::string g_staAssoc = "ap1"; // ont | ap1 | ap2
static uint32_t g_run = 1;
static double g_prewarm = 10.0;
static double g_test = 10.0;
static uint32_t g_tcpStreams = 20;
static std::string g_trafficType = "tcp";
static std::string g_appRate = "1Gbps";
static uint32_t g_payloadSize = 1500;
static uint16_t g_basePort = 5000;

static uint32_t g_nss = 2;
static uint32_t g_giNs = 800;
static uint32_t g_channelWidth = 160;
static uint16_t g_chanNum = 50;
static double g_txPowerDbm = 20.0;
static uint32_t g_rtsCtsThreshold = 999999;
static std::string g_wiredRate = "10Gbps";
static bool g_enableAmpdu = true;
static bool g_enableAmsdu = true;

static bool g_enablePcap = false;
static std::string g_outCsv = "mesh_test_2.csv";
static bool g_useStaPosOverride = false;
static double g_staX = 0.0;
static double g_staY = 0.0;
static bool g_enableObss = false;
static bool g_enableObss1 = true;
static bool g_enableObss2 = false;
static double g_obssTargetDuty = 0.15;
static double g_obssLinkRateMbps = 1600.0;
static std::string g_obssRate = "150Mbps";
static uint32_t g_obssPacketSize = 1472;
static double g_obssApX = 4.0;
static double g_obssApY = 4.0;
static double g_obssStaX = 5.0;
static double g_obssStaY = 4.0;
static double g_obssAp2X = 8.0;
static double g_obssAp2Y = 6.0;
static double g_obssSta2X = 9.0;
static double g_obssSta2Y = 6.0;
static double g_hopRttProbeInterval = 0.05;
static uint32_t g_hopRttProbeSize = 64;

static std::vector<Ptr<PacketSink>> g_sinks;
static std::vector<uint64_t> g_bytesSnapshot;
static std::ofstream g_csv;
static Ipv4Address g_flowDstIp = Ipv4Address::GetAny();

struct HopStat
{
  std::string name;
  Ptr<Ipv4> ipv4;
  uint32_t interface = 0;
  uint64_t totalBytes = 0;
  uint64_t snapshotBytes = 0;
};

static std::vector<HopStat> g_hopStats;

struct PhyAirtimeStat
{
  Ptr<WifiPhy> phy;
  uint32_t activeTx = 0;
  Time txStart = Seconds(0);
  Time total = Seconds(0);
  Time snapshot = Seconds(0);
};

static std::vector<PhyAirtimeStat> g_obssAirtimeStats;

struct HopRadioMetric
{
  std::string name;
  bool wireless = false;
  Ptr<WifiNetDevice> txWifiDev;
  Ptr<WifiNetDevice> rxWifiDev;
  Mac48Address txMac;
  Mac48Address rxMac;
  uint64_t txPpdus = 0;
  uint64_t txMpdus = 0;
  uint64_t txBytes = 0;
  uint64_t txAirtimeNs = 0;
  uint64_t amsduMsdus = 0;
  uint64_t mcsSamples = 0;
  double mcsSum = 0.0;
  double phyRateMbpsSum = 0.0;
  double dataRateMbpsSum = 0.0;
  uint64_t rxPpdus = 0;
  uint64_t rxFailedPpdus = 0;
  uint64_t rxMpdusOk = 0;
  uint64_t rxMpdusFail = 0;
  uint64_t rxSignalSamples = 0;
  double rxSnrDbSum = 0.0;
  double rxRssiDbmSum = 0.0;
};

struct ObssObserverMetric
{
  std::string observerName;
  uint64_t frames = 0;
  uint64_t bytes = 0;
  double rssiDbmSum = 0.0;
  double snrDbSum = 0.0;
  uint64_t rxPpdus = 0;
  uint64_t rxFailedPpdus = 0;
  uint64_t rxAirtimeNs = 0;
};

struct TcpMetric
{
  uint64_t ackPackets = 0;
  uint64_t ackBytes = 0;
  uint64_t dataPackets = 0;
  uint64_t dataBytes = 0;
  uint64_t retransPackets = 0;
  uint64_t rttSamples = 0;
  double rttMsSum = 0.0;
  uint64_t lastRttSamples = 0;
  double lastRttMsSum = 0.0;
};

static std::vector<HopRadioMetric> g_radioMetrics;
static std::map<std::string, ObssObserverMetric> g_obssObservers;
static std::vector<Mac48Address> g_obssTxMacs;
static TcpMetric g_tcpMetric;
static bool g_collectMetrics = false;

struct HopRttMetric
{
  std::string name;
  Ptr<Socket> clientSocket;
  Ptr<Socket> serverSocket;
  Ipv4Address dstIp;
  uint16_t port = 0;
  uint32_t nextSeq = 0;
  uint32_t sent = 0;
  uint32_t received = 0;
  std::vector<double> rttsMs;
};

static std::vector<HopRttMetric> g_hopRttMetrics;

static double
SafeRatio(double numerator, double denominator)
{
  return denominator > 0.0 ? numerator / denominator : 0.0;
}

static bool
IsDataMpdu(const Ptr<const WifiMpdu>& mpdu)
{
  const WifiMacHeader& hdr = mpdu->GetHeader();
  return hdr.IsData() || hdr.IsQosData();
}

static uint64_t
CountMsdusInMpdu(const Ptr<const WifiMpdu>& mpdu)
{
  const WifiMacHeader& hdr = mpdu->GetHeader();
  if (!hdr.IsQosData() || !hdr.IsQosAmsdu())
  {
    return IsDataMpdu(mpdu) ? 1 : 0;
  }
  return static_cast<uint64_t>(std::distance(mpdu->begin(), mpdu->end()));
}

static bool
PsduMatchesDownlinkData(Ptr<const WifiPsdu> psdu, const Mac48Address& txMac, const Mac48Address& rxMac)
{
  if (!psdu || psdu->GetAddr1() != rxMac || psdu->GetAddr2() != txMac)
  {
    return false;
  }

  for (const auto& mpdu : *PeekPointer(psdu))
  {
    if (IsDataMpdu(mpdu))
    {
      return true;
    }
  }
  return false;
}

static bool
PsduTransmittedByObss(Ptr<const WifiPsdu> psdu)
{
  return psdu &&
         std::find(g_obssTxMacs.begin(), g_obssTxMacs.end(), psdu->GetAddr2()) != g_obssTxMacs.end();
}

static bool
PeekWifiMacHeaderFromMonitorPacket(Ptr<const Packet> packet, MpduInfo aMpdu, WifiMacHeader& hdr)
{
  Ptr<Packet> copy = packet->Copy();
  if (aMpdu.type != NORMAL_MPDU)
  {
    AmpduSubframeHeader subframeHeader;
    copy->RemoveHeader(subframeHeader);
  }
  return copy->PeekHeader(hdr) > 0;
}

static void
LinkPhyTxPsduBegin(uint32_t metricIndex, WifiConstPsduMap psduMap, WifiTxVector txVector, double txPowerW)
{
  (void)txPowerW;
  if (!g_collectMetrics || metricIndex >= g_radioMetrics.size())
  {
    return;
  }

  auto& metric = g_radioMetrics[metricIndex];
  if (!metric.wireless || !metric.txWifiDev)
  {
    return;
  }

  for (const auto& staIdPsdu : psduMap)
  {
    const uint16_t staId = staIdPsdu.first;
    Ptr<const WifiPsdu> psdu = staIdPsdu.second;
    if (!PsduMatchesDownlinkData(psdu, metric.txMac, metric.rxMac))
    {
      continue;
    }

    const WifiMode mode = txVector.GetMode(staId);
    const uint64_t phyRate = mode.GetPhyRate(txVector, staId);
    const uint64_t dataRate = mode.GetDataRate(txVector, staId);
    const Time txDuration = WifiPhy::CalculateTxDuration(psdu, txVector, metric.txWifiDev->GetPhy()->GetPhyBand());

    metric.txPpdus++;
    metric.txMpdus += psdu->GetNMpdus();
    metric.txBytes += psdu->GetSize();
    metric.txAirtimeNs += txDuration.GetNanoSeconds();
    metric.mcsSamples++;
    metric.mcsSum += static_cast<double>(mode.GetMcsValue());
    metric.phyRateMbpsSum += static_cast<double>(phyRate) / 1e6;
    metric.dataRateMbpsSum += static_cast<double>(dataRate) / 1e6;

    for (const auto& mpdu : *PeekPointer(psdu))
    {
      metric.amsduMsdus += CountMsdusInMpdu(mpdu);
    }
  }
}

static void
LinkRxOutcome(uint32_t metricIndex,
              Ptr<const WifiPpdu> ppdu,
              RxSignalInfo signalInfo,
              const WifiTxVector& txVector,
              const std::vector<bool>& statusPerMpdu)
{
  (void)txVector;
  if (!g_collectMetrics || metricIndex >= g_radioMetrics.size())
  {
    return;
  }

  auto& metric = g_radioMetrics[metricIndex];
  if (!metric.wireless || !ppdu)
  {
    return;
  }

  Ptr<const WifiPsdu> psdu = ppdu->GetPsdu();
  if (!PsduMatchesDownlinkData(psdu, metric.txMac, metric.rxMac))
  {
    return;
  }

  metric.rxPpdus++;

  bool anyFailed = false;
  std::size_t i = 0;
  for (const auto& mpdu : *PeekPointer(psdu))
  {
    if (!IsDataMpdu(mpdu))
    {
      ++i;
      continue;
    }

    const bool ok = i < statusPerMpdu.size() && statusPerMpdu[i];
    if (ok)
    {
      metric.rxMpdusOk++;
    }
    else
    {
      metric.rxMpdusFail++;
      anyFailed = true;
    }
    ++i;
  }

  if (anyFailed)
  {
    metric.rxFailedPpdus++;
  }
}

static void
LinkMonitorRx(uint32_t metricIndex,
              Ptr<const Packet> packet,
              uint16_t channelFreqMhz,
              WifiTxVector txVector,
              MpduInfo aMpdu,
              SignalNoiseDbm signalNoise,
              uint16_t staId)
{
  (void)channelFreqMhz;
  (void)txVector;
  (void)aMpdu;
  (void)staId;
  if (!g_collectMetrics || metricIndex >= g_radioMetrics.size())
  {
    return;
  }

  auto& metric = g_radioMetrics[metricIndex];
  if (!metric.wireless)
  {
    return;
  }

  WifiMacHeader hdr;
  if (!PeekWifiMacHeaderFromMonitorPacket(packet, aMpdu, hdr) ||
      hdr.GetAddr1() != metric.rxMac ||
      hdr.GetAddr2() != metric.txMac)
  {
    return;
  }

  if (!hdr.IsData() && !hdr.IsQosData())
  {
    return;
  }

  metric.rxSignalSamples++;
  metric.rxRssiDbmSum += static_cast<double>(signalNoise.signal);
  metric.rxSnrDbSum += static_cast<double>(signalNoise.signal - signalNoise.noise);
}

static void
ObssMonitorRx(const std::string& observerName,
              Ptr<const Packet> packet,
              uint16_t channelFreqMhz,
              WifiTxVector txVector,
              MpduInfo aMpdu,
              SignalNoiseDbm signalNoise,
              uint16_t staId)
{
  (void)channelFreqMhz;
  (void)txVector;
  (void)aMpdu;
  (void)staId;
  if (!g_collectMetrics || g_obssTxMacs.empty())
  {
    return;
  }

  WifiMacHeader hdr;
  if (!PeekWifiMacHeaderFromMonitorPacket(packet, aMpdu, hdr))
  {
    return;
  }

  const Mac48Address transmitter = hdr.GetAddr2();
  if (std::find(g_obssTxMacs.begin(), g_obssTxMacs.end(), transmitter) == g_obssTxMacs.end())
  {
    return;
  }

  auto& metric = g_obssObservers[observerName];
  metric.observerName = observerName;
  metric.frames++;
  metric.bytes += packet->GetSize();
  metric.rssiDbmSum += static_cast<double>(signalNoise.signal);
  metric.snrDbSum += static_cast<double>(signalNoise.signal - signalNoise.noise);
}

static void
ObssRxOutcome(const std::string& observerName,
              Ptr<const WifiPpdu> ppdu,
              RxSignalInfo signalInfo,
              const WifiTxVector& txVector,
              const std::vector<bool>& statusPerMpdu)
{
  (void)signalInfo;
  (void)txVector;
  if (!g_collectMetrics || g_obssTxMacs.empty() || !ppdu)
  {
    return;
  }

  Ptr<const WifiPsdu> psdu = ppdu->GetPsdu();
  if (!PsduTransmittedByObss(psdu))
  {
    return;
  }

  auto& metric = g_obssObservers[observerName];
  metric.observerName = observerName;
  metric.rxPpdus++;
  metric.rxAirtimeNs += ppdu->GetTxDuration().GetNanoSeconds();

  bool anyFailed = false;
  std::size_t i = 0;
  for (const auto& mpdu : *PeekPointer(psdu))
  {
    if (!IsDataMpdu(mpdu))
    {
      ++i;
      continue;
    }
    if (i >= statusPerMpdu.size() || !statusPerMpdu[i])
    {
      anyFailed = true;
    }
    ++i;
  }

  if (anyFailed)
  {
    metric.rxFailedPpdus++;
  }
}

static double
Percentile(std::vector<double> values, double percentile)
{
  if (values.empty())
  {
    return 0.0;
  }
  std::sort(values.begin(), values.end());
  const double rank = percentile * static_cast<double>(values.size() - 1);
  const size_t index = static_cast<size_t>(std::ceil(rank));
  return values[std::min(index, values.size() - 1)];
}

static double
Stddev(const std::vector<double>& values)
{
  if (values.size() < 2)
  {
    return 0.0;
  }
  const double mean = std::accumulate(values.begin(), values.end(), 0.0) / values.size();
  double sumSq = 0.0;
  for (double value : values)
  {
    const double diff = value - mean;
    sumSq += diff * diff;
  }
  return std::sqrt(sumSq / static_cast<double>(values.size() - 1));
}

static void
HopRttServerRx(uint32_t metricIndex, Ptr<Socket> socket)
{
  if (metricIndex >= g_hopRttMetrics.size())
  {
    return;
  }

  Address from;
  Ptr<Packet> packet;
  while ((packet = socket->RecvFrom(from)))
  {
    socket->SendTo(packet, 0, from);
  }
}

static void
HopRttClientRx(uint32_t metricIndex, Ptr<Socket> socket)
{
  if (metricIndex >= g_hopRttMetrics.size())
  {
    return;
  }

  Ptr<Packet> packet;
  while ((packet = socket->Recv()))
  {
    SeqTsHeader header;
    if (packet->GetSize() < header.GetSerializedSize())
    {
      continue;
    }
    packet->RemoveHeader(header);
    auto& metric = g_hopRttMetrics[metricIndex];
    metric.received++;
    metric.rttsMs.push_back((Simulator::Now() - header.GetTs()).GetMicroSeconds() / 1000.0);
  }
}

static void
SendHopRttProbe(uint32_t metricIndex)
{
  if (metricIndex >= g_hopRttMetrics.size())
  {
    return;
  }

  if (Simulator::Now() >= Seconds(g_prewarm + g_test))
  {
    return;
  }

  auto& metric = g_hopRttMetrics[metricIndex];
  if (g_collectMetrics && metric.clientSocket)
  {
    SeqTsHeader header;
    header.SetSeq(metric.nextSeq++);
    const uint32_t payloadSize = std::max<uint32_t>(g_hopRttProbeSize, header.GetSerializedSize()) -
                                 header.GetSerializedSize();
    Ptr<Packet> packet = Create<Packet>(payloadSize);
    packet->AddHeader(header);
    metric.clientSocket->Send(packet);
    metric.sent++;
  }

  Simulator::Schedule(Seconds(g_hopRttProbeInterval), &SendHopRttProbe, metricIndex);
}

static void
TcpRxTrace(const Ptr<const Packet> packet, const TcpHeader& header, const Ptr<const TcpSocketBase> socket)
{
  (void)socket;
  if (!g_collectMetrics)
  {
    return;
  }

  if ((header.GetFlags() & TcpHeader::ACK) != 0)
  {
    g_tcpMetric.ackPackets++;
    g_tcpMetric.ackBytes += packet->GetSize() + header.GetSerializedSize();
  }
}

static void
TcpTxTrace(const Ptr<const Packet> packet, const TcpHeader& header, const Ptr<const TcpSocketBase> socket)
{
  (void)socket;
  if (!g_collectMetrics)
  {
    return;
  }

  if (packet->GetSize() > 0)
  {
    g_tcpMetric.dataPackets++;
    g_tcpMetric.dataBytes += packet->GetSize() + header.GetSerializedSize();
  }
}

static void
TcpRetransmissionTrace(const Ptr<const Packet> packet,
                       const TcpHeader& header,
                       const Address& localAddr,
                       const Address& peerAddr,
                       const Ptr<const TcpSocketBase> socket)
{
  (void)packet;
  (void)header;
  (void)localAddr;
  (void)peerAddr;
  (void)socket;
  if (g_collectMetrics)
  {
    g_tcpMetric.retransPackets++;
  }
}

static void
TcpRttTrace(Time oldValue, Time newValue)
{
  (void)oldValue;
  if (!g_collectMetrics)
  {
    return;
  }
  g_tcpMetric.rttSamples++;
  g_tcpMetric.rttMsSum += newValue.GetMilliSeconds();
}

static void
TcpLastRttTrace(Time oldValue, Time newValue)
{
  (void)oldValue;
  if (!g_collectMetrics)
  {
    return;
  }
  g_tcpMetric.lastRttSamples++;
  g_tcpMetric.lastRttMsSum += newValue.GetMilliSeconds();
}

static ModeConfig
ResolveMode(uint32_t mode)
{
  switch (mode)
  {
    case 1:
      return {TopologyType::STAR_L1, false, false, "L1_STAR_WW"};
    case 2:
      return {TopologyType::STAR_L1, true, false, "L1_STAR_EW"};
    case 3:
      return {TopologyType::STAR_L1, false, true, "L1_STAR_WE"};
    case 4:
      return {TopologyType::STAR_L1, true, true, "L1_STAR_EE"};
    case 5:
      return {TopologyType::CHAIN_L2, false, false, "L2_CHAIN_WW"};
    case 6:
      return {TopologyType::CHAIN_L2, true, false, "L2_CHAIN_EW"};
    case 7:
      return {TopologyType::CHAIN_L2, false, true, "L2_CHAIN_WE"};
    case 8:
      return {TopologyType::CHAIN_L2, true, true, "L2_CHAIN_EE"};
    default:
      NS_FATAL_ERROR("Invalid mode: " << mode << " (expect 1..8)");
  }
}

static void
EnsureCsvHeader()
{
  bool needHeader = true;
  {
    std::ifstream fin(g_outCsv);
    if (fin.good() && fin.peek() != std::ifstream::traits_type::eof())
    {
      needHeader = false;
    }
  }

  g_csv.open(g_outCsv, std::ios::app);
  if (!g_csv.is_open())
  {
    NS_FATAL_ERROR("Cannot open output CSV: " << g_outCsv);
  }

  if (needHeader)
  {
    g_csv << "mode,scenario,staAssoc,run,staX,staY,trafficType,endToEndMbps,"
          << "hop1Name,hop1Mbps,hop1RttAvgMs,hop1RttP95Ms,hop1RttLoss,hop1RttJitterMs,"
          << "hop2Name,hop2Mbps,hop2RttAvgMs,hop2RttP95Ms,hop2RttLoss,hop2RttJitterMs,"
          << "hop3Name,hop3Mbps,hop3RttAvgMs,hop3RttP95Ms,hop3RttLoss,hop3RttJitterMs,";
    for (uint32_t i = 1; i <= 3; ++i)
    {
      g_csv << "hop" << i << "Wireless,"
            << "hop" << i << "McsAvg,"
            << "hop" << i << "PhyRateMbpsAvg,"
            << "hop" << i << "DataRateMbpsAvg,"
            << "hop" << i << "TxAirtimeDuty,"
            << "hop" << i << "TxPpdus,"
            << "hop" << i << "TxMpdus,"
            << "hop" << i << "AvgMpdusPerPpdu,"
            << "hop" << i << "AvgPsduBytes,"
            << "hop" << i << "AvgMsdusPerMpdu,"
            << "hop" << i << "RxMpduPsr,"
            << "hop" << i << "RxMpduPer,"
            << "hop" << i << "RxPpduPsr,"
            << "hop" << i << "RxRssiDbmAvg,"
            << "hop" << i << "RxSnrDbAvg,";
    }
    g_csv << "tcpAckPktsPerSec,tcpAckBytesPerSec,tcpDataPktsPerSec,tcpDataMbps,"
          << "tcpRetransPkts,tcpRttMsAvg,tcpLastRttMsAvg,"
          << "ontObssFrames,ontObssRssiDbmAvg,ontObssSnrDbAvg,ontObssAirtimeDuty,ontObssPpduPsr,"
          << "ap1ObssFrames,ap1ObssRssiDbmAvg,ap1ObssSnrDbAvg,ap1ObssAirtimeDuty,ap1ObssPpduPsr,"
          << "ap2ObssFrames,ap2ObssRssiDbmAvg,ap2ObssSnrDbAvg,ap2ObssAirtimeDuty,ap2ObssPpduPsr,"
          << "staObssFrames,staObssRssiDbmAvg,staObssSnrDbAvg,staObssAirtimeDuty,staObssPpduPsr,"
          << "obssEnabled,obssTargetDuty,obssMeasuredDuty,obssRate\n";
  }
}

static void
CloseCsv()
{
  if (g_csv.is_open())
  {
    g_csv.flush();
    g_csv.close();
  }
}

static void
SnapshotSinks()
{
  g_bytesSnapshot.resize(g_sinks.size());
  for (size_t i = 0; i < g_sinks.size(); ++i)
  {
    g_bytesSnapshot[i] = g_sinks[i]->GetTotalRx();
  }
}

static void
SnapshotHopStats()
{
  for (auto& hop : g_hopStats)
  {
    hop.snapshotBytes = hop.totalBytes;
  }
}

static void
SnapshotObssAirtimeStats()
{
  for (auto& stat : g_obssAirtimeStats)
  {
    stat.snapshot = stat.total;
  }
}

static double
GetObssMeasuredDuty()
{
  Time total = Seconds(0);
  for (const auto& stat : g_obssAirtimeStats)
  {
    total += stat.total - stat.snapshot;
  }
  return total.GetSeconds() / g_test;
}

static void
HopIpv4RxTrace(uint32_t hopIndex, Ptr<const Packet> packet, Ptr<Ipv4> ipv4, uint32_t interface)
{
  if (hopIndex >= g_hopStats.size())
  {
    return;
  }

  auto& hop = g_hopStats[hopIndex];
  if (ipv4 != hop.ipv4 || interface != hop.interface)
  {
    return;
  }

  Ipv4Header ipHeader;
  Ptr<Packet> copy = packet->Copy();
  copy->PeekHeader(ipHeader);

  // Count the selected forward transport, including UDP (the 3.45 script
  // accidentally hard-coded TCP here). Exclude diagnostic probe ports.
  if (ipHeader.GetProtocol() != (g_trafficType == "udp" ? 17 : 6) ||
      ipHeader.GetDestination() != g_flowDstIp)
  {
    return;
  }

  if (ipHeader.GetFragmentOffset() == 0)
  {
    copy->RemoveHeader(ipHeader);
    uint16_t port = 0;
    if (g_trafficType == "udp") { UdpHeader h; copy->PeekHeader(h); port = h.GetDestinationPort(); }
    else { TcpHeader h; copy->PeekHeader(h); port = h.GetDestinationPort(); }
    if (port < g_basePort || port >= uint32_t(g_basePort) + g_tcpStreams) return;
  }
  hop.totalBytes += packet->GetSize();
}

static void
RegisterHopStat(const std::string& name, Ptr<Ipv4> ipv4, uint32_t interface)
{
  const uint32_t hopIndex = static_cast<uint32_t>(g_hopStats.size());
  HopStat hop;
  hop.name = name;
  hop.ipv4 = ipv4;
  hop.interface = interface;
  g_hopStats.push_back(hop);
  ipv4->TraceConnectWithoutContext("Rx", MakeBoundCallback(&HopIpv4RxTrace, hopIndex));
}

static void
ObssPhyTxBegin(uint32_t statIndex, Ptr<const Packet> packet, double txPowerW)
{
  (void)packet;
  (void)txPowerW;
  if (statIndex >= g_obssAirtimeStats.size())
  {
    return;
  }

  auto& stat = g_obssAirtimeStats[statIndex];
  if (stat.activeTx == 0)
  {
    stat.txStart = Simulator::Now();
  }
  ++stat.activeTx;
}

static void
ObssPhyTxEnd(uint32_t statIndex, Ptr<const Packet> packet)
{
  (void)packet;
  if (statIndex >= g_obssAirtimeStats.size())
  {
    return;
  }

  auto& stat = g_obssAirtimeStats[statIndex];
  if (stat.activeTx == 0)
  {
    return;
  }

  --stat.activeTx;
  if (stat.activeTx == 0)
  {
    stat.total += Simulator::Now() - stat.txStart;
  }
}

static void
RegisterObssAirtimeStats(const NetDeviceContainer& devs)
{
  for (uint32_t i = 0; i < devs.GetN(); ++i)
  {
    Ptr<WifiNetDevice> wifiDev = DynamicCast<WifiNetDevice>(devs.Get(i));
    if (!wifiDev)
    {
      continue;
    }

    const uint32_t statIndex = static_cast<uint32_t>(g_obssAirtimeStats.size());
    PhyAirtimeStat stat;
    stat.phy = wifiDev->GetPhy();
    g_obssAirtimeStats.push_back(stat);
    stat.phy->TraceConnectWithoutContext("PhyTxBegin", MakeBoundCallback(&ObssPhyTxBegin, statIndex));
    stat.phy->TraceConnectWithoutContext("PhyTxEnd", MakeBoundCallback(&ObssPhyTxEnd, statIndex));
  }
}

static void
RegisterHopRadioMetric(const std::string& name, const NetDeviceContainer& devs, bool wireless)
{
  HopRadioMetric metric;
  metric.name = name;
  metric.wireless = wireless;

  if (wireless)
  {
    metric.txWifiDev = DynamicCast<WifiNetDevice>(devs.Get(0));
    metric.rxWifiDev = DynamicCast<WifiNetDevice>(devs.Get(1));
    if (!metric.txWifiDev || !metric.rxWifiDev)
    {
      NS_FATAL_ERROR("Wireless metric requested for non-Wi-Fi link " << name);
    }
    metric.txMac = Mac48Address::ConvertFrom(metric.txWifiDev->GetAddress());
    metric.rxMac = Mac48Address::ConvertFrom(metric.rxWifiDev->GetAddress());
  }

  const uint32_t metricIndex = static_cast<uint32_t>(g_radioMetrics.size());
  g_radioMetrics.push_back(metric);

  if (wireless)
  {
    g_radioMetrics[metricIndex].txWifiDev->GetPhy()->TraceConnectWithoutContext(
      "PhyTxPsduBegin",
      MakeBoundCallback(&LinkPhyTxPsduBegin, metricIndex));
    g_radioMetrics[metricIndex].rxWifiDev->GetPhy()->GetState()->TraceConnectWithoutContext(
      "RxOutcome",
      MakeBoundCallback(&LinkRxOutcome, metricIndex));
    g_radioMetrics[metricIndex].rxWifiDev->GetPhy()->TraceConnectWithoutContext(
      "MonitorSnifferRx",
      MakeBoundCallback(&LinkMonitorRx, metricIndex));
  }
}

static void
RegisterObssTxMac(const NetDeviceContainer& devs)
{
  if (devs.GetN() == 0)
  {
    return;
  }
  Ptr<WifiNetDevice> txWifiDev = DynamicCast<WifiNetDevice>(devs.Get(0));
  if (txWifiDev)
  {
    g_obssTxMacs.push_back(Mac48Address::ConvertFrom(txWifiDev->GetAddress()));
  }
}

static void
RegisterObssObserver(const std::string& observerName, Ptr<Node> node)
{
  g_obssObservers[observerName].observerName = observerName;
  for (uint32_t i = 0; i < node->GetNDevices(); ++i)
  {
    Ptr<WifiNetDevice> wifiDev = DynamicCast<WifiNetDevice>(node->GetDevice(i));
    if (!wifiDev)
    {
      continue;
    }
    wifiDev->GetPhy()->TraceConnectWithoutContext(
      "MonitorSnifferRx",
      MakeBoundCallback(&ObssMonitorRx, observerName));
    wifiDev->GetPhy()->GetState()->TraceConnectWithoutContext(
      "RxOutcome",
      MakeBoundCallback(&ObssRxOutcome, observerName));
  }
}

static void
RegisterHopRttMetric(const std::string& name,
                     Ptr<Node> srcNode,
                     Ptr<Node> dstNode,
                     Ipv4Address dstIp)
{
  const uint32_t metricIndex = static_cast<uint32_t>(g_hopRttMetrics.size());
  HopRttMetric metric;
  metric.name = name;
  metric.dstIp = dstIp;
  metric.port = static_cast<uint16_t>(9300 + metricIndex);
  metric.serverSocket = Socket::CreateSocket(dstNode, UdpSocketFactory::GetTypeId());
  metric.clientSocket = Socket::CreateSocket(srcNode, UdpSocketFactory::GetTypeId());

  if (metric.serverSocket->Bind(InetSocketAddress(dstIp, metric.port)) != 0)
  {
    NS_FATAL_ERROR("Failed to bind RTT probe server for " << name);
  }
  if (metric.clientSocket->Bind() != 0)
  {
    NS_FATAL_ERROR("Failed to bind RTT probe client for " << name);
  }
  metric.clientSocket->Connect(InetSocketAddress(dstIp, metric.port));

  g_hopRttMetrics.push_back(metric);
  g_hopRttMetrics[metricIndex].serverSocket->SetRecvCallback(MakeBoundCallback(&HopRttServerRx, metricIndex));
  g_hopRttMetrics[metricIndex].clientSocket->SetRecvCallback(MakeBoundCallback(&HopRttClientRx, metricIndex));
  Simulator::Schedule(Seconds(g_prewarm), &SendHopRttProbe, metricIndex);
}

static void
RegisterTcpSocketTraces(Ptr<Node> srcNode)
{
  const std::string base = "/NodeList/" + std::to_string(srcNode->GetId()) +
                           "/$ns3::TcpL4Protocol/SocketList/*/";
  Config::ConnectWithoutContext(base + "Rx", MakeCallback(&TcpRxTrace));
  Config::ConnectWithoutContext(base + "Tx", MakeCallback(&TcpTxTrace));
  Config::ConnectWithoutContext(base + "Retransmission", MakeCallback(&TcpRetransmissionTrace));
  Config::ConnectWithoutContext(base + "RTT", MakeCallback(&TcpRttTrace));
  Config::ConnectWithoutContext(base + "LastRTT", MakeCallback(&TcpLastRttTrace));
}

static void
AppendEmptyHopRadioMetric()
{
  for (uint32_t i = 0; i < 15; ++i)
  {
    g_csv << ",";
  }
}

static void
AppendHopRttMetric(size_t index)
{
  if (index >= g_hopRttMetrics.size() || g_hopRttMetrics[index].sent == 0)
  {
    g_csv << ",,,,";
    return;
  }

  const auto& metric = g_hopRttMetrics[index];
  const double avg = SafeRatio(std::accumulate(metric.rttsMs.begin(), metric.rttsMs.end(), 0.0),
                               metric.rttsMs.size());
  const double loss = SafeRatio(metric.sent - metric.received, metric.sent);

  g_csv << "," << std::fixed << std::setprecision(3) << avg
        << "," << std::fixed << std::setprecision(3) << Percentile(metric.rttsMs, 0.95)
        << "," << std::fixed << std::setprecision(6) << loss
        << "," << std::fixed << std::setprecision(3) << Stddev(metric.rttsMs);
}

static void
AppendHopRadioMetric(size_t index)
{
  if (index >= g_radioMetrics.size())
  {
    AppendEmptyHopRadioMetric();
    return;
  }

  const auto& metric = g_radioMetrics[index];
  g_csv << "," << (metric.wireless ? 1 : 0);
  if (!metric.wireless)
  {
    for (uint32_t i = 0; i < 14; ++i)
    {
      g_csv << ",";
    }
    return;
  }

  const double txPpdus = static_cast<double>(metric.txPpdus);
  const double txMpdus = static_cast<double>(metric.txMpdus);
  const double rxMpdus = static_cast<double>(metric.rxMpdusOk + metric.rxMpdusFail);
  const double rxPpdus = static_cast<double>(metric.rxPpdus);
  const double rxPpduOk = static_cast<double>(metric.rxPpdus >= metric.rxFailedPpdus
                                                ? metric.rxPpdus - metric.rxFailedPpdus
                                                : 0);

  g_csv << "," << std::fixed << std::setprecision(3) << SafeRatio(metric.mcsSum, metric.mcsSamples)
        << "," << std::fixed << std::setprecision(3) << SafeRatio(metric.phyRateMbpsSum, metric.mcsSamples)
        << "," << std::fixed << std::setprecision(3) << SafeRatio(metric.dataRateMbpsSum, metric.mcsSamples)
        << "," << std::fixed << std::setprecision(6) << (static_cast<double>(metric.txAirtimeNs) / 1e9 / g_test)
        << "," << metric.txPpdus
        << "," << metric.txMpdus
        << "," << std::fixed << std::setprecision(3) << SafeRatio(txMpdus, txPpdus)
        << "," << std::fixed << std::setprecision(3) << SafeRatio(metric.txBytes, txPpdus)
        << "," << std::fixed << std::setprecision(3) << SafeRatio(metric.amsduMsdus, txMpdus)
        << "," << std::fixed << std::setprecision(6) << SafeRatio(metric.rxMpdusOk, rxMpdus)
        << "," << std::fixed << std::setprecision(6) << SafeRatio(metric.rxMpdusFail, rxMpdus)
        << "," << std::fixed << std::setprecision(6) << SafeRatio(rxPpduOk, rxPpdus)
        << "," << std::fixed << std::setprecision(3) << SafeRatio(metric.rxRssiDbmSum, metric.rxSignalSamples)
        << "," << std::fixed << std::setprecision(3) << SafeRatio(metric.rxSnrDbSum, metric.rxSignalSamples);
}

static void
AppendTcpMetrics()
{
  g_csv << "," << std::fixed << std::setprecision(3) << SafeRatio(g_tcpMetric.ackPackets, g_test)
        << "," << std::fixed << std::setprecision(3) << SafeRatio(g_tcpMetric.ackBytes, g_test)
        << "," << std::fixed << std::setprecision(3) << SafeRatio(g_tcpMetric.dataPackets, g_test)
        << "," << std::fixed << std::setprecision(3) << (static_cast<double>(g_tcpMetric.dataBytes) * 8.0 / g_test / 1e6)
        << "," << g_tcpMetric.retransPackets
        << "," << std::fixed << std::setprecision(3) << SafeRatio(g_tcpMetric.rttMsSum, g_tcpMetric.rttSamples)
        << "," << std::fixed << std::setprecision(3) << SafeRatio(g_tcpMetric.lastRttMsSum, g_tcpMetric.lastRttSamples);
}

static void
AppendObssObserverMetric(const std::string& observerName)
{
  const auto it = g_obssObservers.find(observerName);
  if (it == g_obssObservers.end())
  {
    g_csv << ",0,,,,";
    return;
  }

  const auto& metric = it->second;
  g_csv << "," << metric.frames;
  if (metric.frames == 0)
  {
    g_csv << ",,";
  }
  else
  {
    g_csv << "," << std::fixed << std::setprecision(3) << SafeRatio(metric.rssiDbmSum, metric.frames)
          << "," << std::fixed << std::setprecision(3) << SafeRatio(metric.snrDbSum, metric.frames);
  }

  g_csv << "," << std::fixed << std::setprecision(6)
        << (static_cast<double>(metric.rxAirtimeNs) / 1e9 / g_test);
  if (metric.rxPpdus == 0)
  {
    g_csv << ",";
  }
  else
  {
    const double okPpdus = static_cast<double>(metric.rxPpdus >= metric.rxFailedPpdus
                                                 ? metric.rxPpdus - metric.rxFailedPpdus
                                                 : 0);
    g_csv << "," << std::fixed << std::setprecision(6) << SafeRatio(okPpdus, metric.rxPpdus);
  }
}

static NetDeviceContainer
InstallWiredLink(Ptr<Node> a, Ptr<Node> b)
{
  CsmaHelper csma;
  csma.SetChannelAttribute("DataRate", StringValue(g_wiredRate));
  csma.SetChannelAttribute("Delay", TimeValue(NanoSeconds(500)));
  return csma.Install(NodeContainer(a, b));
}

static NetDeviceContainer
InstallWifiP2pLink(Ptr<Node> apNode,
                   Ptr<Node> staNode,
                   const std::string& ssid,
                   const YansWifiPhyHelper& phyTemplate,
                   std::vector<NetDeviceContainer>* pcapWifiLinks)
{
  YansWifiPhyHelper phy = phyTemplate;

  WifiHelper wifi;
  wifi.SetStandard(WIFI_STANDARD_80211be);
  wifi.SetRemoteStationManager("ns3::IdealWifiManager");

  WifiMacHelper mac;
  const Ssid s = Ssid(ssid);

  mac.SetType("ns3::ApWifiMac", "Ssid", SsidValue(s));
  NetDeviceContainer apDev = wifi.Install(phy, mac, apNode);

  mac.SetType("ns3::StaWifiMac", "Ssid", SsidValue(s), "ActiveProbing", BooleanValue(false));
  NetDeviceContainer staDev = wifi.Install(phy, mac, staNode);

  NetDeviceContainer all;
  all.Add(apDev);
  all.Add(staDev);

  if (pcapWifiLinks)
  {
    pcapWifiLinks->push_back(all);
  }
  return all;
}

static NetDeviceContainer
InstallWifiAccess(Ptr<Node> infraNode,
                  Ptr<Node> staNode,
                  const YansWifiPhyHelper& phyTemplate,
                  std::vector<NetDeviceContainer>* pcapWifiLinks)
{
  return InstallWifiP2pLink(infraNode, staNode, "ACC", phyTemplate, pcapWifiLinks);
}

static Vector
GetNodePosition(const ModeConfig& cfg, const std::string& nodeName)
{
  (void)cfg;
  if (nodeName == "ont")
  {
    return Vector(0.0, 0.0, 0.0);
  }
  if (nodeName == "ap1")
  {
    return Vector(10.0, 0.0, 0.0);
  }
  return Vector(10.0, 8.0, 0.0);
}

static Vector
GetStaPosition(const ModeConfig& cfg, const std::string& staAssoc)
{
  (void)cfg;
  if (staAssoc == "ont")
  {
    return Vector(1.5, -1.2, 0.0);
  }
  if (staAssoc == "ap1")
  {
    return Vector(10.0, -1.2, 0.0);
  }
  return Vector(10.0, 6.8, 0.0);
}

static Ptr<Node>
ResolveAssocNode(Ptr<Node> ont, Ptr<Node> ap1, Ptr<Node> ap2, const std::string& staAssoc)
{
  if (staAssoc == "ont")
  {
    return ont;
  }
  if (staAssoc == "ap1")
  {
    return ap1;
  }
  if (staAssoc == "ap2")
  {
    return ap2;
  }
  NS_FATAL_ERROR("Invalid staAssoc: " << staAssoc << " (expect ont|ap1|ap2)");
}

static uint32_t
FindInterfaceForAddress(Ptr<Ipv4> ipv4, Ipv4Address address)
{
  for (uint32_t i = 0; i < ipv4->GetNInterfaces(); ++i)
  {
    for (uint32_t j = 0; j < ipv4->GetNAddresses(i); ++j)
    {
      if (ipv4->GetAddress(i, j).GetLocal() == address)
      {
        return i;
      }
    }
  }
  NS_FATAL_ERROR("Interface not found for address " << address);
}

static void
InstallBulkTraffic(Ptr<Node> srcNode, Ipv4Address dstIp, double start, double stop)
{
  const std::string socketFactory =
      (g_trafficType == "udp") ? "ns3::UdpSocketFactory" : "ns3::TcpSocketFactory";
  for (uint32_t i = 0; i < g_tcpStreams; ++i)
  {
    const uint16_t port = g_basePort + i;
    OnOffHelper sender(socketFactory, InetSocketAddress(dstIp, port));
    sender.SetAttribute("DataRate", DataRateValue(DataRate(g_appRate)));
    sender.SetAttribute("PacketSize", UintegerValue(g_payloadSize));
    sender.SetAttribute("OnTime", StringValue("ns3::ConstantRandomVariable[Constant=1e9]"));
    sender.SetAttribute("OffTime", StringValue("ns3::ConstantRandomVariable[Constant=0]"));
    ApplicationContainer app = sender.Install(srcNode);
    app.Start(Seconds(start + 0.01 * i));
    app.Stop(Seconds(stop));
  }
}

int
main(int argc, char* argv[])
{
  LogComponentEnable("MESH_TEST_OBSS_METRICS", LOG_LEVEL_INFO);

  CommandLine cmd(__FILE__);
  cmd.AddValue("mode", "1..8 (8 mesh combinations)", g_mode);
  cmd.AddValue("staAssoc", "STA association target: ont|ap1|ap2", g_staAssoc);
  cmd.AddValue("run", "RNG run index", g_run);
  cmd.AddValue("prewarm", "Warm-up time (s)", g_prewarm);
  cmd.AddValue("test", "Measure window (s)", g_test);
  cmd.AddValue("tcpStreams", "Number of parallel downlink application streams", g_tcpStreams);
  cmd.AddValue("trafficType", "Main downlink traffic type: tcp|udp", g_trafficType);
  cmd.AddValue("appRate", "Per-flow offered application rate, e.g. 1Gbps or 20Gbps", g_appRate);
  cmd.AddValue("wiredRate", "Wired backhaul rate, e.g. 10Gbps or 20Gbps", g_wiredRate);
  cmd.AddValue("rtsCtsThreshold", "Wi-Fi RTS/CTS threshold (0 enables RTS/CTS for all data frames)", g_rtsCtsThreshold);
  cmd.AddValue("enableAmpdu", "Enable BE A-MPDU aggregation", g_enableAmpdu);
  cmd.AddValue("enableAmsdu", "Enable BE A-MSDU aggregation", g_enableAmsdu);
  cmd.AddValue("enablePcap", "Enable PCAP", g_enablePcap);
  cmd.AddValue("out", "Output CSV path", Callback<bool, const std::string&>(
      [](const std::string& value) { g_outCsv = value; return true; }));
  cmd.AddValue("useStaPos", "Use manual STA XY position override", g_useStaPosOverride);
  cmd.AddValue("staX", "Manual STA X (meters) when useStaPos=1", g_staX);
  cmd.AddValue("staY", "Manual STA Y (meters) when useStaPos=1", g_staY);
  cmd.AddValue("enableObss", "Enable an overlapping 5 GHz BSS on the same channel", g_enableObss);
  cmd.AddValue("enableObss1", "Enable the first OBSS interferer", g_enableObss1);
  cmd.AddValue("enableObss2", "Enable the second OBSS interferer", g_enableObss2);
  cmd.AddValue("obssTargetDuty", "Target aggregate OBSS airtime duty used to derive offered load", g_obssTargetDuty);
  cmd.AddValue("obssLinkRateMbps", "Estimated OBSS effective link rate used when obssRate is empty", g_obssLinkRateMbps);
  cmd.AddValue("obssRate", "Explicit aggregate OBSS UDP offered rate, e.g. 160Mbps; empty derives from target duty", g_obssRate);
  cmd.AddValue("obssPacketSize", "OBSS UDP payload size in bytes", g_obssPacketSize);
  cmd.AddValue("obssApX", "First OBSS AP X position", g_obssApX);
  cmd.AddValue("obssApY", "First OBSS AP Y position", g_obssApY);
  cmd.AddValue("obssStaX", "First OBSS STA X position", g_obssStaX);
  cmd.AddValue("obssStaY", "First OBSS STA Y position", g_obssStaY);
  cmd.AddValue("obssAp2X", "Second OBSS AP X position", g_obssAp2X);
  cmd.AddValue("obssAp2Y", "Second OBSS AP Y position", g_obssAp2Y);
  cmd.AddValue("obssSta2X", "Second OBSS STA X position", g_obssSta2X);
  cmd.AddValue("obssSta2Y", "Second OBSS STA Y position", g_obssSta2Y);
  cmd.AddValue("hopRttProbeInterval", "Per-hop UDP RTT probe interval during the measurement window (s)", g_hopRttProbeInterval);
  cmd.AddValue("hopRttProbeSize", "Per-hop UDP RTT probe packet size in bytes", g_hopRttProbeSize);
  bool enableMeshviz = false;
  bool openMeshviz = true;
  std::string meshvizFile;
  double captureStart = -1.0;
  double captureDuration = 0.15;
  uint64_t maxPpdus = 100000;
  cmd.AddValue("enableMeshviz", "Record this run and open MeshViz after simulation", enableMeshviz);
  cmd.AddValue("openMeshviz", "Open the viewer when enableMeshviz=1; disable for headless runs", openMeshviz);
  cmd.AddValue("meshviz", "Explicit MeshViz JSONL path; capture-only unless enableMeshviz=1",
      Callback<bool, const std::string&>([&meshvizFile](const std::string& value) {
        meshvizFile = value; return true;
      }));
  cmd.AddValue("captureStart", "PPDU capture start in seconds; -1 uses prewarm", captureStart);
  cmd.AddValue("captureDuration", "Detailed PPDU capture duration in seconds", captureDuration);
  cmd.AddValue("maxPpdus", "Detailed PPDU cap; full-window metrics remain enabled", maxPpdus);
  cmd.Parse(argc, argv);
  NS_ABORT_MSG_IF(g_prewarm < 0 || g_test <= 0 || g_hopRttProbeInterval <= 0 ||
                  g_tcpStreams == 0 || g_tcpStreams > 60000 ||
                  uint32_t(g_basePort) + g_tcpStreams > 65536, "Invalid time/flow arguments");

  if (enableMeshviz)
  {
    if (meshvizFile.empty())
    {
      meshvizFile = (std::filesystem::path(MeshvizHelper::CreateRunDirectory()) / "run.jsonl").string();
    }
    const auto parent = std::filesystem::absolute(meshvizFile).parent_path();
    std::filesystem::create_directories(parent);
    if (g_outCsv == "mesh_test_2.csv") g_outCsv = (parent / "metrics.csv").string();
    std::cout << "MeshViz result: " << std::filesystem::absolute(meshvizFile) << std::endl;
  }

  const ModeConfig cfg = ResolveMode(g_mode);

  if (g_staAssoc != "ont" && g_staAssoc != "ap1" && g_staAssoc != "ap2")
  {
    NS_FATAL_ERROR("Invalid staAssoc: " << g_staAssoc << " (expect ont|ap1|ap2)");
  }

  EnsureCsvHeader();

  RngSeedManager::SetSeed(1);
  RngSeedManager::SetRun(g_run);

  Config::SetDefault("ns3::WifiRemoteStationManager::RtsCtsThreshold", StringValue(std::to_string(g_rtsCtsThreshold)));
  Config::SetDefault("ns3::TcpSocket::SegmentSize", UintegerValue(1500));
  Config::SetDefault("ns3::TcpSocket::RcvBufSize", UintegerValue(2 * 1024 * 1024));
  Config::SetDefault("ns3::TcpSocket::SndBufSize", UintegerValue(2 * 1024 * 1024));
  Config::SetDefault("ns3::TcpSocketBase::Timestamp", BooleanValue(false));

  if (g_trafficType != "tcp" && g_trafficType != "udp")
  {
    NS_FATAL_ERROR("Invalid trafficType: " << g_trafficType << " (expect tcp|udp)");
  }
  Config::SetDefault("ns3::WifiMacQueue::MaxSize",
                     QueueSizeValue(QueueSize(QueueSizeUnit::PACKETS, std::numeric_limits<uint32_t>::max())));
  Config::SetDefault("ns3::WifiMac::BE_MaxAmpduSize",
                     UintegerValue(g_enableAmpdu ? kDefaultBeMaxAmpduSize : 0));
  Config::SetDefault("ns3::WifiMac::BE_MaxAmsduSize",
                     UintegerValue(g_enableAmsdu ? kDefaultBeMaxAmsduSize : 0));
  Config::SetDefault("ns3::HeConfiguration::GuardInterval",
              TimeValue(NanoSeconds(g_giNs)));

  Ptr<Node> ont = CreateObject<Node>();
  Ptr<Node> ap1 = CreateObject<Node>();
  Ptr<Node> ap2 = CreateObject<Node>();
  Ptr<Node> sta = CreateObject<Node>();
  Ptr<Node> bgAp = CreateObject<Node>();
  Ptr<Node> bgSta = CreateObject<Node>();
  Ptr<Node> bgAp2 = CreateObject<Node>();
  Ptr<Node> bgSta2 = CreateObject<Node>();

  NodeContainer allNodes(ont, ap1, ap2, sta, bgAp, bgSta, bgAp2, bgSta2);

  MobilityHelper mob;
  mob.SetMobilityModel("ns3::ConstantPositionMobilityModel");
  mob.Install(allNodes);

  ont->GetObject<MobilityModel>()->SetPosition(GetNodePosition(cfg, "ont"));
  ap1->GetObject<MobilityModel>()->SetPosition(GetNodePosition(cfg, "ap1"));
  ap2->GetObject<MobilityModel>()->SetPosition(GetNodePosition(cfg, "ap2"));
  const Vector staPos = g_useStaPosOverride ? Vector(g_staX, g_staY, 0.0) : GetStaPosition(cfg, g_staAssoc);
  sta->GetObject<MobilityModel>()->SetPosition(staPos);
  bgAp->GetObject<MobilityModel>()->SetPosition(Vector(g_obssApX, g_obssApY, 0.0));
  bgSta->GetObject<MobilityModel>()->SetPosition(Vector(g_obssStaX, g_obssStaY, 0.0));
  bgAp2->GetObject<MobilityModel>()->SetPosition(Vector(g_obssAp2X, g_obssAp2Y, 0.0));
  bgSta2->GetObject<MobilityModel>()->SetPosition(Vector(g_obssSta2X, g_obssSta2Y, 0.0));

  InternetStackHelper stack;
  stack.Install(allNodes);

  Ptr<Ipv4> ontIpv4 = ont->GetObject<Ipv4>();
  Ptr<Ipv4> ap1Ipv4 = ap1->GetObject<Ipv4>();
  Ptr<Ipv4> ap2Ipv4 = ap2->GetObject<Ipv4>();
  ontIpv4->SetAttribute("IpForward", BooleanValue(true));
  ap1Ipv4->SetAttribute("IpForward", BooleanValue(true));
  ap2Ipv4->SetAttribute("IpForward", BooleanValue(true));

  // All wireless links share one channel so backhaul and access contend on the same medium.
  YansWifiChannelHelper chHelper;
  chHelper.SetPropagationDelay("ns3::ConstantSpeedPropagationDelayModel");
  chHelper.AddPropagationLoss("ns3::LogDistancePropagationLossModel");

  YansWifiPhyHelper sharedPhy;
  sharedPhy.SetChannel(chHelper.Create());
  sharedPhy.Set("ChannelSettings",
                StringValue("{" + std::to_string(g_chanNum) + "," + std::to_string(g_channelWidth) + ",BAND_5GHZ,0}"));
  sharedPhy.Set("Antennas", UintegerValue(g_nss));
  sharedPhy.Set("MaxSupportedTxSpatialStreams", UintegerValue(g_nss));
  sharedPhy.Set("MaxSupportedRxSpatialStreams", UintegerValue(g_nss));
  sharedPhy.Set("TxPowerStart", DoubleValue(g_txPowerDbm));
  sharedPhy.Set("TxPowerEnd", DoubleValue(g_txPowerDbm));

  std::vector<NetDeviceContainer> pcapWifiLinks;
  std::vector<NetDeviceContainer> pcapWiredLinks;

  NetDeviceContainer ontAp1Devs;
  NetDeviceContainer ontAp2Devs;
  NetDeviceContainer ap1Ap2Devs;
  NetDeviceContainer obssDevs1;
  NetDeviceContainer obssDevs2;

  if (cfg.topology == TopologyType::STAR_L1)
  {
    ontAp1Devs = cfg.linkAIsWired
                   ? InstallWiredLink(ont, ap1)
                   : InstallWifiP2pLink(ont, ap1, "BH_O_A1", sharedPhy, &pcapWifiLinks);
    ontAp2Devs = cfg.linkBIsWired
                   ? InstallWiredLink(ont, ap2)
                   : InstallWifiP2pLink(ont, ap2, "BH_O_A2", sharedPhy, &pcapWifiLinks);

    if (cfg.linkAIsWired)
    {
      pcapWiredLinks.push_back(ontAp1Devs);
    }
    if (cfg.linkBIsWired)
    {
      pcapWiredLinks.push_back(ontAp2Devs);
    }
  }
  else
  {
    ontAp1Devs = cfg.linkAIsWired
                   ? InstallWiredLink(ont, ap1)
                   : InstallWifiP2pLink(ont, ap1, "BH_O_A1", sharedPhy, &pcapWifiLinks);
    ap1Ap2Devs = cfg.linkBIsWired
                   ? InstallWiredLink(ap1, ap2)
                   : InstallWifiP2pLink(ap1, ap2, "BH_A1_A2", sharedPhy, &pcapWifiLinks);

    if (cfg.linkAIsWired)
    {
      pcapWiredLinks.push_back(ontAp1Devs);
    }
    if (cfg.linkBIsWired)
    {
      pcapWiredLinks.push_back(ap1Ap2Devs);
    }
  }

  Ptr<Node> assocNode = ResolveAssocNode(ont, ap1, ap2, g_staAssoc);
  NetDeviceContainer acc = InstallWifiAccess(assocNode, sta, sharedPhy, &pcapWifiLinks);
  if (g_enableObss && g_enableObss1)
  {
    obssDevs1 = InstallWifiP2pLink(bgAp, bgSta, "OBSS_5G_1", sharedPhy, &pcapWifiLinks);
    RegisterObssAirtimeStats(obssDevs1);
    RegisterObssTxMac(obssDevs1);
  }
  if (g_enableObss && g_enableObss2)
  {
    obssDevs2 = InstallWifiP2pLink(bgAp2, bgSta2, "OBSS_5G_2", sharedPhy, &pcapWifiLinks);
    RegisterObssAirtimeStats(obssDevs2);
    RegisterObssTxMac(obssDevs2);
  }
  RegisterObssObserver("ont", ont);
  RegisterObssObserver("ap1", ap1);
  RegisterObssObserver("ap2", ap2);
  RegisterObssObserver("sta", sta);

  Ipv4AddressHelper addr;
  Ipv4InterfaceContainer ifOntAp1;
  Ipv4InterfaceContainer ifOntAp2;
  Ipv4InterfaceContainer ifAp1Ap2;
  Ipv4InterfaceContainer ifAcc;
  Ipv4InterfaceContainer ifObss1;
  Ipv4InterfaceContainer ifObss2;

  auto AssignSubnet = [&](const NetDeviceContainer& devs, const std::string& base) {
    addr.SetBase(base.c_str(), "255.255.255.0");
    return addr.Assign(devs);
  };

  ifOntAp1 = AssignSubnet(ontAp1Devs, "10.1.1.0");
  if (cfg.topology == TopologyType::STAR_L1)
  {
    ifOntAp2 = AssignSubnet(ontAp2Devs, "10.1.2.0");
  }
  else
  {
    ifAp1Ap2 = AssignSubnet(ap1Ap2Devs, "10.1.3.0");
  }
  ifAcc = AssignSubnet(acc, "10.1.10.0");
  if (g_enableObss && g_enableObss1)
  {
    ifObss1 = AssignSubnet(obssDevs1, "10.1.20.0");
  }
  if (g_enableObss && g_enableObss2)
  {
    ifObss2 = AssignSubnet(obssDevs2, "10.1.21.0");
  }

  Ipv4StaticRoutingHelper staticRouting;
  Ptr<Ipv4StaticRouting> ontRt = staticRouting.GetStaticRouting(ontIpv4);
  Ptr<Ipv4StaticRouting> ap1Rt = staticRouting.GetStaticRouting(ap1Ipv4);
  Ptr<Ipv4StaticRouting> ap2Rt = staticRouting.GetStaticRouting(ap2Ipv4);
  Ptr<Ipv4StaticRouting> staRt = staticRouting.GetStaticRouting(sta->GetObject<Ipv4>());

  const Ipv4Address ontAp1OntIp = ifOntAp1.GetAddress(0);
  const Ipv4Address ontAp1Ap1Ip = ifOntAp1.GetAddress(1);
  const uint32_t ontAp1IfOnt = FindInterfaceForAddress(ontIpv4, ontAp1OntIp);
  const uint32_t ontAp1IfAp1 = FindInterfaceForAddress(ap1Ipv4, ontAp1Ap1Ip);

  Ipv4Address ontAp2Ap2Ip = Ipv4Address::GetZero();
  uint32_t ontAp2IfAp2 = 0;
  if (cfg.topology == TopologyType::STAR_L1)
  {
    ontAp2Ap2Ip = ifOntAp2.GetAddress(1);
    ontAp2IfAp2 = FindInterfaceForAddress(ap2Ipv4, ontAp2Ap2Ip);
  }

  Ipv4Address ap1Ap2Ap1Ip = Ipv4Address::GetZero();
  Ipv4Address ap1Ap2Ap2Ip = Ipv4Address::GetZero();
  uint32_t ap1Ap2IfAp1 = 0;
  uint32_t ap1Ap2IfAp2 = 0;
  if (cfg.topology == TopologyType::CHAIN_L2)
  {
    ap1Ap2Ap1Ip = ifAp1Ap2.GetAddress(0);
    ap1Ap2Ap2Ip = ifAp1Ap2.GetAddress(1);
    ap1Ap2IfAp1 = FindInterfaceForAddress(ap1Ipv4, ap1Ap2Ap1Ip);
    ap1Ap2IfAp2 = FindInterfaceForAddress(ap2Ipv4, ap1Ap2Ap2Ip);
  }

  const Ipv4Address accessInfraIp = ifAcc.GetAddress(0);
  const Ipv4Address accessStaIp = ifAcc.GetAddress(1);
  const uint32_t staAccessIf = FindInterfaceForAddress(sta->GetObject<Ipv4>(), accessStaIp);

  staRt->SetDefaultRoute(accessInfraIp, staAccessIf);

  if (g_staAssoc == "ap1")
  {
    ontRt->AddNetworkRouteTo(Ipv4Address("10.1.10.0"), Ipv4Mask("255.255.255.0"), ontAp1Ap1Ip, ontAp1IfOnt);
  }
  else if (g_staAssoc == "ap2")
  {
    if (cfg.topology == TopologyType::STAR_L1)
    {
      const Ipv4Address ontAp2OntIp = ifOntAp2.GetAddress(0);
      const uint32_t ontAp2IfOnt = FindInterfaceForAddress(ontIpv4, ontAp2OntIp);
      ontRt->AddNetworkRouteTo(Ipv4Address("10.1.10.0"),
                               Ipv4Mask("255.255.255.0"),
                               ontAp2Ap2Ip,
                               ontAp2IfOnt);
    }
    else
    {
      ontRt->AddNetworkRouteTo(Ipv4Address("10.1.10.0"),
                               Ipv4Mask("255.255.255.0"),
                               ontAp1Ap1Ip,
                               ontAp1IfOnt);
      ap1Rt->AddNetworkRouteTo(Ipv4Address("10.1.10.0"),
                               Ipv4Mask("255.255.255.0"),
                               ap1Ap2Ap2Ip,
                               ap1Ap2IfAp1);
      ap2Rt->AddNetworkRouteTo(Ipv4Address("10.1.1.0"),
                               Ipv4Mask("255.255.255.0"),
                               ap1Ap2Ap1Ip,
                               ap1Ap2IfAp2);
    }
  }

  const double simEnd = g_prewarm + g_test + 1.0;
  const double trafficStart = 1.0;
  const double trafficStop = simEnd - 0.1;
  const uint32_t obssSources = g_enableObss ? static_cast<uint32_t>(g_enableObss1) + static_cast<uint32_t>(g_enableObss2) : 0;

  std::string effectiveObssRate;
  std::string effectiveObssPerSourceRate;
  if (g_obssRate.empty())
  {
    const double aggregateRateMbps = g_obssTargetDuty * g_obssLinkRateMbps;
    std::ostringstream aggregateRate;
    aggregateRate << std::fixed << std::setprecision(3) << aggregateRateMbps << "Mbps";
    effectiveObssRate = aggregateRate.str();

    std::ostringstream perSourceRate;
    perSourceRate << std::fixed << std::setprecision(3) << (aggregateRateMbps / std::max<uint32_t>(1, obssSources)) << "Mbps";
    effectiveObssPerSourceRate = perSourceRate.str();
  }
  else
  {
    effectiveObssRate = g_obssRate;
    const uint64_t perSourceBps = DataRate(g_obssRate).GetBitRate() / std::max<uint32_t>(1, obssSources);
    effectiveObssPerSourceRate = std::to_string(perSourceBps) + "bps";
  }

  if (g_enableObss)
  {
    auto InstallObssTraffic = [&](Ptr<Node> src, Ptr<Node> dst, Ipv4Address dstIp, uint16_t port) {
      PacketSinkHelper obssSink("ns3::UdpSocketFactory",
                                InetSocketAddress(Ipv4Address::GetAny(), port));
      ApplicationContainer sinkApp = obssSink.Install(dst);
      sinkApp.Start(Seconds(0.0));
      sinkApp.Stop(Seconds(simEnd));

      OnOffHelper obssSender("ns3::UdpSocketFactory", InetSocketAddress(dstIp, port));
      obssSender.SetAttribute("DataRate", DataRateValue(DataRate(effectiveObssPerSourceRate)));
      obssSender.SetAttribute("PacketSize", UintegerValue(g_obssPacketSize));
      obssSender.SetAttribute("OnTime", StringValue("ns3::ConstantRandomVariable[Constant=1e9]"));
      obssSender.SetAttribute("OffTime", StringValue("ns3::ConstantRandomVariable[Constant=0]"));
      ApplicationContainer senderApp = obssSender.Install(src);
      senderApp.Start(Seconds(trafficStart));
      senderApp.Stop(Seconds(trafficStop));
    };

    if (g_enableObss1)
    {
      InstallObssTraffic(bgAp, bgSta, ifObss1.GetAddress(1), 9000);
    }
    if (g_enableObss2)
    {
      InstallObssTraffic(bgAp2, bgSta2, ifObss2.GetAddress(1), 9010);
    }
  }

  for (uint32_t i = 0; i < g_tcpStreams; ++i)
  {
    const uint16_t port = g_basePort + i;
    const std::string socketFactory =
        (g_trafficType == "udp") ? "ns3::UdpSocketFactory" : "ns3::TcpSocketFactory";
    PacketSinkHelper sink(socketFactory, InetSocketAddress(Ipv4Address::GetAny(), port));
    ApplicationContainer apps = sink.Install(sta);
    apps.Start(Seconds(0.0));
    apps.Stop(Seconds(simEnd));
    g_sinks.push_back(DynamicCast<PacketSink>(apps.Get(0)));
  }

  Ptr<Ipv4> staIpv4 = sta->GetObject<Ipv4>();
  Ipv4Address staIp = Ipv4Address::GetAny();
  for (uint32_t i = 1; i < staIpv4->GetNInterfaces(); ++i)
  {
    Ipv4Address cand = staIpv4->GetAddress(i, 0).GetLocal();
    if (cand != Ipv4Address("127.0.0.1") && cand != Ipv4Address::GetZero())
    {
      staIp = cand;
      break;
    }
  }
  if (staIp == Ipv4Address::GetAny())
  {
    NS_FATAL_ERROR("Failed to resolve STA access IP");
  }

  g_flowDstIp = staIp;
  std::unique_ptr<MeshvizHelper> meshviz;
  if (!meshvizFile.empty())
  {
    if (captureStart < 0) captureStart = g_prewarm;
    meshviz = std::make_unique<MeshvizHelper>(meshvizFile, Seconds(g_prewarm),
        Seconds(g_prewarm + g_test), Seconds(captureStart),
        Seconds(captureStart + captureDuration), maxPpdus);
    meshviz->SetFlow(staIp, g_basePort, g_tcpStreams);
    meshviz->AddNode(ont, "ONT");
    meshviz->AddNode(ap1, "AP1");
    meshviz->AddNode(ap2, "AP2");
    meshviz->AddNode(sta, "STA1");
    if (obssDevs1.GetN()) { meshviz->AddNode(bgAp, "OBSS AP1"); meshviz->AddNode(bgSta, "OBSS STA1"); }
    if (obssDevs2.GetN()) { meshviz->AddNode(bgAp2, "OBSS AP2"); meshviz->AddNode(bgSta2, "OBSS STA2"); }
    meshviz->AddLink(ontAp1Devs, !cfg.linkAIsWired);
    meshviz->AddLink(ontAp2Devs, !cfg.linkBIsWired);
    meshviz->AddLink(ap1Ap2Devs, !cfg.linkBIsWired);
    meshviz->AddLink(acc, true);
    meshviz->AddLink(obssDevs1, true, true);
    meshviz->AddLink(obssDevs2, true, true);
    for (const auto& sink : g_sinks) meshviz->TrackSink(sink);
  }
  g_hopStats.clear();
  g_hopRttMetrics.clear();
  if (g_staAssoc == "ont")
  {
    RegisterHopStat("ONT->STA", staIpv4, staAccessIf);
    RegisterHopRadioMetric("ONT->STA", acc, true);
    RegisterHopRttMetric("ONT->STA", ont, sta, accessStaIp);
  }
  else if (g_staAssoc == "ap1")
  {
    RegisterHopStat("ONT->AP1", ap1Ipv4, ontAp1IfAp1);
    RegisterHopStat("AP1->STA", staIpv4, staAccessIf);
    RegisterHopRadioMetric("ONT->AP1", ontAp1Devs, !cfg.linkAIsWired);
    RegisterHopRadioMetric("AP1->STA", acc, true);
    RegisterHopRttMetric("ONT->AP1", ont, ap1, ontAp1Ap1Ip);
    RegisterHopRttMetric("AP1->STA", ap1, sta, accessStaIp);
  }
  else
  {
    if (cfg.topology == TopologyType::STAR_L1)
    {
      RegisterHopStat("ONT->AP2", ap2Ipv4, ontAp2IfAp2);
      RegisterHopRadioMetric("ONT->AP2", ontAp2Devs, !cfg.linkBIsWired);
      RegisterHopRttMetric("ONT->AP2", ont, ap2, ontAp2Ap2Ip);
    }
    else
    {
      RegisterHopStat("ONT->AP1", ap1Ipv4, ontAp1IfAp1);
      RegisterHopStat("AP1->AP2", ap2Ipv4, ap1Ap2IfAp2);
      RegisterHopRadioMetric("ONT->AP1", ontAp1Devs, !cfg.linkAIsWired);
      RegisterHopRadioMetric("AP1->AP2", ap1Ap2Devs, !cfg.linkBIsWired);
      RegisterHopRttMetric("ONT->AP1", ont, ap1, ontAp1Ap1Ip);
      RegisterHopRttMetric("AP1->AP2", ap1, ap2, ap1Ap2Ap2Ip);
    }
    RegisterHopStat("AP2->STA", staIpv4, staAccessIf);
    RegisterHopRadioMetric("AP2->STA", acc, true);
    RegisterHopRttMetric("AP2->STA", ap2, sta, accessStaIp);
  }

  InstallBulkTraffic(ont, staIp, trafficStart, trafficStop);
  if (g_trafficType == "tcp")
  {
    Simulator::Schedule(Seconds(trafficStart + 0.05), &RegisterTcpSocketTraces, ont);
  }

  Simulator::Schedule(Seconds(g_prewarm), &SnapshotSinks);
  Simulator::Schedule(Seconds(g_prewarm), &SnapshotHopStats);
  Simulator::Schedule(Seconds(g_prewarm), &SnapshotObssAirtimeStats);
  Simulator::Schedule(Seconds(g_prewarm), []() { g_collectMetrics = true; });
  Simulator::Schedule(Seconds(g_prewarm + g_test), []() { g_collectMetrics = false; });
  Simulator::Schedule(Seconds(g_prewarm + g_test), [cfg, staPos, effectiveObssRate]() {
    uint64_t deltaBytes = 0;
    for (size_t i = 0; i < g_sinks.size(); ++i)
    {
      const uint64_t nowBytes = g_sinks[i]->GetTotalRx();
      deltaBytes += (nowBytes - g_bytesSnapshot[i]);
    }
    const double thptMbps = (deltaBytes * 8.0) / g_test / 1e6;

    std::vector<double> hopMbps(g_hopStats.size(), 0.0);
    for (size_t i = 0; i < g_hopStats.size(); ++i)
    {
      const uint64_t hopDelta = g_hopStats[i].totalBytes - g_hopStats[i].snapshotBytes;
      hopMbps[i] = (hopDelta * 8.0) / g_test / 1e6;
    }
    const double obssMeasuredDuty = g_enableObss ? GetObssMeasuredDuty() : 0.0;

    std::ostringstream oss;
    oss << "[RESULT] mode=" << g_mode << " scenario=" << cfg.name << " staAssoc=" << g_staAssoc
        << " trafficType=" << g_trafficType
        << " endToEnd=" << std::fixed << std::setprecision(3) << thptMbps << " Mbps";
    for (size_t i = 0; i < g_hopStats.size(); ++i)
    {
      oss << " | " << g_hopStats[i].name << "=" << std::fixed << std::setprecision(3) << hopMbps[i] << " Mbps";
    }
    if (g_enableObss)
    {
      oss << " | OBSS targetDuty=" << std::fixed << std::setprecision(3) << g_obssTargetDuty
          << " measuredDuty=" << obssMeasuredDuty;
    }
    NS_LOG_INFO(oss.str());

    g_csv << g_mode << "," << cfg.name << "," << g_staAssoc << "," << g_run << ","
          << staPos.x << "," << staPos.y << "," << g_trafficType << ","
          << std::fixed << std::setprecision(3) << thptMbps;
    for (size_t i = 0; i < 3; ++i)
    {
      if (i < g_hopStats.size())
      {
        g_csv << "," << g_hopStats[i].name << "," << std::fixed << std::setprecision(3) << hopMbps[i];
        AppendHopRttMetric(i);
      }
      else
      {
        g_csv << ",,,,,,";
      }
    }
    for (size_t i = 0; i < 3; ++i)
    {
      AppendHopRadioMetric(i);
    }
    AppendTcpMetrics();
    AppendObssObserverMetric("ont");
    AppendObssObserverMetric("ap1");
    AppendObssObserverMetric("ap2");
    AppendObssObserverMetric("sta");
    g_csv << "," << (g_enableObss ? 1 : 0) << ","
          << std::fixed << std::setprecision(3) << g_obssTargetDuty << ","
          << std::fixed << std::setprecision(6) << obssMeasuredDuty << ","
          << (g_enableObss ? effectiveObssRate : "");
    g_csv << "\n";
    g_csv.flush();
  });

  if (g_enablePcap)
  {
    for (size_t i = 0; i < pcapWifiLinks.size(); ++i)
    {
      YansWifiPhyHelper pcapPhy;
      pcapPhy.SetPcapDataLinkType(WifiPhyHelper::DLT_IEEE802_11_RADIO);
      pcapPhy.EnablePcap("mesh_test_wifi_" + std::to_string(i), pcapWifiLinks[i], true);
    }

    for (size_t i = 0; i < pcapWiredLinks.size(); ++i)
    {
      CsmaHelper csma;
      csma.EnablePcap("mesh_test_wired_" + std::to_string(i), pcapWiredLinks[i], true);
    }
  }

  Simulator::Stop(Seconds(simEnd));
  Simulator::Run();
  if (meshviz) meshviz->Finish();
  Simulator::Destroy();

  CloseCsv();
  if (enableMeshviz && openMeshviz) return MeshvizHelper::OpenViewer(meshvizFile);
  return 0;
}
