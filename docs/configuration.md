# Compile-time configuration

## Mode and network size

### Client mode: **MAX_PEERS** = 0

In client mode, *zhe* scouts for a broker (and nothing but a broker) until it finds one, then it establishes a session with that broker. When the session is lost, it goes back to scouting. It does not communicate with any other nodes in the network.

Upon establishing a session, it informs the broker of all its subscriptions and publications and then relies on the broker informing it of all remote subscriptions matching its publications before it sends out any data.

There are small differences in discovery behaviour between client mode and peer-to-peer mode, but generally, it is sensible to simply consider the broker a peer.

### Peer-to-peer mode: **MAX_PEERS** > 0

In peer-to-peer mode, *zhe* scouts forever, establishing/accepting sessions for other peers and brokers. It informs its peers only of its subscriptions.

With **MAX\_PEERS** > 1 (presumably the most meaningful configuration in peer-to-peer mode), it requires multicast. Conduit 0 is assumed to address all peers (note that the actual addresses are run-time configuration items).

### Identification

Peers have a unique identifier, a non-empty sequence of at most **PEERID\_SIZE** bytes. This identifier is included in the *open*, *accept*, *close* and *keepalive* messages, the first two of which are used for establishing a session, the third for closing one (as well as for rejecting a request), and the fourth is sent periodically in conbination with the scouts.

For most messages, peers are identified by the source address in the packet. In the UDP/IP version, that is simply the randomly chosen *IP*:*PORT* pair of the one socket used for transmitting data. If the address of the peer changes during its lifetime, the messages sent after the change will be interpreted as sent by (presumably) unknown peer.

Most data from an unknown peer will be dropped, only the session management ones (the above, but also including *scout* and *hello*) are accepted. The peer ID will then be matched with the known peers, and the source address with which the peer is associated updated. This re-establishes normal communication with the peer.

### Timers, &c.

Session management — discovery, opening sessions, lease renewal — are all timed activities. As *zhe* is a polling-based, non-threaded library, it requires that the application code invokes its housekeeping function "often enough".

Timing is configured in terms of units of (configurable) **zhe\_time\_t**. Currently only a 1ms timebase has been tested, but the intent is that this timebase is configurable by setting **ZHE\_TIMEBASE** to the number of nanoseconds in one unit of **zhe\_time\_t**.

* **SCOUT\_INTERVAL** is the interval between *scout* and *keepalive* messages. A peer-to-peer *zhe* node sends *scout* messages periodically (provided the housekeeping function is invoked in a timely manner), and *keepalive* only when there is another node. Client-mode doesn't send *scout* messages when connected to a broker.
* **SCOUT\_COUNT** is the number of *scout* messages sent after starting in peer-to-peer mode, 0 means it will scout forever.
* **OPEN\_INTERVAL** is the interval between *open* messages when trying to establish a session with another node. After **OPEN\_RETRIES** without a response, it will abandon the attempt to establish a connection with this peer. It will try again once it receives a *hello* message again.
* **LEASE\_DURATION** is the advertised lease duration of this node and must (for now) be greater than **SCOUT\_INTERVAL**. When a remote node does not receive any message from this node for this long, that remote node will close the session. For now it simply sends *keepalive* messages at a shorter period than **LEASE\_DURATION**.

## Conduits

*Note*: unicast conduits are supported, but not used in any meaningful way yet. Ultimately, the plan is for it to dynamically switch between unicast and multicast based on the number of subscribing peers, but for the moment, the unicast conduits should generally not be used (because it will currently always always address the peer at local index 0).

*Zhe* distinguishes between *input* and *output* conduits, to allow configuring a different numbers of conduits for receiving and for transmitting. The state maintained by an input conduit is typically much less than that maintained by an output conduit, because the output requires a transmit window for providing reliability, whereas the input side simply discards reliable messages received out-of-order.

### Configuration

Configuration is done using the following macros:

* **N\_IN\_CONDUITS** is the number of input conduits. For each peer, a small amount of state is maintained for each of these input conduits. They are all equivalent.
* **N\_OUT\_CONDUITS** is the number of output conduits. All of these, or all-but-one of these are multicast conduits — this depends on the HAVE\_UNICAST\_CONDUIT setting. For the multicast conduits, a transmit window of XMITW\_BYTES bytes is maintained.
* **HAVE\_UNICAST\_CONDUIT** configures whether (1) or not (0) a unicast conduit is present. If present, it is the conduit with id N\_OUT\_CONDUITS-1 and uses a transmit window of XMITW\_BYTES\_UNICAST, unlike the multicast conduits. For a minimal-size client, the advice is to configure only a unicast conduit.

### Addressing

There is, firstly, a *scouting* address, the address to which scout messages are sent that trigger all discover activity. The current UDP/IP implementation assumes that this is a multicast address and unconditionally joins the group using the IP_ADD_MULTICAST socket option.

Up to **MAX\_MULTICAST\_GROUPS** additional multicast groups can be joined (all on the same socket). The groups joined are configured separately from the addresses multicasts are sent to by the multicast conduits to allow mapping in- and output conduits differently on different peers.

### Timing

Reliable transmission generally requires the use of timers for detecting packet loss. In the current version of *zhe*, a *synch* is sent periodically for each conduit over which reliable data has been sent that has not yet been acknowledged by every matched peer. The interval is set by **MSYNCH\_INTERVAL**.

Secondly, it attempts to avoid retransmitting samples more often than is reasonable considering the roundtrip time. For this the **ROUNDTRIP\_TIME\_ESTIMATE** is used, but it should be noted that at a 1ms time resolution, a realistic round-trip time estimate on a fast network can't even be represented. It only matters when there is packet loss, however, and really only affects the 2nd and further retransmit requests, so this limitation should not be a major issue.

Finally, it supports combining messages to a same destination and (for data) on the same conduit. This increases the size of the packets and allows much higher throughput in some cases. To ensure that the data always leaves the node in a timely manner, a packet is always sent after waiting at most for **LATENCY\_BUDGET** units of time (of course depending on the polling rate of the application). If **LATENCY\_BUDGET** is set to 0, it is *always* sent immediately and no packing will occur; if it is set to **LATENCY\_BUDGET\_INF** (= 2^32-1) instead, it will only be sent when full or a message incompatible with the current contents is sent.

### Sequence numbers

Sequence number size is configurable (at least in principle, it hasn't been tested much) by setting **SEQNUM\_SIZE** to the sequence number size in bits. Supported values are 7, 14, 28 or 56 (that is 7 bits/byte for 1, 2, 4 and 8 byte integers). These sizes ensure that the variable length encoding doesn't add a nearly-empty byte for a large part of the sequence number range.

The sequence number size is related to the size of the transmit window, such that the maximum number of reliable samples in the transmit window must be less than 2^(**SEQNUM\_SIZE**-1).

Minimum entry size in the transmit window is 7 bytes (2 bytes administrative overhead of the transmit window itself, 1 byte payload) for up to 63 samples, so 7-bit sequence numbers allow for a transmit window of at least 441 bytes.

### Fragmentation

Fragmentation is not supported (yet). The maximum sample size is the **TRANSPORT\_MTU** less a few bytes of overhead, with 14-bit sequence numbers the worst-case overhead is:

* conduit id: 0 (conduit 0), 1 (conduits 1–4), 2 (conduits ≥5)
* fixed header: 1 byte
* sequence number: 2 bytes
* resource id: 1–5 bytes (for 32-bit resource ids, but it is VLE encoded so resource ids ≤127 require only 1 byte)
* payload length: for typical MTUs 1–2 bytes

In other words, 12 bytes.

Note that resource IDs and conduit IDs are under application control, and that in practice overhead is expected to be significantly less for most data.

### Transmit window for reliable transmission

Each conduit has a transmit window for reliable transmission, of which one has to at least configure the size in bytes, and optionally the size in samples.

There is one pair of settings for all multicast conduits: **XMITW\_BYTES** and **XMITW\_SAMPLES**. The former should be sized respecting that the highest number of samples that can be fit in must be less than 2^(**SEQNUM\_SIZE**-1) if the latter is set to 0 to disable limiting the number of samples, or else to the maximum number of samples that can be held at any point in time. For the unicast conduits (if at all present), the settings are **XMITW\_BYTES\_UNICAST** and **XMITW\_SAMPLES\_UNICAST**.

The structure of the transmit window is always a sequence of (size of message, message) pairs, mapped to the transmit window in a strictly circular manner. This means that dropping samples from the window on acknowledgement and servicing retransmit requests may require scanning the transmit window to locate the oldest sample to keep and/or the first sample to retransmit. As this is potentially a time-consuming operation, it is possible to enable an transmit window "index", a circular array of starting positions in the transmit window, provided **XMITW\_SAMPLES** (and **XMIT\_SAMPLES\_UNICAST** if unicast conduits are present) are both greater than 0. 

## Number of publications & subscriptions

Communication *zhe* is done by publishing updates to resources, which are then distributed to subscribes to those resources, followed by the invocation of a appliation-defined handler.

* **ZHE\_MAX\_PUBLICATIONS** is the maximum number of simultaneous publications.
* **ZHE\_MAX\_SUBSCRIPTIONS** is the maximum number of simultaneous subscriptions. If multiple subscriptions to the same resource are taken, the handlers associated with these subscriptions are called in turn.
* **ZHE\_MAX\_SUBSCRIPTIONS\_PER\_PEER** is the maximum number of simultaneous subscriptions a peer can have before this peer runs out of storage space.
* **ZHE\_MAX\_RESOURCES** is the maximum number of resource URIs.
* **ZHE\_MAX\_RID** is the highest allowed resource id.

## Resource URIs

If **ZHE\_MAX\_URISPACE** > 0, then that much memory is reserved for storing URIs. Internal fragmentation is not an issue as an incremental, compacting garbage collector is used to ensure all memory is actually usable, even when URIs are removed (which currently isn't implemented yet). Also, this adds URI matching in the publish-subscribe administration. URIs can contain wildcards, and so two URIs match if there is a string that matches both.

The current PoC has hopelessly inefficient matching, both in time and space ...

# Run-time configuration

All run-time configuration is done through the value of an object of type **struct zhe\_config**, passed by reference to **zhe\_init()**, which transforms or copies the values it reuqires.

## Peer ID

The peer ID is set using the **idlen** and **id** fields. Together these constitute a non-empty sequence of at most **PEERID\_SIZE** bytes.

## Addressing

The scouting address is set using the **scoutaddr** string. The port number in this *IP*:*PORT* pair determines the port used for all multicasts.

The addresses of multicast groups to join are specified as an array of **n\_mcgroups\_join** strings in **mcgroups\_join**. Each one should be in the format *IP*:*PORT*, though the port of course is meaningless given that they are all joined on the one socket bound to the port specified in the scouting address.

Multicast conduits use the addresses specified as **n_mconduit\_dstaddrs** strings in the **mconduit\_dstaddrs**, again as *IP*:*PORT* pairs, and with the requirement that the ports all be the same as the one used for the scouting address. The number of addresses must match the number of configured multicast output conduits, or **(N\_OUT\_CONDUITS - HAVE\_UNICAST\_CONDUIT)**.
