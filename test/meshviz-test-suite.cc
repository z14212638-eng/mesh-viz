// SPDX-License-Identifier: MIT
#include "ns3/inet-socket-address.h"
#include "ns3/internet-stack-helper.h"
#include "ns3/ipv4-address-helper.h"
#include "ns3/meshviz-helper.h"
#include "ns3/packet-sink-helper.h"
#include "ns3/simple-net-device-helper.h"
#include "ns3/simulator.h"
#include "ns3/socket.h"
#include "ns3/test.h"
#include "ns3/udp-socket-factory.h"

#include <fstream>
#include <sstream>

using namespace ns3;

/** @ingroup tests
 * Check real packet delivery against known delay and byte-count expectations.
 */
class MeshvizDeliveryTest : public TestCase
{
  public:
    MeshvizDeliveryTest()
        : TestCase("Record known link delay and application delivery")
    {
    }

  private:
    void DoRun() override
    {
        NodeContainer nodes;
        nodes.Create(2);
        SimpleNetDeviceHelper link;
        link.SetChannelAttribute("Delay", TimeValue(MilliSeconds(10)));
        auto devices = link.Install(nodes);
        InternetStackHelper stack;
        stack.Install(nodes);
        Ipv4AddressHelper addresses;
        addresses.SetBase("10.10.0.0", "255.255.255.0");
        auto interfaces = addresses.Assign(devices);
        PacketSinkHelper sinkHelper("ns3::UdpSocketFactory",
                                    InetSocketAddress(Ipv4Address::GetAny(), 5000));
        auto apps = sinkHelper.Install(nodes.Get(1));
        apps.Start(Seconds(0));
        auto sink = DynamicCast<PacketSink>(apps.Get(0));
        auto socket = Socket::CreateSocket(nodes.Get(0), UdpSocketFactory::GetTypeId());
        socket->Connect(InetSocketAddress(interfaces.GetAddress(1), 5000));
        const auto file = CreateTempDirFilename("meshviz-known-delivery.jsonl");
        MeshvizHelper recorder(file, Seconds(0.1), Seconds(0.25), Seconds(0.1), Seconds(0.25));
        recorder.SetFlow(interfaces.GetAddress(1), 5000, 1);
        recorder.AddNode(nodes.Get(0), "source\"quoted");
        recorder.AddNode(nodes.Get(1), "sink");
        recorder.AddLink(devices, false);
        recorder.TrackSink(sink);
        Simulator::Schedule(Seconds(0.01), [socket] { socket->Send(Create<Packet>(100)); });
        Simulator::Schedule(Seconds(0.15), [socket] { socket->Send(Create<Packet>(100)); });
        Simulator::Stop(Seconds(0.3));
        Simulator::Run();
        recorder.Finish();
        recorder.Finish();
        NS_TEST_ASSERT_MSG_EQ(sink->GetTotalRx(),
                              200,
                              "Both warm-up and measured datagrams arrive");
        Simulator::Destroy();
        std::ifstream input(file);
        std::stringstream buffer;
        buffer << input.rdbuf();
        const auto text = buffer.str();
        NS_TEST_ASSERT_MSG_EQ((text.find("\"meanDelayMs\":10") != std::string::npos),
                              true,
                              "Known 10 ms hop delay");
        NS_TEST_ASSERT_MSG_EQ((text.find("\"mbps\":0.008}") != std::string::npos),
                              true,
                              "Only measured application bytes in goodput");
        NS_TEST_ASSERT_MSG_EQ((text.find("\"mbps\":0.01024,") != std::string::npos),
                              true,
                              "IP header overhead only in per-hop throughput");
        NS_TEST_ASSERT_MSG_EQ((text.find("source\\\"quoted") != std::string::npos),
                              true,
                              "Node labels escape JSON quotes");
        const auto end = text.find("\"type\":\"end\"");
        NS_TEST_ASSERT_MSG_EQ((end != std::string::npos), true, "Recording finalized");
        NS_TEST_ASSERT_MSG_EQ((text.find("\"type\":\"end\"", end + 1) == std::string::npos),
                              true,
                              "Finish is idempotent");
    }
};

/** @ingroup tests
 * Native test entry point for ./test.py -s meshviz.
 */
class MeshvizTestSuite : public TestSuite
{
  public:
    MeshvizTestSuite()
        : TestSuite("meshviz", Type::UNIT)
    {
        AddTestCase(new MeshvizDeliveryTest, TestCase::Duration::QUICK);
    }
};

static MeshvizTestSuite g_meshvizTestSuite; ///< Register the module test suite.
