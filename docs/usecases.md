# Adumbrated use cases

## 1. A handful of tiny monitoring devices

* 2kB RAM, 30kB Flash, 8-bit CPU
* minimal client
* 20 byte MTU (BLE)
* low data rates, higher under abnormal conditions
* pre-configured discovery
* no need for “proper” topic definitions/URIs

Small number of devices allows for very small peer IDs
Small number of data per device allows for very small numerical resource IDs
Low data rates allow 7-bit sequence numbers
Run directly on top of BLE
Overhead for session establishment (client & broker, so from client perspective only once):

* scout @ 2 bytes
* hello, open & accept @ ~10 bytes each

Overheads for pushing data:

* small unreliable: 4 bytes
* small reliable: 4 bytes

Overhead for pulling data:

* assumption is that one is only reading single values
* PULL message: 4 bytes
* response: 4 bytes (same as pushing reliable data)

Overhead for use of reliable channel:

* typically has 2-byte ACKNACKs every few samples
* data loss typically causes occasional 3-byte SYNCH and 3-byte ACKNACKs + retransmit

Memory footprint of current prototype: ~300 bytes

## 2. A score of medium sized cooperating devices in a cluster

* Cortex-M4 @ 200MHz or similar
* peer-to-peer required
* dynamic discovery
* routing between clusters

Compared to (1):

* Larger IDs to keep device unique even between clusters
* Significantly more resource IDs needed
* Assume 14-bit sequence numbers to allow for exploiting faster networks

Overhead for session establishment:

* O(N^2) sessions to be established, requiring hello, open & accept @ ~20 bytes each
* continuous background scouting at 2 bytes/peer once every few seconds
Overhead for discovery:
* 3 byte overhead for a declare message; in each batch, 1 COMMIT @ 1 byte, and N RESULT @ 2 bytes
* resource declarations: assume 40 bytes (~30 bytes for URI, plus a few QoS overrides)
* subscription declarations: assume 12 bytes (some properties and/or periodic config)
* publication declarations: assume 8 bytes (to leave some room for properties)
* peers using the same resource declarations -> sending them only once generally sufficient

If one were to assume:

* 20 peers
* 30 subscriptions and 10 publications per peer
* on average 10 resources actually declared by each

then about 20kB in declarations total
Overhead for data transfer:

* data: 7 bytes (larger sequence number, larger RID than in (1))
* ACKNACK and SYNCH grow by to 3 and 4 respectively (larger sequence number)

Memory footprint of current prototype: ~60kB

## 3. Minimal wire overhead

* replacement for Modbus-over-TCP
* small devices, must have less than 8 byte overhead
* peer-to-peer or client
* no need for “proper” topic definitions/URIs

Clearly, this use case sits in between (1) and (2) and is already met by the more demanding case of (2). It is not worth working the details.

Memory footprint of current prototype:

* client: ~15kB
* peer, multicast only: ~30kB
* peer, multicast + unicast: ~50kB

## 4. Low-latency, low-performance CPUs

* Cortex-M4 @ 200MHz or similar
* performance critical
* minimal wire overhead
* small messages with low latency
* sometimes larger amounts of data without hard latency requirements
* no need for peer-to-peer
* only a few different resource IDs in one client
* dynamic discovery

This is actually simpler than (2), especially if one configures multiple conduits with different “latency budgets” and timings. Overhead for discovery is less than in (2) because of the lower number of resource IDs used. The use of multiple conduits will in many cases add a byte to a sample, and overhead for individual samples would consequently be comparable (2).

The more interesting bit is the performance and memory use considering the limited capabilities of a Cortex-M4. Our current prototype can meet these requirements using about 10kB of memory. While we don’t have direct performance measurements on this class of hardware, it seems reasonable to expect a rate of 10k+ samples/s on a Cortex-M4, considering that it manages approximately 700k samples/s on a Raspberry Pi3.

Memory footprint of current prototype: ~20kB

## 5. “DDS” in extreme conditions

* low-performance CPUs at @ 50MHz or similar
* real-time, safety-critical, peer-to-peer
* fairly high data rates
* reliability optional
* pre-configured discovery

This is essentially a variant of (2) with similar overhead for data — probably slightly smaller, so 5 or bytes instead of 7. The startup cost of session establishment remains, but the cost of transmitting and processing declarations is avoided. Our current prototype proves that performance in such a machine would be quite alright (see case (4)), and further that it is feasible to build a statically allocated, peer-to-peer implementation with a completely predictable data path.

Memory footprint of current prototype:

* no URIs: ~30kB
* with URIs: ~40kB

## 6. Larger dynamic mesh networks

* mesh networking -> dynamic discovery over multicast, but unicast support essential
* no designated machine that can function as a broker, therefore peer-to-peer is required 
* aggressive sleep schedules: always push can’t work, but only pull also doesn’t work

Here the issue not so much overhead — similar to case (2), but with more resources, subscriptions and publications. That means data overhead probably goes up further (larger resource IDs may be useful), say 10 bytes; and the discovery overhead grows linearly. That means each peer will likely have to share up to 50kB of discovery information with its peers. Session lifetimes can be long enough to deal with short interruptions in WiFi connectivity (and certainly to span sleep cycles), and therefore the discovery is not likely to be repeated often.

This use case requires a mixture of multicast and unicast. Discovery needs to be bootstrapped over multicast, but WiFi mesh networks have atrocious behaviour as soon multicast rates go up. This can only be avoided by using unicast as well. The discovery data will therefore likely be sent via unicast, but WiFi performance is good enough for that to be a non-issue.

Memory footprint of current prototype: ~500kB

## 7. Fog05 (our Fog Computing IaaS, not yet public)

* hardware configuration over “DDS”
* small footprint essential
* peer-to-peer with multicast preferred
* reliant on URIs, dynamic discovery

We expect that it will likely be dynamically creating and deleting URIs and possibly hash the URIs to resource IDs. This will force the resource IDs to be much larger. Furthermore, it is a reasonable assumption that data sizes will be larger, which will increase payload sizes to 2 bytes and likely add fragmentation and probably the use of multiple conduits. Consequently the data overhead likely grows to 16 bytes/sample.

Arguably in a case like this, where URIs are manipulated dynamically, declaring URIs is really part of the application functionality, and so it is quite unfair to classify all the declarations associated with this dynamic behaviour as overhead.

Memory footprint of current prototype: ~10MB

## 8. ROS2

* ROS2 finally got rid of the master for discovery, should not re-introduce it
* industrial usage really needs peer-to-peer
* dynamic discovery

We consider that the feature set of ROS2 is such that it fits well with the feature set of our proposal, and we intend to prove that it is sufficient to run ROS2 without having DDS in the stack. This means all the arguments why ROS2 chose DDS need to be maintained, and it really is just about reducing overhead and increasing performance. Case (7) already covers the worst-case overhead, and this is a major improvement over DDS in any case.

## 9. Connectivity over {LTE, 5G, NB-IoT} + derivatives

* native multicast support; need to exploit it
* often cost is directly related to bandwidth in non-private cells

We expect that private cells to be used in, e.g., factories; in this case peer-to-peer networks using multicast are expected. Overhead will likely be similar to case (2), though perhaps with a larger number of nodes, which increases the amount of discovery data (see case (6)). Over public networks, we expect clients connecting to brokers, either push or pulling data. For overhead, this would end up somewhere between cases (1) and (2).
