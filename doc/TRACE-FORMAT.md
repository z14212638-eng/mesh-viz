# MeshViz JSONL schema 1

One UTF-8 JSON object per line. Files contain a single run. No concatenated runs.
The final `end` object is mandatory; the viewer rejects interrupted recordings.
Integer nanoseconds are absolute simulation time. MACs are 48-bit, big-endian
integers, rendered as six hexadecimal octets by the viewer.

| type | purpose / key fields |
|---|---|
| `run` | baseline, schema, measurement and capture start/end, bin width |
| `node` | stable physical node ID, name, physical coordinates (not layout positions) |
| `device` | node ID, actual device index, MAC, medium |
| `link` | edge ID, physical endpoints, exact interface MACs, medium, background flag |
| `packet` | source IP-datagram tracking ID, TCP/UDP protocol, destination port, TCP sequence |
| `ppdu` | run-local transmission ID, sender/receiver, time bounds, MCS, MPDU count, first MPDU retry flag, all carried packet IDs |
| `rx` | PPDU ID, addressed receiver node, decoding time, successful/failed MPDU counts, SNR if valid |
| `rx-drop` | PPDU ID, addressed receiver, drop time and actual ns-3 PHY failure reason; SNR unknown |
| `hop` | packet ID, hop, fragment offset, IPv4 TX/RX time, IP bytes, paired delay in milliseconds or -1 |
| `metric` | per-hop receive Mbps, matched delay sample count, mean/max delay (null if none), bin start |
| `total` | aggregate main-flow application goodput Mbps, bin start |
| `end` | recorded PPDU count and dropped-from-capture PPDU count |

PPDU IDs identify physical transmission attempts within this run. They are not
ns-3 node IDs, packet UIDs, or global identities across runs. Current SU scenarios
have one PSDU per PPDU. MU/MLO are outside this recorder's supported scope.

A tracked datagram can contribute to several aggregated PPDUs, several hops, and
several MAC attempts. Never infer a route from a PPDU ID or IP address alone. Use
`ppdu.packets` and the observed `hop` events, then resolve endpoint MACs through
`device.node`. Different interfaces of a relay stay on the same physical node.
TCP sequence/port can help compare higher-layer retransmissions, while each source
IP transmission remains its own trace event. The built-in scenario tracks the
forward main flow; reverse TCP ACK PPDUs are visible but are not a second main-flow
route.

Capture starts, ends and caps can cut a path. An RX corresponding to a captured
hop TX may be emitted after the capture end. Source `packet` metadata can be absent
for packets created before capture. Neither condition authorizes inventing missing
events. Management/control/OBSS PPDUs normally have no tracked main-flow packet IDs.

Reception failure is receiver-specific. A time overlap is not proof of a collision.
The ns-3.48 all-MPDU-failure `RxOutcome` does not initialize SNR; this recorder emits
null instead of reading it. Preamble and other PHY drops use `PhyRxPpduDrop`.
