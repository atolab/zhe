/* -*- mode: c++; c-basic-offset: 4; fill-column: 95; -*- */

#define restrict

#include <string.h>
extern "C" {
#include "zhe.h"
#include "platform-arduino.h"
}
#include "zhe-assert.h"

static const uint8_t peerid[] = { 'z', 'b', 'o', 't' };
static uint8_t inbuf[TRANSPORT_MTU];
static uint8_t inp;
static struct zhe_address dummyaddr;

#define RID_BASE     ((zhe_rid_t)0x00010000) /* unique for this mBot */
#define RID_DISTANCE (RID_BASE + 0x1)
#define RID_MOTOR    (RID_BASE + 0x2)

#define BLINKENLIGHTS 0

#if BLINKENLIGHTS

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wnarrowing"
#include "mCore.h"
#pragma GCC diagnostic pop

static MeDCMotor MotorL(M1);
static MeDCMotor MotorR(M2);
static MeUltrasonic ultr(PORT_3);

struct motorstate {
    int16_t speedL;
    int16_t speedR;
};

MeRGBLed rgb;
MeBuzzer buzzer;

struct blinker {
    uint32_t q;
    uint8_t st;
    uint8_t qn;
    uint8_t rgb[3];
    uint32_t lt;
};
struct blinker blink;

void blinkbyte(uint8_t x)
{
    if (blink.qn < 32) {
        blink.q |= (uint32_t)x << blink.qn;
        blink.qn = (blink.qn < 24) ? blink.qn + 8 : 32;
    }
}

void flashled(uint8_t r, uint8_t g, uint8_t b)
{
    blink.rgb[0] = r; blink.rgb[1] = g; blink.rgb[2] = b;
}

void led0blinker(void)
{
    uint32_t t = millis();
    switch (blink.st & 3) {
        case 0: /* quiescent */
            if (blink.rgb[0] | blink.rgb[1] | blink.rgb[2]) { /* flash led than dark */
                rgb.setColorAt(0, blink.rgb[0], blink.rgb[1], blink.rgb[2]);
                rgb.show();
                blink.rgb[0] = blink.rgb[1] = blink.rgb[2] = 0;
                blink.st = (blink.st & 0xfc) | 1;
                blink.lt = t + 100;
            } else if (blink.qn > 0) { /* blink new bits, flash white first */
                rgb.setColorAt(0, 15, 15, 15);
                rgb.show();
                blink.st = (blink.st & 0xfc) | 3;
                blink.lt = t + 50;
            }
            break;
        case 1: /* flashing blink.rgb */
            if (t < blink.lt) {
                break;
            }
            rgb.setColorAt(0, 2, 2, 2);
            rgb.show();
            blink.st = (blink.st & 0xfc) | 2;
            blink.lt = t + 100;
            break;
        case 2: /* blanking */
            if (t < blink.lt) {
                break;
            }
            blink.st &= 0xfc;
            break;
        case 3: /* blinking bits */
            if (t < blink.lt) {
                break;
            } else if (blink.qn == 0 || (blink.rgb[0] | blink.rgb[1] | blink.rgb[2])) {
                /* no more bits to blink, or flash prempts */
                rgb.setColorAt(0, 2, 2, 2);
                rgb.show();
                blink.st = (blink.st & 0xfc) | 2;
                blink.lt = t + 50;
            } else {
                uint8_t x = blink.q & 1;
                blink.q >>= 1;
                blink.qn--;
                if (blink.st & 0x8) {
                    /* 0 => purple, 1 => cyan */
                    rgb.setColorAt(0, x ? 0 : 10, x ? 10 : 0, 5);
                } else {
                    /* 0 => red, 1 => green */
                    rgb.setColorAt(0, x ? 0 : 10, x ? 10 : 0, 0);
                }
                rgb.show();
                blink.st ^= 0x8;
                blink.lt = t + 50;
            }
            break;
    }
}

void xrce_panic(uint16_t line, uint16_t code)
{
    uint32_t tblink;
    uint8_t bf = 0;
    
    /* Give the rest of the code a chance to do something (stop the engines ...) */
    pre_panic_handler();
    
    /* The right (starboard) LED flashes blue than blinks the line number and the code as two
       16 bit numbers from lsb to msb using the led0 blinker (which invariably involves a flash
       of white at the start).  It does so by simply overwriting its state to get the desired
       result.  The left LED blinks purple at ~3Hz.  */
    blink.qn = 0;
    blink.st = 0;
    while (1) {
        if ((blink.st & 3) == 0 && blink.qn == 0) {
            /* led0blinker all done - restart */
            blink.st = 0;
            blink.qn = 32;
            blink.q = ((uint32_t)code << 16) | line;
            blink.rgb[0] = 0; blink.rgb[1] = 0; blink.rgb[2] = 15;
        }
        led0blinker();
        if (millis() > tblink) {
            bf++;
            if ((bf & 1) == 0) {
                rgb.setColorAt(1, 0, 0, 0);
            } else {
                rgb.setColorAt(1, 15, 0, 15);
            }
            rgb.show();
            tblink = millis() + 167;
        }
    }
}

#else

void xrce_panic(uint16_t line, uint16_t code)
{
    while (1) { }
}

#endif /* BLINKENLIGHTS */

size_t zhe_platform_addr2string(const struct zhe_platform *pf, char * restrict str, size_t size, const zhe_address_t * restrict addr)
{
    zhe_assert(size > 0);
    str[0] = 0;
    return 0;
}

int zhe_platform_string2addr(const struct zhe_platform *pf, struct zhe_address * restrict addr, const char * restrict str)
{
    memset(addr, 0, sizeof(*addr));
    return 1;
}

int zhe_platform_send(struct zhe_platform *pf, const void * restrict buf, size_t size, const zhe_address_t * restrict dst)
{
    size_t i;
    zhe_assert(size <= TRANSPORT_MTU);
#if TRANSPORT_MTU > 255
#error "PACKET mode currently has MTU limited to 255 because it writes the length as a single byte"
#endif
    Serial.write((uint8_t)size);
    for (i = 0; i != size; i++) {
        Serial.write(((uint8_t *)buf)[i]);
    }
    return (int)size;
}

int zhe_platform_addr_eq(const struct zhe_address *a, const struct zhe_address *b)
{
    return 1;
}

void zhe_platform_close_session(struct zhe_platform *pf, const struct zhe_address * restrict addr)
{
}

void zhe_platform_housekeeping(struct zhe_platform *pf, zhe_time_t tnow)
{
}

void pre_panic_handler(void)
{
#if BLINKENLIGHTS
    MotorL.run(0);
    MotorR.run(0);
#endif
}

static void handle_motorstate(zhe_rid_t rid, const void *payload, zhe_paysize_t sz, void *arg)
{
#if BLINKENLIGHTS
    const struct motorstate *ms = (const struct motorstate *)payload;
    if (sizeof(*ms) > sz) {
        /* malformed */
        flashled(15,15,3);
        blinkbyte(sz);
        return;
    }
    flashled(0,15,3);
    MotorL.run(ms->speedL);
    MotorR.run(ms->speedR);
#endif
}

void setup(void)
{
    uint8_t state = 0;
    struct zhe_config config;
    memset(&config, 0, sizeof(config));
    config.id = peerid;
    config.idlen = sizeof(peerid);
    config.scoutaddr = &dummyaddr;

#if BLINKENLIGHTS
    buzzer.tone(440, 300);
    delay(50);
    buzzer.noTone();
    rgb.setNumber(2);
    rgb.show();
    rgb.setColor(2, 2, 2);
#endif

    Serial.begin(115200);

    /* Perhaps shouldn't take this time here, but before one calls zhe_init(); for now however, it is safe to do it here */
    zhe_time_t t_state_changed = millis();
    while (state != 2) {
        /* On startup, wait up to 5s for some input, and if some is received, drain
           the input until nothing is received for 1s.  For some reason, garbage
           seems to come in a few seconds after waking up.  */
        zhe_time_t tnow = millis();
        zhe_timediff_t timeout = (state == 0) ? 5000 : 1000;
        if ((zhe_timediff_t)(tnow - t_state_changed) >= timeout) {
            state = 2;
            t_state_changed = tnow;
        } else if (Serial.available()) {
            (void)Serial.read();
            state = 1;
            t_state_changed = tnow;
        }
    }
    zhe_init(&config, NULL, millis());
}

static void handle_input(zhe_time_t tnow)
{
    static zhe_time_t t_progress;
    uint8_t read_something = 0;
    while (inp < sizeof (inbuf) && Serial.available()) {
        inbuf[inp++] = Serial.read();
        read_something = 1;
    }
    if (inp == 0) {
        t_progress = tnow;
    } else {
        /* No point in repeatedly trying to decode the same incomplete data */
        if (read_something) {
            int cons = zhe_input(inbuf, inp, &dummyaddr, tnow);
            if (cons > 0) {
                t_progress = tnow;
                if (cons < inp) {
                    memmove(inbuf, inbuf + cons, inp - cons);
                }
                inp -= cons;
            }
        }

        if (inp == sizeof(inbuf) || (inp > 0 && tnow > t_progress + 300)) {
            /* No progress: discard whatever we have buffered and hope for the best. */
#if BLINKENLIGHTS
            switch (inp) {
                case 1:  flashled(15,0,0); break;
                case 2:  flashled(15,15,0); break;
                case 3:  flashled(0,15,15); break;
                case 4:  flashled(15,0,15); break;
                default: flashled(15,15,15); break;
            }
            for (uint8_t i = 0; i < inp && i < 4; i++) {
                blinkbyte(inbuf[i]);
            }
#endif
            inp = 0;
        }
    }
}

#if BLINKENLIGHTS
static void flashOnButton(void)
{
    static boolean buttonPressed = false;
    boolean currentPressed;
    pinMode(A7, INPUT);
    currentPressed = !(analogRead(A7) > 10);
    if (currentPressed && !buttonPressed) {
        flashled(15,15,0);
    }
    buttonPressed = currentPressed;
}
#endif

void loop(void)
{
    static zhe_time_t tlast_meas = 0, tlast_pub = 0;
    static zhe_pubidx_t pub_distance;
    static zhe_subidx_t sub_motor;

    pub_distance = zhe_publish(RID_DISTANCE, 0, 0);
    sub_motor = zhe_subscribe(RID_MOTOR, 0, 0, handle_motorstate, NULL);

    zhe_start(millis());
    while(1)
    {
        zhe_time_t tnow = millis();
#if BLINKENLIGHTS
        led0blinker();
        flashOnButton();
#endif
        zhe_housekeeping(tnow);
        handle_input(tnow);
        if (tnow > tlast_meas + 10) {
#if BLINKENLIGHTS
            float d = ultr.distanceCm();
#else
            float d = 1.0;
#endif
            tlast_meas = tnow;
            if (tnow >= tlast_pub + 1000 || (0 < d && d < 5 && tnow >= tlast_pub + 300) || (0 < d && d < 2.5 && tnow >= tlast_pub + 100)) {
                uint8_t x = (uint8_t)(10 * d + 0.5);
                zhe_write(pub_distance, &x, 1, tnow);
                tlast_pub = tnow;
            }
        }
    }
}
