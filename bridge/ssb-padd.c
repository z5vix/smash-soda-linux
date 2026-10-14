/*
 * ssb-padd - native Linux half of the Smash Soda gamepad / input bridge.
 *
 * Replaces the ViGEmBus kernel driver: the Windows build of Smash Soda (running
 * under Proton) writes controller state into a shared-memory file; this daemon
 * mirrors every active slot as a real uinput gamepad so any Linux game or
 * emulator sees it, and forwards rumble back. It also injects keyboard / mouse
 * events from the same bridge so Parsec guests can drive the Linux desktop.
 *
 * Build: cc -O2 -o ssb-padd ssb-padd.c
 * Needs write access to /dev/uinput.
 */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <linux/input.h>
#include <linux/uinput.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "ssb_proto.h"

#define XUSB_DPAD_UP     0x0001
#define XUSB_DPAD_DOWN   0x0002
#define XUSB_DPAD_LEFT   0x0004
#define XUSB_DPAD_RIGHT  0x0008
#define XUSB_START       0x0010
#define XUSB_BACK        0x0020
#define XUSB_LTHUMB      0x0040
#define XUSB_RTHUMB      0x0080
#define XUSB_LSHOULDER   0x0100
#define XUSB_RSHOULDER   0x0200
#define XUSB_GUIDE       0x0400
#define XUSB_A           0x1000
#define XUSB_B           0x2000
#define XUSB_X           0x4000
#define XUSB_Y           0x8000

static volatile sig_atomic_t g_run = 1;
static void on_sig(int s) { (void)s; g_run = 0; }

static struct ssb_pads *g_pads;
static struct ssb_input *g_in;

struct padstats { double last, max; unsigned n, over20, over50; double window; int steady; };
static struct padstats g_st[SSB_MAX_PADS];
static double now_ms(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec * 1000.0 + t.tv_nsec / 1e6; }

struct padstate {
	int fd;               /* uinput fd, -1 when not created   */
	uint32_t last_seq;
	uint32_t last_rumble_seq;
	int ff_id;            /* unused, kept for symmetry        */
	uint8_t eff_strong[16], eff_weak[16];
	uint16_t buttons;
	int16_t lx, ly, rx, ry;
	uint8_t lt, rt;
	int hatx, haty;
};
static struct padstate g_ps[SSB_MAX_PADS];

static int g_kbd = -1, g_mouse_abs = -1;

static FILE *g_hlog;
static void hlog(const char *fmt, ...)
{
	char line[400];
	struct timespec ts; clock_gettime(CLOCK_REALTIME, &ts);
	struct tm tm; localtime_r(&ts.tv_sec, &tm);
	int n = snprintf(line, sizeof line, "%02d:%02d:%02d.%03d ssb-padd: ", tm.tm_hour, tm.tm_min, tm.tm_sec, (int)(ts.tv_nsec / 1000000));
	va_list ap; va_start(ap, fmt); n += vsnprintf(line + n, sizeof line - n - 2, fmt, ap); va_end(ap);
	line[n++] = '\n'; line[n] = 0;
	fputs(line, stderr);
	if (g_hlog) { fputs(line, g_hlog); fflush(g_hlog); }
}

static void die(const char *m) { perror(m); exit(1); }

static void emit(int fd, int type, int code, int val)
{
	struct input_event ev;
	memset(&ev, 0, sizeof ev);
	ev.type = type; ev.code = code; ev.value = val;
	if (write(fd, &ev, sizeof ev) < 0 && errno != EAGAIN) { /* ignore */ }
}
static void sync_(int fd) { emit(fd, EV_SYN, SYN_REPORT, 0); }

/* ---------------------------------------------------------------- shm open */

static void *map_file(const char *dir, const char *name, size_t size)
{
	char path[512];
	snprintf(path, sizeof path, "%s/%s", dir, name);
	int fd = open(path, O_RDWR | O_CREAT, 0600);
	if (fd < 0) die(path);
	if (ftruncate(fd, (off_t)size) < 0) die("ftruncate");
	void *p = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
	if (p == MAP_FAILED) die("mmap");
	close(fd);
	return p;
}

/* ------------------------------------------------------------ gamepad node */

static void ioc(int fd, unsigned long req, long arg)
{
	if (ioctl(fd, req, arg) < 0) { perror("ioctl"); }
}

static void abs_setup(int fd, int code, int min, int max, int fuzz, int flat)
{
	struct uinput_abs_setup s;
	memset(&s, 0, sizeof s);
	s.code = code;
	s.absinfo.minimum = min; s.absinfo.maximum = max;
	s.absinfo.fuzz = fuzz;   s.absinfo.flat = flat;
	if (ioctl(fd, UI_ABS_SETUP, &s) < 0) perror("UI_ABS_SETUP");
}

static int pad_create(int index, uint32_t vidpid, uint32_t type)
{
	int fd = open("/dev/uinput", O_RDWR | O_NONBLOCK);
	if (fd < 0) { perror("/dev/uinput"); return -1; }

	ioc(fd, UI_SET_EVBIT, EV_KEY);
	ioc(fd, UI_SET_EVBIT, EV_ABS);
	ioc(fd, UI_SET_EVBIT, EV_FF);
	static const int keys[] = { BTN_SOUTH, BTN_EAST, BTN_NORTH, BTN_WEST, BTN_TL, BTN_TR,
		BTN_SELECT, BTN_START, BTN_MODE, BTN_THUMBL, BTN_THUMBR };
	for (unsigned i = 0; i < sizeof keys / sizeof *keys; i++) ioc(fd, UI_SET_KEYBIT, keys[i]);
	ioc(fd, UI_SET_FFBIT, FF_RUMBLE);
	ioc(fd, UI_SET_FFBIT, FF_GAIN);

	abs_setup(fd, ABS_X,  -32768, 32767, 16, 128);
	abs_setup(fd, ABS_Y,  -32768, 32767, 16, 128);
	abs_setup(fd, ABS_RX, -32768, 32767, 16, 128);
	abs_setup(fd, ABS_RY, -32768, 32767, 16, 128);
	abs_setup(fd, ABS_Z,  0, 255, 0, 0);
	abs_setup(fd, ABS_RZ, 0, 255, 0, 0);
	abs_setup(fd, ABS_HAT0X, -1, 1, 0, 0);
	abs_setup(fd, ABS_HAT0Y, -1, 1, 0, 0);

	struct uinput_setup us;
	memset(&us, 0, sizeof us);
	us.id.bustype = BUS_USB;
	us.id.vendor  = (vidpid >> 16) ? (vidpid >> 16) : 0x045e;
	us.id.product = (vidpid & 0xffff) ? (vidpid & 0xffff) : 0x028e;
	us.id.version = 0x0110;
	us.ff_effects_max = 16;
	/* Always present as an Xbox 360 pad: universally supported mapping. */
	(void)type;
	snprintf(us.name, sizeof us.name, "Smash Soda Virtual Pad %d", index + 1);
	if (ioctl(fd, UI_DEV_SETUP, &us) < 0 || ioctl(fd, UI_DEV_CREATE) < 0) {
		perror("uinput create");
		close(fd);
		return -1;
	}
	return fd;
}

static void pad_destroy(struct padstate *p)
{
	if (p->fd >= 0) { ioctl(p->fd, UI_DEV_DESTROY); close(p->fd); }
	memset(p, 0, sizeof *p);
	p->fd = -1; p->ff_id = -1;
}

static int clamp16(int v) { return v < -32768 ? -32768 : v > 32767 ? 32767 : v; }

static void pad_push(struct padstate *p, struct ssb_pad *s)
{
	uint16_t b = s->buttons;
	int fd = p->fd;
#define BTN(code, mask) if ((b ^ p->buttons) & (mask)) emit(fd, EV_KEY, code, (b & (mask)) ? 1 : 0)
	BTN(BTN_SOUTH, XUSB_A); BTN(BTN_EAST, XUSB_B); BTN(BTN_NORTH, XUSB_X); BTN(BTN_WEST, XUSB_Y);
	BTN(BTN_TL, XUSB_LSHOULDER); BTN(BTN_TR, XUSB_RSHOULDER);
	BTN(BTN_SELECT, XUSB_BACK); BTN(BTN_START, XUSB_START); BTN(BTN_MODE, XUSB_GUIDE);
	BTN(BTN_THUMBL, XUSB_LTHUMB); BTN(BTN_THUMBR, XUSB_RTHUMB);
#undef BTN
	int hx = ((b & XUSB_DPAD_RIGHT) ? 1 : 0) - ((b & XUSB_DPAD_LEFT) ? 1 : 0);
	int hy = ((b & XUSB_DPAD_DOWN) ? 1 : 0) - ((b & XUSB_DPAD_UP) ? 1 : 0);
	if (hx != p->hatx) emit(fd, EV_ABS, ABS_HAT0X, hx);
	if (hy != p->haty) emit(fd, EV_ABS, ABS_HAT0Y, hy);
	/* XInput Y is up-positive, evdev is up-negative. */
	int lx = s->lx, ly = clamp16(-(int)s->ly), rx = s->rx, ry = clamp16(-(int)s->ry);
	if (lx != p->lx) emit(fd, EV_ABS, ABS_X, lx);
	if (ly != p->ly) emit(fd, EV_ABS, ABS_Y, ly);
	if (rx != p->rx) emit(fd, EV_ABS, ABS_RX, rx);
	if (ry != p->ry) emit(fd, EV_ABS, ABS_RY, ry);
	if (s->lt != p->lt) emit(fd, EV_ABS, ABS_Z, s->lt);
	if (s->rt != p->rt) emit(fd, EV_ABS, ABS_RZ, s->rt);
	sync_(fd);
	p->buttons = b; p->hatx = hx; p->haty = hy;
	p->lx = lx; p->ly = ly; p->rx = rx; p->ry = ry; p->lt = s->lt; p->rt = s->rt;
}

/* Force-feedback requests arrive as events on the uinput fd. */
static void pad_ff(struct padstate *p, struct ssb_pad *s)
{
	struct input_event ev;
	while (read(p->fd, &ev, sizeof ev) == (ssize_t)sizeof ev) {
		if (ev.type == EV_UINPUT && ev.code == UI_FF_UPLOAD) {
			struct uinput_ff_upload up;
			memset(&up, 0, sizeof up);
			up.request_id = ev.value;
			ioctl(p->fd, UI_BEGIN_FF_UPLOAD, &up);
			if (up.effect.type == FF_RUMBLE && up.effect.id >= 0 && up.effect.id < 16) {
				p->eff_strong[up.effect.id] = up.effect.u.rumble.strong_magnitude >> 8;
				p->eff_weak[up.effect.id]   = up.effect.u.rumble.weak_magnitude >> 8;
			}
			up.retval = 0;
			ioctl(p->fd, UI_END_FF_UPLOAD, &up);
		} else if (ev.type == EV_UINPUT && ev.code == UI_FF_ERASE) {
			struct uinput_ff_erase er;
			memset(&er, 0, sizeof er);
			er.request_id = ev.value;
			ioctl(p->fd, UI_BEGIN_FF_ERASE, &er);
			er.retval = 0;
			ioctl(p->fd, UI_END_FF_ERASE, &er);
		} else if (ev.type == EV_FF && ev.code < 16) {
			if (ev.value > 0) {
				s->rumble_large = p->eff_strong[ev.code];
				s->rumble_small = p->eff_weak[ev.code];
			} else {
				s->rumble_large = 0;
				s->rumble_small = 0;
			}
			s->rumble_seq++;
		}
	}
}

/* --------------------------------------------------------- kbd / mouse out */

static int dev_make_kbd_mouse(void)
{
	int fd = open("/dev/uinput", O_RDWR | O_NONBLOCK);
	if (fd < 0) return -1;
	ioc(fd, UI_SET_EVBIT, EV_KEY);
	ioc(fd, UI_SET_EVBIT, EV_REL);
	for (int k = 1; k < 249; k++) ioc(fd, UI_SET_KEYBIT, k);
	ioc(fd, UI_SET_KEYBIT, BTN_LEFT); ioc(fd, UI_SET_KEYBIT, BTN_RIGHT);
	ioc(fd, UI_SET_KEYBIT, BTN_MIDDLE); ioc(fd, UI_SET_KEYBIT, BTN_SIDE);
	ioc(fd, UI_SET_KEYBIT, BTN_EXTRA);
	ioc(fd, UI_SET_RELBIT, REL_X); ioc(fd, UI_SET_RELBIT, REL_Y);
	ioc(fd, UI_SET_RELBIT, REL_WHEEL); ioc(fd, UI_SET_RELBIT, REL_HWHEEL);
	struct uinput_setup us;
	memset(&us, 0, sizeof us);
	us.id.bustype = BUS_USB; us.id.vendor = 0x1d6b; us.id.product = 0x0104; us.id.version = 1;
	snprintf(us.name, sizeof us.name, "Smash Soda Virtual Keyboard/Mouse");
	if (ioctl(fd, UI_DEV_SETUP, &us) < 0 || ioctl(fd, UI_DEV_CREATE) < 0) { close(fd); return -1; }
	return fd;
}

static int dev_make_abs(void)
{
	int fd = open("/dev/uinput", O_RDWR | O_NONBLOCK);
	if (fd < 0) return -1;
	ioc(fd, UI_SET_EVBIT, EV_KEY);
	ioc(fd, UI_SET_EVBIT, EV_ABS);
	ioc(fd, UI_SET_KEYBIT, BTN_TOUCH);
	ioc(fd, UI_SET_PROPBIT, INPUT_PROP_DIRECT);
	abs_setup(fd, ABS_X, 0, 65535, 0, 0);
	abs_setup(fd, ABS_Y, 0, 65535, 0, 0);
	struct uinput_setup us;
	memset(&us, 0, sizeof us);
	us.id.bustype = BUS_USB; us.id.vendor = 0x1d6b; us.id.product = 0x0105; us.id.version = 1;
	snprintf(us.name, sizeof us.name, "Smash Soda Virtual Pointer");
	if (ioctl(fd, UI_DEV_SETUP, &us) < 0 || ioctl(fd, UI_DEV_CREATE) < 0) { close(fd); return -1; }
	return fd;
}

static int g_abs_x, g_abs_y;
static void input_drain(void)
{
	uint32_t head = g_in->head, tail = g_in->tail;
	int dirty_k = 0, dirty_a = 0;
	while (tail != head) {
		struct ssb_input_event e = g_in->ring[tail % SSB_INPUT_RING];
		tail++;
		switch (e.type) {
		case SSB_EV_KEY:
			if (g_kbd >= 0) { emit(g_kbd, EV_KEY, e.a, e.b ? 1 : 0); dirty_k = 1; }
			break;
		case SSB_EV_REL:
			if (g_kbd >= 0) { emit(g_kbd, EV_REL, e.a, e.b); dirty_k = 1; }
			break;
		case SSB_EV_ABS:
			if (g_mouse_abs >= 0) {
				if (e.a == 0) g_abs_x = e.b; else g_abs_y = e.b;
				dirty_a = 1;
			}
			break;
		}
	}
	g_in->tail = tail;
	if (dirty_k) sync_(g_kbd);
	if (dirty_a) {
		emit(g_mouse_abs, EV_ABS, ABS_X, g_abs_x);
		emit(g_mouse_abs, EV_ABS, ABS_Y, g_abs_y);
		sync_(g_mouse_abs);
	}
}

/* -------------------------------------------------------------------- main */

int main(int argc, char **argv)
{
	const char *dir = argc > 1 ? argv[1] : NULL;
	char dirbuf[400];
	if (!dir) {
		const char *rt = getenv("XDG_RUNTIME_DIR");
		snprintf(dirbuf, sizeof dirbuf, "%s/smashsoda", rt ? rt : "/tmp");
		dir = dirbuf;
	}
	mkdir(dir, 0700);

	{
		char lp[512]; snprintf(lp, sizeof lp, "%s/helpers.log", dir);
		g_hlog = fopen(lp, "a");
	}
	signal(SIGINT, on_sig); signal(SIGTERM, on_sig);
	signal(SIGPIPE, SIG_IGN);

	g_pads = map_file(dir, SSB_PADS_FILE, sizeof *g_pads);
	g_in   = map_file(dir, SSB_INPUT_FILE, sizeof *g_in);
	memset(g_pads, 0, sizeof *g_pads);
	memset(g_in, 0, sizeof *g_in);
	g_pads->magic = SSB_PADS_MAGIC; g_pads->version = SSB_PADS_VERSION;
	g_in->magic = SSB_INPUT_MAGIC;  g_in->version = SSB_INPUT_VERSION;

	for (int i = 0; i < SSB_MAX_PADS; i++) { g_ps[i].fd = -1; g_ps[i].ff_id = -1; }

	g_kbd = dev_make_kbd_mouse();
	g_mouse_abs = dev_make_abs();
	if (g_kbd < 0 || g_mouse_abs < 0)
		fprintf(stderr, "ssb-padd: keyboard/mouse injection unavailable (%s)\n", strerror(errno));

	hlog("ready, bridge dir %s", dir);

	unsigned tick = 0;
	while (g_run) {
		for (int i = 0; i < SSB_MAX_PADS; i++) {
			struct ssb_pad *s = &g_pads->pads[i];
			struct padstate *p = &g_ps[i];
			uint32_t act = s->active;
			if (act && p->fd < 0) {
				p->fd = pad_create(i, s->vidpid, s->type);
				p->last_seq = s->seq - 1;
				s->index = i;
				if (p->fd >= 0) hlog("pad %d plugged in", i);
			} else if (!act && p->fd >= 0) {
				pad_destroy(p);
				hlog("pad %d removed", i);
			}
			if (p->fd < 0) continue;
			uint32_t seq = s->seq;
			if (seq != p->last_seq) {
				pad_push(p, s); p->last_seq = seq;
				struct padstats *st = &g_st[i];
				double t = now_ms();
				if (st->last > 0 && t - st->last > 40 && st->steady >= 30) hlog("STALL: pad %d had no update for %.0f ms during steady input", i, t - st->last);
				if (st->last > 0) st->steady = (t - st->last < 20) ? st->steady + 1 : 0;
				if (st->last > 0) { double g = t - st->last; if (g > st->max) st->max = g; if (g > 20) st->over20++; if (g > 50) st->over50++; }
				st->last = t; st->n++;
				if (st->window == 0) st->window = t;
				if (t - st->window >= 5000) {
					hlog("pad %d: %.0f state updates/s, max gap %.0f ms, gaps >20ms: %u, >50ms: %u",
						i, st->n * 1000.0 / (t - st->window), st->max, st->over20, st->over50);
					st->n = st->over20 = st->over50 = 0; st->max = 0; st->window = t;
				}
			}
			pad_ff(p, s);
		}
		if (g_in) input_drain();
		{
			static double lastLoop = 0; double t = now_ms();
			if (lastLoop > 0 && t - lastLoop > 20) hlog("STALL: daemon loop was delayed %.0f ms (CPU starvation?)", t - lastLoop);
			lastLoop = t;
		}
		g_pads->heartbeat++;
		g_in->heartbeat++;
		tick++;
		struct timespec ts = { 0, 2 * 1000 * 1000 }; /* 2 ms => ~500 Hz */
		nanosleep(&ts, NULL);
	}
	for (int i = 0; i < SSB_MAX_PADS; i++) pad_destroy(&g_ps[i]);
	if (g_kbd >= 0) { ioctl(g_kbd, UI_DEV_DESTROY); close(g_kbd); }
	if (g_mouse_abs >= 0) { ioctl(g_mouse_abs, UI_DEV_DESTROY); close(g_mouse_abs); }
	(void)tick;
	return 0;
}
