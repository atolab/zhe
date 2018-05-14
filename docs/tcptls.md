# TCP/TLS example code

## TCP

The TCP example code is in examples/platform/platform-tcp.c and examples/platform/platform-tcp.h and maps all traffic to TCP connections. A peer can serve as a TCP client or as a TCP server, or as both at the same time, but it does not deal gracefully when two peers try to connect to each other at the same time (it will establish the connections, then keep on changing the connection to use).

Scouting necessarily works differently with TCP, because connections need to be established first. Thus, the TCP platform code takes a list of IP:PORT addresses to try connecting to, and whenever it succeeds at establishing a connection it will push a SCOUT message to trigger the regular session establishment. The periodic transmission of SCOUT messages still occurs, but they are ultimately sent only over existing connections. The "scout address" configuration setting is treated as a dummy IP:PORT pair, but then used to fake multicasts.

All these things can be improved upon, but for a PoC, it is good enough.

Secondly, everything is done using non-blocking I/O and polling. Connection establishment has a configurable timeout (ZHE_TCPOPEN_MAXWAIT), and this includes the initial message exchange. Failure to receive a SCOUT (or a HELLO response) in time will cause the connection to be closed and retried later on. The retry interval is separately configurable with ZHE_TCPOPEN_THROTTLE. 

# TLS example

The TLS example code is enabled by defining USE_SSL to 1. It extends the handshake to TLS session establishment.

It requires the ZHE_KEYSTORE environment variable to point to a file containing the primary key and the certificate in PEM format (in that order).

```
openssl req -x509 -newkey rsa:4096 -nodes -keyout keyA.pem -out certA.pem -days 365
cat keyA.pem certA.pem > keystoreA.pem
ZHE_KEYSTORE=keystoreA.pem ./spub 127.0.0.1:7447
```
