## TODOs

In no particular order

* Currently resource declarations can fail with a "try again" response if multiple peers try to defined compatible definitions at the same time, but the "try again" part hasn't been implemented yet ...
* Fix the FIXMEs and TODOs in the code :) e.g.:
  * horrible hack of putting a conduit pointer in a publisher when it should just be an index (note the old plan of using CID (-peeridx-1) to identify unicast conduit to peer peeridx ... the plan should still work)
  * subscriber with guard for transmit window space availability: need to clean it up ... give a set of publishers with sizes, and take it from there
* Quite a few of the buffers could be "dynamically" set (instead of fixed with a bunch of macros at compile time) if I let the application code provide them; that would also allow, e.g., different transmit window sizes for each conduit, or even for each peer
* Some hardcoded things in the message packers should really be parameters (e.g., subscription mode)
* Implement selections
* Implement bindings
* URI validation
* QoS from properties
* Improve upper bounds on looking up/checking URIs, resetting resources declared by a peer
* Add a function to allow upper layers to close a session given a source address (needed in particular for the case of short input in length-prefixed streaming mode)
* Improve unicast support
  * certainly to the point where you can specify unicast should be used, and it will then unicast to the unique subscriber (rejecting attempts by other peers to subscribe)
  * perhaps (because it would be nice, automatically switching between uni- and multicast)?
* QoS from resource definitions
  * handle case where a peer starts using a resource without a resource definition present, then a resource declaration for that RID arrives with a contradictory QoS
*  consider removing length prefix in xmitw when there is an index
  * the current index only covers complete samples, so without changing it, length of the latest sample is unknown
* Deleting subscribers, publishers, resources
  * should change the current arrays to a linked list (well, actually, array elements giving the index of the next) to not waste time on unused slots (ideally compile-time selectable)
  * requires synchronization between pushing "fresh" declarations and historical ones (one option: block sending fresh declarations while historical ones are being sent — it is asynchronous already anyway)
* suppress KEEPALIVEs when data has been sent to all peers "recently"
* peer connect/disconnect/reconnect notifications

## Current receiving of data

Can I make it so that it is possible to receive data in parallel to sending data and housekeeping?

For now without considering more complicated features …

### Session management

ic state is almost independent of oc state

* ACK handling is exception (NACK is not a major issue, the set of messages doesn't shrink until ACK'd, just need to be sure to correctly handle NACKs for non-existent messages — NB also when reader NACKs ahead of writer …)
* determining ACK seq for multicast conduits is strictly receiver processing, so also a non-issue
* main thing is that an ACK opens up new space in the transmit window
  * free space check, state update by xmit & state update by ACK need to work together
  * (pos, firstpos) and (seq, seqbase) are primary
    * 64-bit CAS with current 14-bit seq and 16-bit pos can even do a CAS on all of them at the same time
    * only one sender => only (firstpos, seqbase) are updated by two threads, and it can't ever go from ready-to-send to no-space-for-sending => the two tests — for bytes and for messages — can be done one after the other
    * multiple threads would be more problematic, but maybe a sufficiently wide CAS could still make it happen (but then there'd also be the issue of reserving space in the window, not sending out the message, ... — probably a really bad idea to do multiple threads on a single conduit)
  * draining_window is not that major an issue, it introduces hysteresis but is not required for correct operation, so we may err either way
lease expiry happens in housekeeping
* also transition from OPEN+i => OPEN+(i+1) and of OPEN+k => UNKNOWN
* can be done atomically
they interact via peer state & tlease
* receiver can change peer state (CLOSE, HELLO, OPEN, ACCEPT)
  * should not touch EXPIRED state
  * OPEN+i => ESTABLISHED can be done atomically
  * no problem if an additional OPEN goes out …
  * instead of closing a session directly, it could change it to EXPIRED and let housekeeping deal with it
  * lease expiry only for ESTABLISHED
* tlease is updated only by receiver once ESTABLISHED, only by housekeeping when not
  * but they can be split at the cost of a little memory
* if CAS can be done on the pair it should be pretty trivial
* if not, it should still be fairly trivial if lease expiry transitions first to EXPIRED and then to UNKNOWN
  * checking can then be skipped if EXPIRED (and so tlease can still be updated by receiver)
  * receiver ignores packets for an EXPIRED peer
  * need to track whether receiver is active — what if multiple receiver threads?
  * for single receiver, could get away with a few extra states, or one extra bit:
    * ESTABLISHED, IDLE
    * ESTABLISHED, BUSY
    * EXPIRED, IDLE
    * EXPIRED, BUSY
lease expiry would then go from ESTABLISHED, x => EXPIRED, x
receiver transition is ESTABLISHED, IDLE => ESTABLISHED, BUSY => \*, IDLE
housekeeping transitions peers in EXPIRED, IDLE to UNKNOWN
  * add a counter instead of a bit and this works for multiple receive threads
  * indeed might even work for multiple threads in general?

session management seems quite feasible

### Outgoing packet buffer

* it is relevant for sending ACKs and retransmitting messages (and, perhaps, PONGs)
  * offloading to housekeeping could be an option, but it increases latency
  * perhaps it could be an option?
* session management responses can fairly easily be offloaded to housekeeping
  * a few µs latency doesn't matter so much there
* another option would be to have a buffer per "thread", that trivially separates output from receive path and output from other path
  * probably the best option — and otherwise? if I do want to use a single one?
    * would have to reserve space, not send until done filling it
    * would need a way to continue writing while sending => requires multiple buffers
    * max 2 concurrent "threads" => max 2 buffers, but then why bother with having an integrated buffer?
* what about the consequences for the platform side?
  * if sending responses is offloaded to housekeeping, there are none
  * otherwise it needs to be able to handle two parallel streams of packets
  * (1) not my problem, and (2) I think there is plenty of modern hardware capable of doing something like this efficiently

### Declarations

If session cleanup is always done by housekeeping, and if all declaration publishing remains part of housekeeping, then declaration publishing simply need not be concerned with session cleanup.

All sending to be done on same thread as housekeeping — or else by explicitly serializing the operations using a mutex. Then sending is safe, too.

Absent cancellations/forgetting?

_New pubs, subs_

Need to pushed out, but that's normal writing. Again on the assumption that housekeeping is on the same thread as the application code, should be mostly ok.

* need to match with known remote subscriptions, though
* that interferes with incoming declarations
* currently bitmap with a bit per publisher, set iff remote subscriptions exist

So CAS should do the trick (perhaps even an atomic bitwise-or)

_Late joiners_

Trigger sending existing declarations when entering ESTABLISHED state (could just be part of housekeeping, no need to actually make a call if reset_peer() resets the counters and housekeeping only does the declaration pushing for ESTABLISHED peers)

_Incoming declarations_

Only the commit operation is potentially difficult, the pre-commit stuff all is exclusively to the receive thread (with the exception of cleaning up, but that is taken care of by scheduling the cleanup wisely, i.e., as describe above).
Without support for forgetting remote declarations, it is only ever setting a bit in a bitmask (so CAS or even atomic bitwise-or suffice). Supporting forgetting remote readers it is a bit trickier because it involves scanner all peers' subscriptions for the RID.

_Disappearance of a peer_

That's easy — all by housekeeping, no interference from receive thread.

