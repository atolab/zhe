# Application interface

*Zhe* requires a notion of time, but it does not retrieve the current time. Instead, all operations are non-blocking and the current time is passed in as a parameter in the various API calls. The current time parameter is invariably named "tnow".

*Note*: currently the state is stored in global variables; it may be that this will be changed to an instance of a struct type, to allow multiple instantiations of *zhe* (in equal static configurations). 

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

The return value is the length in bytes of the prefix of buf that consists of complete, valid Zenoh messages — if it is *sz*, the entire buffer has been processed. In a packet-based system, there is probably no point in continuing processing the remaining bytes, but in a stream-based system, receipt of new data may mean the decoding can continue.

A malformed message on input may result in a failure to continue decoding. In a stream-based system this may require resetting the connection if progress cannot be made for some time. (*Note*: this is something that will be changed, distinguishing between incomplete and invalid messages.)

## Declaring Resources

A resource with id *rid* and URI *uri* can be declared using:

* bool **zhe\_declare\_resource**(zhe\_rid\_t rid, const char *uri)

provided the use of URIs has been enabled by defining **ZHE\_MAX\_URISPACE** > 0. The return value indicates where the declaration was successful. Currently, a definition will be accepted if (1) *rid* is locally unknown and no local publications or subscriptions exist; or (2) if it is bound to *uri*. A declaration is sent to all peers, and the peers will store the definition internally if the same conditions are met. If the conditions are not met, an error will be reported back.

Currently aborting a transaction will not result in removing any tentatively defined resources. This is most definitely a bug.

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
