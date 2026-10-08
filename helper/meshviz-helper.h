// SPDX-License-Identifier: MIT
#ifndef MESHVIZ_HELPER_H
#define MESHVIZ_HELPER_H
#include "ns3/ipv4.h"
#include "ns3/net-device-container.h"
#include "ns3/node.h"
#include "ns3/packet-sink.h"
#include "ns3/wifi-net-device.h"
#include "ns3/wifi-phy-state-helper.h"
#include "ns3/wifi-ppdu.h"
#include "ns3/wifi-psdu.h"

#include <fstream>
#include <map>
#include <set>
#include <string>
#include <tuple>

namespace ns3
{
/** @defgroup meshviz Single-run mesh trace recorder */
/** @ingroup meshviz
 * Offline, per-run recorder. Keep alive through Simulator::Destroy().
 * Register nodes and point-to-point topology edges before running. Metrics use
 * IPv4 Tx -> next-hop IPv4 Rx, including queueing, retries and link delivery.
 * Capture limits affect detailed records only, never measurement aggregates.
 */
class MeshvizHelper
{
  public:
    /**
     * @param file Output JSONL filename.
     * @param measureStart Inclusive statistics start.
     * @param measureEnd Exclusive statistics end.
     * @param captureStart Inclusive detailed capture start.
     * @param captureEnd Exclusive detailed capture end.
     * @param maxPpdus Maximum detailed PPDU records.
     */
    MeshvizHelper(const std::string& file,
                  Time measureStart,
                  Time measureEnd,
                  Time captureStart,
                  Time captureEnd,
                  uint64_t maxPpdus = 100000);
    ~MeshvizHelper();
    /** Create a unique directory for one run without replacing previous results.
     * @param base Parent directory for generated results.
     * @return Absolute path of the new directory.
     */
    static std::string CreateRunDirectory(const std::string& base = "meshviz-results");
    /** Open the completed trace in the sibling build-tree viewer, without a shell.
     * Waits until the viewer closes. Recording does not depend on Qt.
     * @param file Completed JSONL trace path.
     * @return Viewer exit status, or nonzero if unavailable or launch fails.
     */
    static int OpenViewer(const std::string& file);
    /** Register a physical node and all its existing interfaces.
     * @param node Installed node with devices and Internet stack.
     * @param name Stable display label, independent of interface MAC role.
     */
    void AddNode(Ptr<Node> node, const std::string& name);
    /** Register a directed topology edge.
     * @param devices Pair of actual sender and receiver NetDevices.
     * @param wireless True for Wi-Fi, false for a wired medium.
     * @param background Exclude this edge from main-flow statistics.
     */
    void AddLink(const NetDeviceContainer& devices, bool wireless, bool background = false);
    /** Add an application to aggregate delivered goodput.
     * @param sink Main-flow receiver; register each sink once.
     */
    void TrackSink(Ptr<PacketSink> sink);
    /** Select forward main-flow datagrams before running the simulator.
     * @param destination Main-flow destination IPv4 address.
     * @param firstPort First main-flow destination transport port.
     * @param streams Number of consecutive destination ports.
     */
    void SetFlow(Ipv4Address destination, uint16_t firstPort, uint16_t streams);
    /** Flush metrics and close the recording. Safe to call repeatedly. */
    void Finish();

  private:
    /** @internal One directed interface pair. */
    struct Edge
    {
        Ptr<NetDevice> tx, rx;     ///< Actual outgoing and next-hop receiving interfaces.
        bool wireless, background; ///< Medium flag and exclusion from main-flow metrics.
        uint32_t id;               ///< Run-local topology edge identifier.
    };

    /** @internal One outstanding fragment transmission. */
    struct Pending
    {
        int64_t ns;     ///< IPv4 transmission timestamp in nanoseconds.
        uint32_t bytes; ///< Transmitted IP packet size.
        bool capture;   ///< Emit eventual delivery even beyond the capture boundary.
    };

    /** @internal Accumulators for a 100 ms receive-time bin. */
    struct Bin
    {
        uint64_t bytes = 0, count = 0;     ///< Received bytes and matched delay samples.
        double delaySum = 0, delayMax = 0; ///< Delay accumulators in milliseconds.
    };

    static void Originate(MeshvizHelper*, const Ipv4Header&, Ptr<const Packet>, uint32_t);
    static void IpTx(MeshvizHelper*, Ptr<const Packet>, Ptr<Ipv4>, uint32_t);
    static void IpRx(MeshvizHelper*, Ptr<const Packet>, Ptr<Ipv4>, uint32_t);
    static void PhyTx(MeshvizHelper*, Ptr<WifiNetDevice>, WifiConstPsduMap, WifiTxVector, double);
    static void RxDrop(MeshvizHelper*,
                       Ptr<WifiNetDevice>,
                       Ptr<const WifiPpdu>,
                       WifiPhyRxfailureReason);
    static void RxOutcome(MeshvizHelper*,
                          Ptr<WifiNetDevice>,
                          Ptr<const WifiPpdu>,
                          RxSignalInfo,
                          const WifiTxVector&,
                          const std::vector<bool>&);
    static void SinkRx(MeshvizHelper*, Ptr<const Packet>, const Address&);
    void IpEvent(bool tx, Ptr<const Packet>, Ptr<Ipv4>, uint32_t);
    /** @return Whether the current time is inside detailed capture. */
    bool Capturing() const;
    /** @return Whether the current time is inside the statistics interval. */
    bool Measuring() const;
    /** Read or attach an identity.
     * @param packet Packet carrying a byte tag.
     * @param create Attach a fresh identity when absent.
     * @return Identity, or zero when absent and creation is disabled.
     */
    uint64_t PacketId(Ptr<const Packet> packet, bool create);
    /** Enumerate all identities preserved inside an aggregate.
     * @param packet Packet or aggregate.
     * @return Distinct carried identities.
     */
    std::set<uint64_t> PacketIds(Ptr<const Packet> packet) const;
    /** Periodically release unpaired transmissions older than ten seconds. */
    void ExpirePending();
    std::ofstream m_out; ///< Streaming JSONL output; no full trace retained.
    Time m_start, m_end, m_captureStart, m_captureEnd; ///< Half-open time windows.
    uint64_t m_maxPpdus, m_nextPacket = 0, m_nextPpdu = 0, m_written = 0, m_skipped = 0;
    bool m_finished = false;                 ///< Protect finalization and late callbacks.
    int64_t m_lastExpiryNs = 0;              ///< Last outstanding-packet cleanup time.
    Ipv4Address m_dst;                       ///< Main-flow destination.
    uint16_t m_firstPort = 0, m_streams = 0; ///< Main-flow transport port range.
    std::map<uint32_t, std::string> m_names; ///< Physical node labels.
    std::vector<Edge> m_edges;               ///< Actual directed links.
    std::map<std::tuple<uint64_t, uint32_t, uint16_t>, Pending>
        m_pending;                                      ///< Packet/hop/fragment joins.
    std::map<std::pair<uint32_t, int64_t>, Bin> m_bins; ///< Hop and time-bin accumulators.
    std::map<int64_t, uint64_t> m_sinkBins; ///< Application payload by receive-time bin.

    // Exact PSDU object identity survives channel copies in ns-3.48 SU Wi-Fi.
    /** @internal Retain PSDU identity until associated receptions complete. */
    struct TxRef
    {
        Ptr<const WifiPsdu> psdu; ///< Keep PSDU identity alive.
        uint64_t id;              ///< PPDU transmission identifier.
        int64_t end;              ///< End of the transmission.
        uint64_t sender;          ///< Actual transmitter MAC.
    };

    std::map<const WifiPsdu*, TxRef> m_air; ///< Short-lived receive association map.
};
} // namespace ns3
#endif
