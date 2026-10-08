// SPDX-License-Identifier: MIT
#include "meshviz-helper.h"

#include "ns3/abort.h"
#include "ns3/ipv4-header.h"
#include "ns3/mobility-model.h"
#include "ns3/simulator.h"
#include "ns3/system-path.h"
#include "ns3/tag.h"
#include "ns3/tcp-header.h"
#include "ns3/udp-header.h"
#include "ns3/wifi-mpdu.h"
#include "ns3/wifi-phy.h"

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <iostream>
#ifdef _WIN32
#include <process.h>
#else
#include <sys/wait.h>
#include <unistd.h>
#endif
#include <iomanip>
#include <limits>
#include <sstream>

namespace ns3
{
std::string
MeshvizHelper::CreateRunDirectory(const std::string& base)
{
    namespace fs = std::filesystem;
    fs::create_directories(base);
    const auto stamp = std::chrono::duration_cast<std::chrono::microseconds>(
                           std::chrono::system_clock::now().time_since_epoch())
                           .count();
    for (unsigned i = 0;; ++i)
    {
        const auto directory = fs::absolute(
            fs::path(base) / ("run-" + std::to_string(stamp) + "-" + std::to_string(i)));
        if (fs::create_directory(directory))
        {
            return directory.string();
        }
    }
}

int
MeshvizHelper::OpenViewer(const std::string& file)
{
    namespace fs = std::filesystem;
    fs::path viewer;
    auto directory = fs::path(SystemPath::FindSelfDirectory());
    while (!directory.empty())
    {
        auto candidate = directory / "contrib/meshviz/meshviz-viewer";
#ifdef _WIN32
        candidate += ".exe";
#endif
        if (fs::is_regular_file(candidate))
        {
            viewer = candidate;
            break;
        }
        if (directory == directory.parent_path())
        {
            break;
        }
        directory = directory.parent_path();
    }
    if (viewer.empty())
    {
        std::cerr << "MeshViz: viewer unavailable. Build meshviz-viewer with Qt Widgets, "
                     "or use --openMeshviz=0. Trace saved: "
                  << file << std::endl;
        return 2;
    }
    const auto executable = viewer.string();
    const auto trace = fs::absolute(file).string();
    std::cout << "MeshViz: opening " << trace << std::endl;
#ifdef _WIN32
    const auto status = _spawnl(_P_WAIT,
                                executable.c_str(),
                                executable.c_str(),
                                trace.c_str(),
                                static_cast<char*>(nullptr));
    return status < 0 ? 2 : static_cast<int>(status);
#else
    const auto child = fork();
    if (child < 0)
    {
        return 2;
    }
    if (child == 0)
    {
        execl(executable.c_str(), executable.c_str(), trace.c_str(), static_cast<char*>(nullptr));
        _exit(127);
    }
    int status = 0;
    while (waitpid(child, &status, 0) < 0)
    {
        if (errno != EINTR)
        {
            return 2;
        }
    }
    return WIFEXITED(status) ? WEXITSTATUS(status) : 2;
#endif
}

namespace
{
class MeshvizPacketTag : public Tag
{
  public:
    uint64_t id = 0;

    static TypeId GetTypeId()
    {
        static TypeId t =
            TypeId("ns3::MeshvizPacketTag").SetParent<Tag>().AddConstructor<MeshvizPacketTag>();
        return t;
    }

    TypeId GetInstanceTypeId() const override
    {
        return GetTypeId();
    }

    uint32_t GetSerializedSize() const override
    {
        return 8;
    }

    void Serialize(TagBuffer b) const override
    {
        b.WriteU64(id);
    }

    void Deserialize(TagBuffer b) override
    {
        id = b.ReadU64();
    }

    void Print(std::ostream& o) const override
    {
        o << id;
    }
};

uint64_t
Mac(Ptr<NetDevice> d)
{
    uint8_t b[6];
    Mac48Address::ConvertFrom(d->GetAddress()).CopyTo(b);
    uint64_t v = 0;
    for (auto c : b)
    {
        v = (v << 8) | c;
    }
    return v;
}

uint64_t
Mac(Mac48Address a)
{
    uint8_t b[6];
    a.CopyTo(b);
    uint64_t v = 0;
    for (auto c : b)
    {
        v = (v << 8) | c;
    }
    return v;
}

std::string
Quote(const std::string& s)
{
    std::ostringstream o;
    o << '"';
    for (unsigned char c : s)
    {
        if (c == '"' || c == '\\')
        {
            o << '\\' << c;
        }
        else if (c < 32)
        {
            o << "\\u" << std::hex << std::setw(4) << std::setfill('0') << unsigned(c);
        }
        else
        {
            o << c;
        }
    }
    return o.str() + '"';
}

constexpr int64_t BIN_NS = 100000000; // 100 ms, last bin clipped to measurement end
} // namespace

MeshvizHelper::MeshvizHelper(const std::string& file,
                             Time start,
                             Time end,
                             Time captureStart,
                             Time captureEnd,
                             uint64_t maxPpdus)
    : m_out(file),
      m_start(start),
      m_end(end),
      m_captureStart(captureStart),
      m_captureEnd(captureEnd),
      m_maxPpdus(maxPpdus)
{
    NS_ABORT_MSG_IF(!m_out, "Cannot write MeshViz trace: " << file);
    NS_ABORT_MSG_IF(start < Seconds(0) || end <= start || captureEnd <= captureStart ||
                        maxPpdus == 0,
                    "Invalid MeshViz intervals/cap");
    m_out << "{\"type\":\"run\",\"schema\":1,\"baseline\":\"ns-3.48\",\"startNs\":"
          << start.GetNanoSeconds() << ",\"endNs\":" << end.GetNanoSeconds()
          << ",\"captureStartNs\":" << captureStart.GetNanoSeconds()
          << ",\"captureEndNs\":" << captureEnd.GetNanoSeconds() << ",\"binNs\":" << BIN_NS
          << "}\n";
}

MeshvizHelper::~MeshvizHelper()
{
    Finish();
}

bool
MeshvizHelper::Capturing() const
{
    auto t = Simulator::Now();
    return !m_finished && t >= m_captureStart && t < m_captureEnd;
}

bool
MeshvizHelper::Measuring() const
{
    auto t = Simulator::Now();
    return !m_finished && t >= m_start && t < m_end;
}

void
MeshvizHelper::SetFlow(Ipv4Address dst, uint16_t port, uint16_t streams)
{
    m_dst = dst;
    m_firstPort = port;
    m_streams = streams;
}

void
MeshvizHelper::AddNode(Ptr<Node> node, const std::string& name)
{
    NS_ABORT_MSG_IF(m_names.count(node->GetId()), "Node already registered");
    m_names[node->GetId()] = name;
    Vector p;
    if (auto mob = node->GetObject<MobilityModel>())
    {
        p = mob->GetPosition();
    }
    m_out << "{\"type\":\"node\",\"node\":" << node->GetId() << ",\"name\":" << Quote(name)
          << ",\"x\":" << p.x << ",\"y\":" << p.y << "}\n";
    for (uint32_t i = 0; i < node->GetNDevices(); ++i)
    {
        auto d = node->GetDevice(i);
        if (!Mac48Address::IsMatchingType(d->GetAddress()) ||
            d->GetInstanceTypeId().GetName() == "ns3::LoopbackNetDevice")
        {
            continue;
        }
        auto wifi = DynamicCast<WifiNetDevice>(d);
        m_out << "{\"type\":\"device\",\"node\":" << node->GetId() << ",\"device\":" << i
              << ",\"mac\":" << Mac(d) << ",\"wireless\":" << (wifi ? "true" : "false") << "}\n";
        if (wifi)
        {
            wifi->GetPhy()->TraceConnectWithoutContext("PhyTxPsduBegin",
                                                       MakeBoundCallback(&PhyTx, this, wifi));
            wifi->GetPhy()->TraceConnectWithoutContext("PhyRxPpduDrop",
                                                       MakeBoundCallback(&RxDrop, this, wifi));
            wifi->GetPhy()->GetState()->TraceConnectWithoutContext(
                "RxOutcome",
                MakeBoundCallback(&RxOutcome, this, wifi));
        }
    }
    auto ip = node->GetObject<Ipv4>();
    if (ip)
    {
        ip->TraceConnectWithoutContext("SendOutgoing", MakeBoundCallback(&Originate, this));
        ip->TraceConnectWithoutContext("Tx", MakeBoundCallback(&IpTx, this));
        ip->TraceConnectWithoutContext("Rx", MakeBoundCallback(&IpRx, this));
    }
}

void
MeshvizHelper::AddLink(const NetDeviceContainer& devs, bool wireless, bool background)
{
    if (devs.GetN() == 0)
    {
        return;
    }
    NS_ABORT_MSG_IF(devs.GetN() != 2, "Register one pair of devices per edge");
    uint32_t id = m_edges.size();
    m_edges.push_back({devs.Get(0), devs.Get(1), wireless, background, id});
    m_out << "{\"type\":\"link\",\"id\":" << id << ",\"from\":" << devs.Get(0)->GetNode()->GetId()
          << ",\"to\":" << devs.Get(1)->GetNode()->GetId() << ",\"sender\":" << Mac(devs.Get(0))
          << ",\"receiver\":" << Mac(devs.Get(1))
          << ",\"wireless\":" << (wireless ? "true" : "false")
          << ",\"background\":" << (background ? "true" : "false") << "}\n";
}

void
MeshvizHelper::TrackSink(Ptr<PacketSink> sink)
{
    sink->TraceConnectWithoutContext("Rx", MakeBoundCallback(&SinkRx, this));
}

uint64_t
MeshvizHelper::PacketId(Ptr<const Packet> p, bool create)
{
    MeshvizPacketTag t;
    if (p->FindFirstMatchingByteTag(t))
    {
        return t.id;
    }
    if (!create)
    {
        return 0;
    }
    t.id = ++m_nextPacket;
    p->AddByteTag(t);
    return t.id;
}

std::set<uint64_t>
MeshvizHelper::PacketIds(Ptr<const Packet> p) const
{
    std::set<uint64_t> ids;
    auto it = p->GetByteTagIterator();
    while (it.HasNext())
    {
        auto item = it.Next();
        if (item.GetTypeId() == MeshvizPacketTag::GetTypeId())
        {
            MeshvizPacketTag t;
            item.GetTag(t);
            ids.insert(t.id);
        }
    }
    return ids;
}

void
MeshvizHelper::Originate(MeshvizHelper* s, const Ipv4Header& h, Ptr<const Packet> p, uint32_t)
{
    if (h.GetDestination() != s->m_dst)
    {
        return;
    }
    uint16_t port = 0;
    uint32_t seq = 0;
    if (h.GetProtocol() == 6)
    {
        TcpHeader t;
        p->PeekHeader(t);
        port = t.GetDestinationPort();
        seq = t.GetSequenceNumber().GetValue();
    }
    else if (h.GetProtocol() == 17)
    {
        UdpHeader u;
        p->PeekHeader(u);
        port = u.GetDestinationPort();
    }
    else
    {
        return;
    }
    if (port < s->m_firstPort || port >= uint32_t(s->m_firstPort) + s->m_streams)
    {
        return;
    }
    auto id = s->PacketId(p, true);
    if (s->Capturing() && s->m_written < s->m_maxPpdus)
    {
        s->m_out << "{\"type\":\"packet\",\"packet\":" << id
                 << ",\"protocol\":" << unsigned(h.GetProtocol()) << ",\"port\":" << port
                 << ",\"tcpSeq\":" << seq << "}\n";
    }
}

void
MeshvizHelper::IpTx(MeshvizHelper* s, Ptr<const Packet> p, Ptr<Ipv4> ip, uint32_t i)
{
    s->IpEvent(true, p, ip, i);
}

void
MeshvizHelper::IpRx(MeshvizHelper* s, Ptr<const Packet> p, Ptr<Ipv4> ip, uint32_t i)
{
    s->IpEvent(false, p, ip, i);
}

void
MeshvizHelper::ExpirePending()
{
    const auto now = Simulator::Now().GetNanoSeconds();
    // Bound losses/never-delivered packets without retaining a full-run history.
    if (now - m_lastExpiryNs < 1000000000LL)
    {
        return;
    }
    m_lastExpiryNs = now;
    for (auto it = m_pending.begin(); it != m_pending.end();)
    {
        if (now - it->second.ns > 10000000000LL)
        {
            it = m_pending.erase(it);
        }
        else
        {
            ++it;
        }
    }
}

void
MeshvizHelper::IpEvent(bool tx, Ptr<const Packet> p, Ptr<Ipv4> ip, uint32_t i)
{
    if (m_finished)
    {
        return;
    }
    const uint64_t id = PacketId(p, false);
    if (!id)
    {
        return;
    }
    Ipv4Header h;
    if (!p->PeekHeader(h) || h.GetDestination() != m_dst)
    {
        return;
    }
    const auto dev = ip->GetNetDevice(i);
    const auto now = Simulator::Now().GetNanoSeconds();
    for (const auto& e : m_edges)
    {
        if (e.background || (tx ? e.tx : e.rx) != dev)
        {
            continue;
        }
        auto key = std::make_tuple(id, e.id, h.GetFragmentOffset());
        bool capture = Capturing() && m_written < m_maxPpdus;
        if (tx)
        {
            m_pending[key] = {now, p->GetSize(), capture};
            ExpirePending();
        }
        auto it = m_pending.find(key);
        double delay = -1;
        if (!tx && it != m_pending.end())
        {
            delay = (now - it->second.ns) / 1e6;
            capture = capture || it->second.capture;
        }
        if (capture)
        {
            m_out << "{\"type\":\"hop\",\"event\":\"" << (tx ? "TX" : "RX")
                  << "\",\"packet\":" << id << ",\"hop\":" << e.id << ",\"timeNs\":" << now
                  << ",\"fragment\":" << h.GetFragmentOffset() << ",\"bytes\":" << p->GetSize()
                  << ",\"delayMs\":" << delay << "}\n";
        }
        if (!tx)
        {
            if (Measuring())
            {
                auto& b = m_bins[{e.id, (now - m_start.GetNanoSeconds()) / BIN_NS}];
                b.bytes += p->GetSize();
                if (delay >= 0)
                {
                    ++b.count;
                    b.delaySum += delay;
                    b.delayMax = std::max(b.delayMax, delay);
                }
            }
            if (it != m_pending.end())
            {
                m_pending.erase(it);
            }
        }
    }
}

void
MeshvizHelper::SinkRx(MeshvizHelper* s, Ptr<const Packet> p, const Address&)
{
    if (s->Measuring())
    {
        s->m_sinkBins[(Simulator::Now() - s->m_start).GetNanoSeconds() / BIN_NS] += p->GetSize();
    }
}

void
MeshvizHelper::PhyTx(MeshvizHelper* s,
                     Ptr<WifiNetDevice> d,
                     WifiConstPsduMap psdus,
                     WifiTxVector v,
                     double)
{
    if (!s->Capturing())
    {
        return;
    }
    auto now = Simulator::Now().GetNanoSeconds();
    for (auto it = s->m_air.begin(); it != s->m_air.end();)
    {
        if (it->second.end + 1000000 < now)
        {
            it = s->m_air.erase(it);
        }
        else
        {
            ++it;
        }
    }
    const auto duration =
        WifiPhy::CalculateTxDuration(psdus, v, d->GetPhy()->GetPhyBand()).GetNanoSeconds();
    for (const auto& [staId, psdu] : psdus)
    {
        if (s->m_written >= s->m_maxPpdus)
        {
            ++s->m_skipped;
            continue;
        }
        ++s->m_written;
        const auto ppdu = ++s->m_nextPpdu;
        const auto& h = (*psdu->begin())->GetHeader();
        // TA is authoritative where present; ACK/CTS lack TA: use traced PHY's device.
        uint64_t sender = (h.IsAck() || h.IsCts()) ? Mac(d) : Mac(h.GetAddr2());
        NS_ABORT_MSG_IF(sender != Mac(d), "MeshViz TA does not match the transmitting device MAC");
        auto mode = v.GetMode(staId);
        int mcs = mode.GetModulationClass() >= WIFI_MOD_CLASS_HT ? mode.GetMcsValue() : -1;
        std::set<uint64_t> ids;
        for (const auto& mpdu : *psdu)
        {
            auto part = s->PacketIds(mpdu->GetPacket());
            ids.insert(part.begin(), part.end());
        }
        s->m_air[PeekPointer(psdu)] = {psdu, ppdu, now + duration, sender};
        s->m_out << "{\"type\":\"ppdu\",\"id\":" << ppdu << ",\"node\":" << d->GetNode()->GetId()
                 << ",\"device\":" << d->GetIfIndex() << ",\"sender\":" << sender
                 << ",\"receiver\":" << Mac(h.GetAddr1()) << ",\"startNs\":" << now
                 << ",\"endNs\":" << now + duration
                 << ",\"channel\":" << unsigned(d->GetPhy()->GetChannelNumber())
                 << ",\"mcs\":" << mcs << ",\"bytes\":" << psdu->GetSize()
                 << ",\"mpdus\":" << psdu->GetNMpdus()
                 << ",\"retry\":" << (h.IsRetry() ? "true" : "false")
                 << ",\"frame\":" << Quote(h.GetTypeString()) << ",\"packets\":[";
        bool first = true;
        for (auto id : ids)
        {
            if (!first)
            {
                s->m_out << ',';
            }
            first = false;
            s->m_out << id;
        }
        s->m_out << "]}\n";
    }
}

void
MeshvizHelper::RxOutcome(MeshvizHelper* s,
                         Ptr<WifiNetDevice> d,
                         Ptr<const WifiPpdu> ppdu,
                         RxSignalInfo info,
                         const WifiTxVector&,
                         const std::vector<bool>& status)
{
    if (s->m_finished || !ppdu)
    {
        return;
    }
    auto psdu = ppdu->GetPsdu();
    auto it = s->m_air.find(PeekPointer(psdu));
    if (it == s->m_air.end())
    {
        return;
    }
    if (!psdu->GetAddr1().IsGroup() && Mac(psdu->GetAddr1()) != Mac(d))
    {
        return;
    }
    auto ok = std::count(status.begin(), status.end(), true);
    // ns-3.48 leaves RxSignalInfo uninitialized when every MPDU fails.
    // Never inspect that field on the all-failed path.
    double snr = ok > 0 ? 10 * std::log10(info.snr) : std::numeric_limits<double>::quiet_NaN();
    s->m_out << "{\"type\":\"rx\",\"ppdu\":" << it->second.id
             << ",\"node\":" << d->GetNode()->GetId()
             << ",\"timeNs\":" << Simulator::Now().GetNanoSeconds() << ",\"ok\":" << ok
             << ",\"failed\":" << status.size() - ok << ",\"snrDb\":";
    if (std::isfinite(snr))
    {
        s->m_out << snr;
    }
    else
    {
        s->m_out << "null";
    }
    s->m_out << "}\n";
}

void
MeshvizHelper::RxDrop(MeshvizHelper* s,
                      Ptr<WifiNetDevice> d,
                      Ptr<const WifiPpdu> ppdu,
                      WifiPhyRxfailureReason reason)
{
    if (s->m_finished || !ppdu)
    {
        return;
    }
    auto psdu = ppdu->GetPsdu();
    auto it = s->m_air.find(PeekPointer(psdu));
    if (it == s->m_air.end())
    {
        return;
    }
    if (!psdu->GetAddr1().IsGroup() && Mac(psdu->GetAddr1()) != Mac(d))
    {
        return;
    }
    std::ostringstream label;
    label << reason;
    s->m_out << "{\"type\":\"rx-drop\",\"ppdu\":" << it->second.id
             << ",\"node\":" << d->GetNode()->GetId()
             << ",\"timeNs\":" << Simulator::Now().GetNanoSeconds()
             << ",\"ok\":0,\"failed\":" << psdu->GetNMpdus()
             << ",\"snrDb\":null,\"reason\":" << Quote(label.str()) << "}\n";
}

void
MeshvizHelper::Finish()
{
    if (m_finished)
    {
        return;
    }
    m_finished = true;
    auto span = (m_end - m_start).GetNanoSeconds();
    auto count = (span + BIN_NS - 1) / BIN_NS;
    for (int64_t i = 0; i < count; ++i)
    {
        double secs = std::min(BIN_NS, span - i * BIN_NS) / 1e9;
        m_out << "{\"type\":\"total\",\"timeNs\":" << m_start.GetNanoSeconds() + i * BIN_NS
              << ",\"mbps\":" << m_sinkBins[i] * 8 / secs / 1e6 << "}\n";
        for (const auto& e : m_edges)
        {
            if (!e.background)
            {
                auto b = m_bins[{e.id, i}];
                m_out << "{\"type\":\"metric\",\"hop\":" << e.id
                      << ",\"timeNs\":" << m_start.GetNanoSeconds() + i * BIN_NS
                      << ",\"mbps\":" << b.bytes * 8 / secs / 1e6 << ",\"samples\":" << b.count
                      << ",\"meanDelayMs\":";
                if (b.count)
                {
                    m_out << b.delaySum / b.count;
                }
                else
                {
                    m_out << "null";
                }
                m_out << ",\"maxDelayMs\":";
                if (b.count)
                {
                    m_out << b.delayMax;
                }
                else
                {
                    m_out << "null";
                }
                m_out << "}\n";
            }
        }
    }
    m_out << "{\"type\":\"end\",\"ppdus\":" << m_written << ",\"skippedPpdus\":" << m_skipped
          << "}\n";
    m_out.flush();
    NS_ABORT_MSG_IF(!m_out, "MeshViz trace write failed");
    m_out.close();
}
} // namespace ns3
