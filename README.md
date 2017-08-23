# Overview

Zeno-He (_zhe_ for short) is a tiny implementation of the XRCE protocol that does not depend on dynamic allocation or threading. Instead, it is a non-blocking implementation that assumes single threaded use with polling, and a system that can be sized at compile time. _Zhe_ can be configured to operate in peer-to-peer mode or to operate as a client that relies on a broker.

# Status

The current status is that of a research prototype under development. None of the settings is final, everything can still change.

# Compile-time configuration

Macros, typedefs, functions and variables are referred to in **BOLD** and **bold**.

## Mode and network size

### Client mode: **MAX_PEERS** = 0

In client mode, _zhe_ scouts for a broker (and nothing but a broker) until it finds one, then it establishes a session with that broker. When the session is lost, it goes back to scouting. It does not communicate with any other nodes in the network.

Upon establishing a session, it informs the broker of all its subscriptions and publications and then relies on the broker informing it of all remote subscriptions matching its publications before it sends out any data.

There are small differences in discovery behaviour between client mode and peer-to-peer mode, but generally, it is sensible to simply consider the broker a peer.

### Peer-to-peer mode: **MAX_PEERS** > 0

In peer-to-peer mode, _zhe_ scouts forever, establishing/accepting sessions for other peers and brokers. It informs its peers only of its subscriptions.

With **MAX\_PEERS** > 1 (presumably the most meaningful configuration in peer-to-peer mode), it requires multicast. Conduit 0 is assumed to address all peers.

### Identification

Peers have a unique identifier, a non-empty sequence of at most **PEERID\_SIZE** bytes. This identifier is included in the *open*, *accept*, *close* and *keepalive* messages, the first two of which are used for establishing a session, the third for closing one (as well as for rejecting a request), and the fourth is sent periodically in conbination with the scouts.

For most messages, peers are identified by the source address in the packet. In the UDP/IP version, that is simply the randomly chosen *IP*:*PORT* pair of the one socket used for transmitting data. If the address of the peer changes during its lifetime, the messages sent after the change will be interpreted as sent by (presumably) unknown peer.

Most data from an unknown peer will be dropped, only the session management ones (the above, but also including *scout* and *hello*) are accepted. The peer ID will then be matched with the known peers, and the source address with which the peer is associated updated. This re-establishes normal communication with the peer.

### Timers, &c.

* **SCOUT\_INTERVAL**
* **OPEN\_INTERVAL**
* **OPEN\_RETRIES**
* **LEASE\_DURATION**

## Conduits

_Note_: unicast conduits are supported, but not used in any meaningful way yet. Ultimately, the plan is for it to dynamically switch between unicast and multicast based on the number of subscribing peers, but for the moment, the unicast conduits should not be used.

_Zhe_ distinguishes between _input_ and _output_ conduits, to allow configuring a different numbers of conduits for receiving and for transmitting. The state maintained by an input conduit is typically much less than that maintained by an output conduit, because the output requires a transmit window for providing reliability, whereas the input side simply discards reliable messages received out-of-order.

### Configuration

Configuration is done using the following macros:

* **N\_IN\_CONDUITS** is the number of input conduits. For each peer, a small amount of state is maintained for each of these input conduits. They are all equivalent.
* **N\_OUT\_CONDUITS** is the number of output conduits. All of these, or all-but-one of these are multicast conduits — this depends on the HAVE\_UNICAST\_CONDUIT setting. For the multicast conduits, a transmit window of XMITW\_BYTES bytes is maintained.
* **HAVE\_UNICAST\_CONDUIT** configures whether (1) or not (0) a unicast conduit is present. If present, it is the conduit with id N\_OUT\_CONDUITS-1 and uses a transmit window of XMITW\_BYTES\_UNICAST, unlike the multicast conduits. For a minimal-size client, the advice is to configure only a unicast conduit.

### Addressing

There is, firstly, a *scouting* address, the address to which scout messages are sent that trigger all discover activity. The current UDP/IP implementation assumes that this is a multicast address and unconditionally joins the group using the IP_ADD_MULTICAST socket option.

Up to **MAX\_MULTICAST\_GROUPS** additional multicast groups can be joined (all on the same socket). The groups joined are configured separately from the addresses multicasts are sent to by the multicast conduits to allow mapping in- and output conduits differently on different peers.

### Timing

Scouting, lease management, Timing 

Reliable transmission generally requirs the use of timers. Here there are:

* **ROUNDTRIP\_TIME\_ESTIMATE**
* **MSYNCH\_INTERVAL**
* **LATENCY\_BUDGET**

### Sequence numbers

Sequence number size is configurable (at least in principle):

* **SEQNUM\_SIZE**
* **seq_t** and **sseq_t**

### Fragmentation

Fragmentation is not supported (yet). The maximum sample size is the **TRANSPORT\_MTU** less a few bytes of overhead, with 14-bit sequence numbers the worst-case overhead is:

* conduit id: 0 (conduit 0), 1 (conduits 1–4), 2 (conduits ≥5)
* fixed header: 1 byte
* sequence number: 2 bytes
* resource id: 1–5 bytes (for 32-bit resource ids, but it is VLE encoded so resource ids ≤127 require only 1 byte)
* payload length: for typical MTUs 1–2 bytes

In other words, 12 bytes.

Note that resource IDs and conduit IDs are under application control, and that in practice overhead is expected to be significantly less for most data.

## Number of publications & subscriptions

* MAX\_PUBLICATIONS
* MAX\_SUBSCRIPTIONS
* MAX\_RID
* RID\_T\_SIZE

## Resource IDs

_Note_: no distinction is made between RIDs and SIDs yet. This is *not* in accordance with the XRCE specification proposals and will need to be changed in the future.

# Run-time configuration

All run-time configuration is done through the value of an object of type **struct zeno\_config**, passed by reference to **zeno\_init()**, which transforms or copies the values it reuqires.

## Peer ID

The peer ID is set using the **idlen** and **id** fields. Together these constitute a non-empty sequence of at most **PEERID\_SIZE** bytes.

## Addressing

The scouting address is set using the **scoutaddr** string. The port number in this *IP*:*PORT* pair determines the port used for all multicasts.

The addresses of multicast groups to join are specified as an array of **n\_mcgroups\_join** strings in **mcgroups\_join**. Each one should be in the format *IP*:*PORT*, though the port of course is meaningless given that they are all joined on the one socket bound to the port specified in the scouting address.

Multicast conduits use the addresses specified as **n_mconduit\_dstaddrs** strings in the **mconduit\_dstaddrs**, again as *IP*:*PORT* pairs, and with the requirement that the ports all be the same as the one used for the scouting address. The number of addresses must match the number of configured multicast output conduits, or **(N\_OUT\_CONDUITS - HAVE\_UNICAST\_CONDUIT)**.

## Generic transport options

There is (currently) a possibility of passing one configuration string to the transport layer (**transport\_options**). Currently, only the UDP/IP transport implementation interprets it: if it is not a null pointer, it is interpreted as a decimal representation of the percentage of packets to drop locally (instead of sending them out on the network), if **SIMUL\_PACKET\_LOSS** was true at compile time. No further checking is performed.