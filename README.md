# Overview

Zeno-He (*zhe* for short) is a tiny implementation of the XRCE protocol that does not depend on dynamic allocation or threading. Instead, it is a non-blocking implementation that assumes single threaded use with polling, and a system that can be sized at compile time. *Zhe* can be configured to operate in peer-to-peer mode or to operate as a client that relies on a broker.

# Status

The current status is that of a research prototype under development. None of the settings are final, everything can still change.

# Compile-time configuration

Macros, typedefs, functions and variables are referred to in **BOLD** and **bold**.

## Mode and network size

### Client mode: **MAX_PEERS** = 0

In client mode, *zhe* scouts for a broker (and nothing but a broker) until it finds one, then it establishes a session with that broker. When the session is lost, it goes back to scouting. It does not communicate with any other nodes in the network.

Upon establishing a session, it informs the broker of all its subscriptions and publications and then relies on the broker informing it of all remote subscriptions matching its publications before it sends out any data.

There are small differences in discovery behaviour between client mode and peer-to-peer mode, but generally, it is sensible to simply consider the broker a peer.

### Peer-to-peer mode: **MAX_PEERS** > 0

In peer-to-peer mode, *zhe* scouts forever, establishing/accepting sessions for other peers and brokers. It informs its peers only of its subscriptions.

With **MAX\_PEERS** > 1 (presumably the most meaningful configuration in peer-to-peer mode), it requires multicast. Conduit 0 is assumed to address all peers.

### Identification

Peers have a unique identifier, a non-empty sequence of at most **PEERID\_SIZE** bytes. This identifier is included in the *open*, *accept*, *close* and *keepalive* messages, the first two of which are used for establishing a session, the third for closing one (as well as for rejecting a request), and the fourth is sent periodically in conbination with the scouts.

For most messages, peers are identified by the source address in the packet. In the UDP/IP version, that is simply the randomly chosen *IP*:*PORT* pair of the one socket used for transmitting data. If the address of the peer changes during its lifetime, the messages sent after the change will be interpreted as sent by (presumably) unknown peer.

Most data from an unknown peer will be dropped, only the session management ones (the above, but also including *scout* and *hello*) are accepted. The peer ID will then be matched with the known peers, and the source address with which the peer is associated updated. This re-establishes normal communication with the peer.

### Timers, &c.

Session management — discovery, opening sessions, lease renewal — are all timed activities. As *zhe* is a polling-based, non-threaded library, it requires that the application code invokes its housekeeping function "often enough".

Timing is configured in terms of units of **ztime\_t**. Currently only a 1ms timebase has been tested, but the intent is that this timebase is configurable by setting **ZENO\_TIMEBASE** to the number of nanoseconds in one unit of **ztime\_t**.

* **SCOUT\_INTERVAL** is the interval between *scout* and *keepalive* messages. A peer-to-peer *zhe* node sends *scout* messages periodically (provided the housekeeping function is invoked in a timely manner), and *keepalive* only when there is another node. Client-mode doesn't send *scout* messages when connected to a broker.
* **OPEN\_INTERVAL** is the interval between *open* messages when trying to establish a session with another node. After **OPEN\_RETRIES** without a response, it will abandon the attempt to establish a connection with this peer. It will try again once it receives a *hello* message again.
* **LEASE\_DURATION** is the advertised lease duration of this node and must (for now) be greater than **SCOUT\_INTERVAL**. When a remote node does not receive any message from this node for this long, that remote node will close the session. For now it simply sends *keepalive* messages at a shorter period than **LEASE\_DURATION**.

## Conduits

*Note*: unicast conduits are supported, but not used in any meaningful way yet. Ultimately, the plan is for it to dynamically switch between unicast and multicast based on the number of subscribing peers, but for the moment, the unicast conduits should not be used.

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

Minimum entry size in the transmit window is 7 bytes (2 bytes administrative overhead of the transmit window itself, 1 byte payload) for up to 127 samples, so 7-bit sequence numbers allow for a transmit window of at least 448 bytes.

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

Communication *zhe* is done by publishing updates to resources, which are then distributed to subscribes to those resources, followed by the invocation of a appliation-defined handler.

* **MAX\_PUBLICATIONS** is the maximum number of simultaneous publications.
* **MAX\_SUBSCRIPTIONS** is the maximum number of simultaneous subscriptions. If multiple subscriptions to the same resource are taken, the handlers associated with these subscriptions are called in turn.
* **MAX\_RID** is the highest allowed resource id. The subscription table is direct-mapped on resource id when **MAX\_SUBSCRIPTIONS** is over a threshold (currently 32), and this is table is what requires the limit on the resource ids.
* **RID\_T\_SIZE** is the type used to represent resource ids internally. On the wire they are always variable-length encoded. A smaller type reduces the footprint slightly.

## Resource IDs

*Note*: no distinction is made between RIDs and SIDs yet. This is *not* in accordance with the XRCE specification proposals and will need to be changed in the future.

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

# Application interface

*Note*: changes are to be expected, see also below for some changes considered likely to happen in the near future.

## Intialization

*Zhe* needs to be initialized before any operations may be performed. 

* int **zeno\_init**(const struct zeno\_config *config)
* void **zeno\_loop\_init**(void)

The first initializes the library based on the specified run-time configuration. None of the other functions may be called before **zeno\_init** successfully returns. The return value is 0 on success and negative on failure.

The second function is called to prepare the main polling/operational loop, and initializes some of the time stamps that need to be initialized before starting. Performing this initialization separately means that **zeno\_init** may be called early in the start up of the application code without adverse affects.

## Operation

The only requirement during operation is that

* ztime\_t **zeno\_loop**(void)

is called sufficiently often. It will try to drain the network queue (obviously this is unacceptable in a real-time system, but the intent is to eliminate the reading of packets from this function and so eliminate the problem this policy poses). The return value is currently meaningless, but will become the latest time at which it should be invoked next. 

On a "normal" computer, it is quite annoying to have a polling loop at consuming an entire CPU, and so a function is provided to wait until network packets are available. 

* void **zeno\_wait\_input**(ztimediff\_t timeout)

(Naturally, this is likely to change as well.)

## Publishing data

To publish data of a resource *rid* over a conduit *cid*, the

* pubidx\_t **publish**(rid\_t rid, unsigned cid, int reliable)

must be invoked first to notify the system that the application will be publishing such data. The return value is a local identifier to be identify what is being written in the **zeno\_write** function.

The resource id *rid* must be in [1,**MAX\_RID**]; the conduit id *cid* must be in [0,**N\_OUT\_CONDUITS**-1], with the caveat that currently a unicast conduit should not be used unless there is at most one peer.

Actually publishing an update to the resource is done using:

* int **zeno\_write**(pubidx\_t pubidx, zpsize\_t sz, const void *data)

where *pubidx* refers to the return value of a previous call to **publish** and *sz* and *data* specify a blob of application-defined content.

The return value is 1 if the data was successfully written, 0 if insufficient space was available in the transmit window to store the data.

## Subscribing to data

To subscribe to a resource, the

* subidx\_t **subscribe**(rid\_t rid, zpsize\_t xmitneed, unsigned cid, void (\*handler)(rid\_t rid, zpsize\_t size, const void \*payload, void \*arg), void *arg)

needs to be invoked. The resource to subscribe to is specified by *rid*. Upon receipt of data, the *handler*, with parameters:

* *rid*, the resource id of the data
* *size*, the size of the data, as originally specified in the call to **zeno\_write**
* *payload*, the data as originally specified in the call to **zeno\_write**
* *arg*, the argument pointer given as the final parameter to **subscribe**.

The remaining two parameters, *xmitneed* and *cid* may be used to avoid calling the handler and acknowledging the data if conduit *cid* has less than *xmitneed* space available in its transmit window. *Xmitneed* should include all overhead (for which see elsewhere). If *xmitneed* = 0, *cid* is ignored.

Furthermore, if multiple subscriptions to the same resource are taken, their respective handlers are invoked only if the the combined *xmitneed* are satisfied, else none of the handlers are invoked.

Thus, for cases where a handler needs to publish reliable data on a single conduit and the amount of data is bounded, it is possible to delay calling the handler until that data can be written.

(It is likely that this mechanism will be refined.)

The return value uniquely identifies the subscription (but cannot currently be passed into any other function).

# Likely future changes

* Change the way network input is handled: let the application call a function when data is received over the network, rather than having **zeno_loop()** call into the transport implementation to poll for data. This not only gives a better separation of concerns, but also eliminates the problem of having to decide on a policy for when to stop reading the network queue in **zeno_loop()**.
* Instead of initialising the transport from **zeno_init()**, require that the application passes an initialised transport in **zeno_init()** (or via **struct zeno_config**).
* Add an optional **XMITW_SAMPLES** for limiting transmit window size not only in bytes but also in samples: that gives an alternative to calculating the number of samples that fits in the window.
* With that **XMITW_SAMPLES**, also add an option to maintain an circular array indexing sequence number to position in the transmit window, eliminating the scanning of the window when processing an acknowledgement.
* Split **zeno\_write()** into a pair of funtions, reliable and unreliable and remove the "reliable" flag from **publish()**.
* …
