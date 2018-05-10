# Test programs

## Throughput test

The small (and admittedly rather lacking in beauty) test program named "throughput" in the "test" directory is essentially a bidrectional throughput tester with a platform implementation for POSIX + UDP/IP and for POSIX + TCP/IP.

It has three modes:

* a default mode in which all it does is exist for 20s
* mode `-s` in which it only subscribes to resource 1 and sends back a sample of resource 2 whenever it prints a line of output
* mode `-p` mode in which it does what the mode `-s` does, while also sending samples of resource 1 as fast as it can and printing every sample it receives for resource 2.

Resource 1 data is a combination of a key field (valid values are 0 .. 9) and a 32-bit sequence number. The key field can be set from the command-line to distinguish different sources. The published key value is set using the `-k` option and defaults to 0.

Modes `s` (and hence also mode `p`) prints lines similar to:

```
8730.204 [1] 9664345 0 [21488498,15559]
```

This is:

* a time stamp (here: 8730.204; in seconds + milliseconds since some arbitrary time in the past)
* the key of the source (here: 1)
* the last received sequence number (here: 9664345)
* the total number of samples received out of sequence (here: 0; across all sources; the logic is simply that the next sequence number expected for source *k* is one higher than the previously received one, so multiple sources with the same *k*, restarting them or using unreliable communication i.c.w. packet loss all cause it to increase)
* the number of samples delivered and rejected at the protocol level (here: 21488498 and 15559)

Mode `-p` additionally prints lines:

```
8730.448 4702208 [4334]
```

meaning:

* a time stamp
* number of samples sent (here: 4702208)
* number of SYNCH messages sent (here: 4334)

and, for each "pong" message received on resource 2, it prints:

```
8730.208 pong 3440640 8730.161
```

Which is the last sequence number received by the publisher (whatever key) and the timestamp at which it was received. The test program is configured to use a latency budget of 10ms, and besides there is packing going on, so while it is to some extent indicative of the round-trip latency, it is not a proper measurement of this property.

The default is to enable full tracing, which is a bit too much, the `-q` option will provide a more reasonable amount of output, mostly related to discovery.

The `-X` option can be used to simulate packet loss on transmission, its argument is a percentage. (This is implemented in the UDP part of the platform code.)

A quick test is to run: "./throughput -pq -k *k*" on a number of machines, each with a different *k*. Following some initial prefix of traces, this should produce an output reminiscent of:

```
8728.416 4374528 [4034]
8729.186 [0] 69332110 0 [20668734,15559]
8729.186 [1] 9500308 0 [20668734,15559]
8729.186 [2] 5477714 0 [20668734,15559]
8729.186 [4] 3903923 0 [20668734,15559]
8729.186 [5] 3276800 0 [20668734,15559]
8729.432 4538368 [4184]
8730.106 pong 3424256 8730.102
8730.204 [0] 69496035 0 [21488498,15559]
8730.204 [1] 9664345 0 [21488498,15559]
8730.204 [2] 5641750 0 [21488498,15559]
8730.204 [4] 4067848 0 [21488498,15559]
8730.204 [5] 3440640 0 [21488498,15559]
```

## Roundtrip

The roundtrip test program is helpfully named "roundtrip" and is located in the same test directory. The roundtrip program is really primitive in that it one *must* start the server first by running `roundtrip pong`, then start `roundtrip ping` and hope that the 1s delay built-in to ping is sufficient to cover the discovery time. It can only do reliable communication and does not tolerate sample loss at all.

Typical output (on a pair of RPi3's) is:

```
7732.156 handle_mdeclare 0x7edcc044 seq 8 peeridx 0 ndecls 1 intp 1
7732.156 handle_dresult 0 intp 1 | commitid 0 status 0 rid 0
7732.156 handle_mdeclare 0 .. packet done
starting loop
7732.933 handle_msynch peeridx 0 cid 0 seqbase 0 cnt 1
min        90%        99%        max        avg
287.498    324.217    397.237    21360.5    316.641
231.822    313.852    351.299    9494.72    264.402
182.394    249.009    329.581    9019.83    205.631
183.592    216.144    297.706    4500.07    199.605
182.811    214.998    297.185    4477.68    199.208
```

A "raw" UDP roundtrip takes about 160µs minimum, and one using a bare DDSI-stack some 245µs.
