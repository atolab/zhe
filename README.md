# Overview

Zeno-He (*zhe* for short) is a tiny implementation of the XRCE protocol that does not depend on dynamic allocation or threading. Instead, it is a non-blocking implementation that assumes single threaded use with polling, and a system that can be sized at compile time. *Zhe* can be configured to operate in peer-to-peer mode or to operate as a client that relies on a broker.

*Zhe* does not call *any* operating system functions directly (its use of the standard library functions is currently limited to string.h, stddef.h, limits.h, stdint.h and inttypes.h). It does rely on a small abstraction layer to be provided by the user implementing functions to send a packet without blocking (it may of course be dropped), test addresses for equality and to convert an address to text. What constitutes sending data on a network or what an address looks like is deliberately left undefined. The interface of the abstraction layer is described below.

Please note that it is currently a research project under active development. Only the basic functionality supported by the XRCE protocol has so far been implemented, none of the settings are final, and everything can still change.

# Application interface

*Zhe* requires a notion of time, but it does not retrieve the current time. Instead, all operations are non-blocking and the current time is passed in as a parameter in the various API calls. The current time parameter is invariably named "tnow".

*Note*: currently the state is stored in global variables; it may be that this will be changed to an instance of a struct type, to allow multiple instantiations of XRCE (in equal static configurations). 

## Intialization

*Zhe* needs to be initialized before any operations may be performed, which is split into two functions:

* int **zeno\_init**(const struct zeno\_config \*config, struct zhe\_platform \*pf, zhe\_time\_t tnow)
* void **zeno\_loop\_init**(zhe\_time\_t tnow)

The first initializes the library based on the specified run-time configuration, for which see below for information on run-time configuration. No interpretation is given to the *pf* parameter, it is simply a pointer to an opaque type that is passed unchanged to the abstraction layer.

None of the other functions may be called before **zeno\_init** successfully returns. The return value is 0 on success and negative on failure.

The second function is called to prepare the main polling/operational loop, and initializes some of the time stamps that need to be initialized before starting. Performing this initialization separately means that **zeno\_init** may be called early in the start up of the application code without adverse affects.

## Operation

The only requirement during operation is that

* void **zeno\_housekeeping**(zhe\_time\_t tnow)

is called sufficiently often. It implements all background activity required for discovery, latency budget management and reliability. In consequence, it may send data out via the **zhe\_platform\_send** operation provided by the abstraction layer.

When data is available on the network interface, it is expected that the application invokes

* int **zhe\_input**(const void \* restrict buf, size\_t sz, const struct zhe\_address \*src, zhe\_time\_t tnow)

The *sz* bytes starting at buf will be processed and may lead on the one hand to the transmission of data (in case of the receipt of a NACK or some kinds of discovery data), and on the other hand to the invocation of registered handlers for data the application has subscribed to. The source address *src* is checked against the known peers (or broker, in case of a client).

The return value is the length in bytes of the prefix of buf that consists of complete, valid XRCE messages — if it is *sz*, the entire buffer has been processed. In a packet-based system, there is probably no point in continuing processing the remaining bytes, but in a stream-based system, receipt of new data may mean the decoding can continue.

A malformed message on input may result in a failure to continue decoding. In a stream-based system this may require resetting the connection if progress cannot be made for some time. (*Note*: this is something that will be changed, distinguishing between incomplete and invalid messages.)

## Publishing data

To publish data of a resource *rid* over a conduit *cid*, the

* pubidx\_t **zhe\_publish**(zhe\_rid\_t rid, unsigned cid, int reliable)

must be invoked first to notify the system that the application will be publishing such data. The return value is a local identifier to be identify what is being written in the **zhe\_zeno\_write** function.

The resource id *rid* must be in [1,**ZHE\_MAX\_RID**]; the conduit id *cid* must be in [0,**N\_OUT\_CONDUITS**-1], with the caveat that currently a unicast conduit should not be used unless there is at most one peer.

Notifications to peers (if required) are sent asynchronously by the **zhe\_housekeeping** function. While discovery of a specific publisher is still ongoing, data may not be propagated to the subscribers. The lack of a function to test whether this process is complete will probably be addressed in the near future.

Publishing an update to a resource is done using:

* int **zhe\_write**(zhe\_pubidx\_t pubidx, const void \*data, zhe\_paysize\_t sz, zhe\_time\_t tnow)

where *pubidx* refers to the return value of a previous call to **zhe\_publish** and *data* and *sz* specify a blob of application-defined content.

The return value is 1 if the data was successfully written, 0 if insufficient space was available in the transmit window to store the data.

If **LATENCY\_BUDGET** > 0, the data will not be sent immediately, but rather will be held until the **zhe\_housekeeping** function deems it necessary to send it, or one of the succeeding messages does not meet the conditions for packing the data. It is possible to force the data out at any time by calling:

* void **zhe\_flush**(void)

which immediately passes any buffered packet to the **zhe\_platform\_send** function for transmission.

## Subscribing to data

To subscribe to a resource, the

* subidx\_t **zhe\_subscribe**(zhe\_rid\_t rid, zhe\_paysize\_t xmitneed, unsigned cid, void (\*handler)(zhe\_rid\_t rid, const void \*payload, zhe\_paysize\_t size, void \*arg), void *arg)

needs to be invoked. The resource to subscribe to is specified by *rid*. Upon receipt of data, the *handler*, with parameters:

* *rid*, the resource id of the data
* *payload*, a pointer to the data sent by **zhe\_write**
* *size*, the size of the data
* *arg*, the argument pointer given as the final parameter to **zhe\_subscribe**

The remaining two parameters, *xmitneed* and *cid* may be used to avoid calling the handler and acknowledging the data if conduit *cid* has less than *xmitneed* space available in its transmit window. *Xmitneed* should include all overhead (for which see elsewhere). If *xmitneed* = 0, *cid* is ignored.

Furthermore, if multiple subscriptions to the same resource are taken, their respective handlers are invoked only if the the combined *xmitneed* are satisfied, else none of the handlers are invoked. Thus, for cases where a handler needs to publish reliable data on a single conduit and the amount of data is bounded, it is possible to delay calling the handler until that data can be written. (It is likely that this mechanism will be refined.)

The return value uniquely identifies the subscription (but cannot currently be passed into any other function).

Notifications to peers (if required) are sent asynchronously by the **zhe\_housekeeping** function. While discovery of a specific subscription is still ongoing, data may not yet be propagated to it. The lack of a function to test whether this process is complete will probably be addressed in the near future.

# System interface

These are the functions that must be provided by the application for use by *zhe*:

* int **zhe\_platform\_addr\_eq**(const struct zhe\_address \*a, const struct zhe\_address \*b)
* size\_t **zhe\_platform\_addr2string**(const struct zhe\_platform \*pf, char \* restrict str, size\_t size, const struct zhe\_address \* restrict addr)
* int **zhe\_platform\_send**(struct zhe\_platform *pf, const void * restrict buf, size\_t size, const struct zhe\_address * restrict dst)

The first shall return 1 if the two addresses *a* and *b* are equal, and 0 otherwise.

The second shall convert the address in *addr* to a standard string representation no longer than **TRANSPORT\_ADDRSTRLEN**-1, and store the result into *str*.
 No more than *size* bytes shall be written to *str*, and *str* shall be null-terminated. *Size* must
 consequently be > 0. The number of characters written into *str* shall be returned. (Failure is not an
 option.)
 
The third shall send the packet in *buf* of *size* bytes to address *dst* and return the number of bytes written (which
 must be *sz* for now, sending a packet only partially is not yet supported); or 0 to indicate nothing was written, **SENDRECV\_HANGUP** to indicate the
 other side hung up on us (i.e., when using TCP), or **SENDRECV\_ERROR** for unspecified, and therefore
 fatal errors. It is assumed to be non-blocking.

Then, if **ENABLE\_TRACING** evaluates to true, a tracing function analogous to **fprintf** (and interpreting the format string in the same manner) must be provided:

* void **zhe\_platform\_trace**(struct zhe\_platform \*pf, const char \*fmt, ...)

Finally, the platform specification must include a definition of a macro **zhe_assert**, which is the equivalent of the standard **assert** macro. That is:

```
#include <assert.h>
#define zhe_assert(x) assert(x)
```

is an excellent choice on a hosted environment.

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
* **ZHE\_MAX\_RID** is the highest allowed resource id. The subscription table is direct-mapped on resource id when **ZHE\_MAX\_SUBSCRIPTIONS** is over a threshold (currently 32), and this is table is what requires the limit on the resource ids.
* **ZHE\_RID\_SIZE** is the type used to represent resource ids internally. On the wire they are always variable-length encoded. A smaller type reduces the footprint slightly.

## Resource IDs

*Note*: no distinction is made between RIDs and SIDs yet. This is *not* in accordance with the XRCE specification proposals and will need to be changed in the future.

# Run-time configuration

All run-time configuration is done through the value of an object of type **struct zhe\_config**, passed by reference to **zhe\_init()**, which transforms or copies the values it reuqires.

## Peer ID

The peer ID is set using the **idlen** and **id** fields. Together these constitute a non-empty sequence of at most **PEERID\_SIZE** bytes.

## Addressing

The scouting address is set using the **scoutaddr** string. The port number in this *IP*:*PORT* pair determines the port used for all multicasts.

The addresses of multicast groups to join are specified as an array of **n\_mcgroups\_join** strings in **mcgroups\_join**. Each one should be in the format *IP*:*PORT*, though the port of course is meaningless given that they are all joined on the one socket bound to the port specified in the scouting address.

Multicast conduits use the addresses specified as **n_mconduit\_dstaddrs** strings in the **mconduit\_dstaddrs**, again as *IP*:*PORT* pairs, and with the requirement that the ports all be the same as the one used for the scouting address. The number of addresses must match the number of configured multicast output conduits, or **(N\_OUT\_CONDUITS - HAVE\_UNICAST\_CONDUIT)**.