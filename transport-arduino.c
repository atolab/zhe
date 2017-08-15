#ifdef ARDUINO

#include <assert.h>
#include <string.h>

#include "zeno-config.h"
#include "transport-arduino.h"

#ifdef __APPLE__ /* fake it if not a real Arduino */
static unsigned millis(void) { static unsigned m; return m++; }
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

/* The arduino client code is not that important to me right now, so just take the FSM from the old code
   for draining the input on startup */
#define STATE_WAITINPUT    0
#define STATE_DRAININPUT   1
#define STATE_OPERATIONAL  2

static struct zeno_transport *arduino_new(const struct zeno_config *config, zeno_address_t *scoutaddr)
{
    uint8_t state = STATE_WAITINPUT;
    ztime_t t_state_changed = millis();

    memset(scoutaddr, 0, sizeof(*scoutaddr));
    Serial.begin(115200);

    /* FIXME: perhaps shouldn't take this time here, but before one calls zeno_init(); for now however, it is safe to do it here */
    while (state != STATE_OPERATIONAL) {
        /* On startup, wait up to 5s for some input, and if some is received, drain
           the input until nothing is received for 1s.  For some reason, garbage
           seems to come in a few seconds after waking up.  */
        ztime_t tnow = millis();
        ztime_t timeout = (state == STATE_WAITINPUT) ? 5000 : 1000;
        if ((ztimediff_t)(tnow - t_state_changed) >= timeout) {
            state = STATE_OPERATIONAL;
            t_state_changed = tnow;
        } else if (Serial.available()) {
            (void)Serial.read();
            state = STATE_DRAININPUT;
            t_state_changed = tnow;
        }
    }

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

static int arduino_wait(const struct zeno_transport * restrict tp, ztimediff_t timeout)
{
    return Serial.available();
}

zeno_transport_ops_t transport_arduino = {
    .new = arduino_new,
    .free = arduino_free,
    .addr2string = arduino_addr2string,
    .addr_eq = arduino_addr_eq,
    .send = arduino_send,
    .recv = arduino_recv,
    .wait = arduino_wait
};

#endif
