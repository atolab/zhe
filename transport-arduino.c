#if defined ARDUINO || defined __APPLE__

#include <assert.h>
#include <string.h>

#include "zeno-config.h"
#include "transport-arduino.h"

#ifndef ARDUINO
static void serial_begin(int baud) { }
static void serial_write(uint8_t octet) { }
static void serial_println(void) { }
static uint8_t serial_read(void) { return 0; }
static int serial_available(void) { return 0; }
static struct {
    void (*begin)(int baud);
    void (*write)(uint8_t octet);
    void (*println)(void);
    uint8_t (*read)(void);
    int (*available)(void);
} Serial = {
    .begin = serial_begin,
    .write = serial_write,
    .println = serial_println,
    .read = serial_read,
    .available = serial_available
};
#endif

static struct zeno_transport *arduino_new(const struct zeno_config *config, zeno_address_t *scoutaddr)
{
    memset(scoutaddr, 0, sizeof(*scoutaddr));
    Serial.begin(115200);
    return (struct zeno_transport *)&Serial;
}

static void arduino_free(struct zeno_transport * restrict tp)
{
}

static size_t arduino_addr2string(char * restrict str, size_t size, const zeno_address_t * restrict addr)
{
    assert(size > 0);
    str[0] = 0;
    return 0;
}

static ssize_t arduino_send(struct zeno_transport * restrict tp, const void * restrict buf, size_t size, const zeno_address_t * restrict dst)
{
    size_t i;
    assert(size <= TRANSPORT_MTU);
#if TRANSPORT_MODE == TRANSPORT_PACKET
    Serial.write(0xff);
    Serial.write(0x55);
#if TRANSPORT_MTU > 255
#error "PACKET mode currently has TRANSPORT_MTU limited to 255 because it writes the length as a single byte"
#endif
    Serial.write((uint8_t)size);
#endif
    for (i = 0; i != size; i++) {
        Serial.write(((uint8_t *)buf)[i]);
    }
#if TRANSPORT_MODE == TRANSPORT_PACKET
    Serial.println();
#endif
    return size;
}

#if TRANSPORT_MODE == TRANSPORT_PACKET
static int read_serial(void)
{
    static uint8_t serst = 0;
    if (Serial.available()) {
        uint8_t c = Serial.read();
        switch (serst) {
            case 0:
                serst = (c == 0xff) ? 255 : 0;
                break;
            case 255:
                serst = (c == 0x55) ? 254 : 0;
                break;
            case 254:
                if (c == 0 || c > MTU) {
                    serst = 0; /* ERROR + blinkenlights? */
                } else {
                    serst = c;
                    inp = 0;
                }
                break;
            default:
                inbuf[inp++] = c;
                if (--serst == 0) {
                    return 1;
                }
                break;
        }
    }
    return 0;
}
#endif

static ssize_t arduino_recv(struct zeno_transport * restrict tp, void * restrict buf, size_t size, zeno_address_t * restrict src)
{
#if TRANSPORT_MODE == TRANSPORT_STREAM
    size_t n = 0;
    while (n < size && Serial.available()) {
        ((uint8_t *) buf)[n++] = Serial.read();
    }
    return n;
#else
#error "couldn't be bothered to integrate Arduino code fully"
#endif
}

static int arduino_addr_eq(const struct zeno_address *a, const struct zeno_address *b)
{
    return 1;
}

zeno_transport_ops_t transport_arduino = {
    .new = arduino_new,
    .free = arduino_free,
    .addr2string = arduino_addr2string,
    .addr_eq = arduino_addr_eq,
    .send = arduino_send,
    .recv = arduino_recv
};

#endif
