/*
 * Smash Soda Linux bridge - shared memory protocol.
 *
 * Both the native Linux helpers (ssb-padd, ssb-capture) and the Windows build
 * of Smash Soda (running under Wine/Proton) map files in the bridge directory
 * ($XDG_RUNTIME_DIR/smashsoda). Files are plain MAP_SHARED mappings, so no
 * sockets or syscalls cross the Wine boundary.
 *
 * Keep this header C89/C++ compatible: it is included from both sides.
 */
#ifndef SSB_PROTO_H
#define SSB_PROTO_H

#include <stdint.h>

#define SSB_PADS_FILE   "pads.shm"
#define SSB_VIDEO_FILE  "video.shm"
#define SSB_INPUT_FILE  "input.shm"
#define SSB_AUDIO_FILE  "audio.shm"

/* ---------------------------------------------------------------- gamepads */

#define SSB_PADS_MAGIC   0x53534250u /* 'SSBP' */
#define SSB_PADS_VERSION 1u
#define SSB_MAX_PADS     16

#define SSB_PAD_X360 0u
#define SSB_PAD_DS4  1u

/* Same bit layout as XUSB_GAMEPAD_* (XInput) */
struct ssb_pad {
	volatile uint32_t seq;          /* writer bumps after every state change   */
	volatile uint32_t active;       /* 1 = plugged in                          */
	volatile uint32_t type;         /* SSB_PAD_*                               */
	volatile uint32_t vidpid;       /* (vid << 16) | pid                       */
	volatile uint16_t buttons;      /* XUSB button mask                        */
	volatile uint8_t  lt, rt;
	volatile int16_t  lx, ly, rx, ry; /* XInput convention (up = positive)     */
	volatile uint32_t rumble_seq;   /* daemon bumps when motors change         */
	volatile uint8_t  rumble_large;
	volatile uint8_t  rumble_small;
	volatile uint16_t reserved0;
	volatile uint32_t index;        /* daemon fills: 0-based device number     */
};

struct ssb_pads {
	uint32_t magic;
	uint32_t version;
	volatile uint32_t heartbeat;    /* daemon increments ~100 Hz               */
	uint32_t reserved[5];
	struct ssb_pad pads[SSB_MAX_PADS];
};

/* ------------------------------------------------------------------- video */

#define SSB_VIDEO_MAGIC   0x53534256u /* 'SSBV' */
#define SSB_VIDEO_VERSION 1u
#define SSB_VIDEO_SLOTS   3
#define SSB_VIDEO_MAX_W   3840
#define SSB_VIDEO_MAX_H   2160
#define SSB_VIDEO_SLOT_BYTES ((uint64_t)SSB_VIDEO_MAX_W * SSB_VIDEO_MAX_H * 4)
#define SSB_VIDEO_HDR_BYTES  4096

/* GPU mode: frames never touch system memory. The helper copies each frame GPU-to-GPU into a ring of CUDA
 * buffers and exports them with CUDA IPC; the Windows side (Proton) opens the same buffers and copies from
 * them straight into the encoder. `latest_slot`/`latest_seq` say which buffer holds the newest frame. */
#define SSB_GPU_SLOTS 3
#define SSB_GPU_OFFSET 256   /* byte offset of struct ssb_gpu inside the video file header */

#define SSB_GPU_CONSUMER_UNKNOWN 0u
#define SSB_GPU_CONSUMER_OK      1u
#define SSB_GPU_CONSUMER_FAILED  2u

struct ssb_gpu {
	volatile uint32_t enabled;        /* helper: 1 while frames are delivered through the CUDA ring        */
	volatile uint32_t generation;     /* helper: bumped whenever the ring is (re)allocated                 */
	uint32_t slots;
	uint32_t width, height, pitch;    /* BGRx/BGRA, pitch in bytes                                         */
	volatile uint32_t latest_slot;
	volatile uint32_t latest_seq;
	volatile uint32_t consumer_state; /* Windows side: SSB_GPU_CONSUMER_*                                  */
	uint32_t device;                  /* CUDA device ordinal the ring lives on                             */
	char handle[SSB_GPU_SLOTS][64];   /* CUipcMemHandle per slot                                           */
};

/* Frames are BGRA, tightly packed per row (stride = width * 4). The writer
 * fills slot (frame_seq % SSB_VIDEO_SLOTS) and then publishes by storing the
 * slot's new sequence number in `latest`. The reader copies from
 * slots[latest % SSB_VIDEO_SLOTS] and re-checks `latest` afterwards. With 3
 * slots a reader that copies in under a frame time never sees a torn frame. */
struct ssb_video_slot_info {
	volatile uint32_t width;
	volatile uint32_t height;
};

struct ssb_video {
	uint32_t magic;
	uint32_t version;
	volatile uint32_t heartbeat;      /* capture helper bumps once per second  */
	volatile uint32_t latest;         /* sequence number of newest frame (0=none) */
	struct ssb_video_slot_info info[SSB_VIDEO_SLOTS];
	volatile uint32_t fps;
	/* Logical (compositor) geometry, used to map guest pointer coordinates back
	 * onto the Linux desktop: the captured monitor and the whole desktop bounds. */
	volatile int32_t mon_x, mon_y, mon_w, mon_h;
	volatile int32_t desk_x, desk_y, desk_w, desk_h;
	/* Control: the app sets want_capture while a room is hosted; the helper only opens the
	 * screen-share picker (and streams) while it is set. state: see SSB_CAP_*. */
	volatile uint32_t want_capture;
	volatile uint32_t state;
	volatile uint32_t refresh_mhz;    /* helper: refresh rate of the shared display (millihertz); 0 = unknown     */
	volatile uint32_t want_w;         /* app: requested stream resolution; 0 = keep the host's native resolution */
	volatile uint32_t want_h;
	volatile uint32_t capture_mode;   /* app: 0 default, 1 gpu (zero-copy), 2 gl (GPU download), 3 shm (compositor readback) */
	volatile uint32_t source_types;   /* app: 0 default, 1 screen, 2 window, 3 either                                        */
	volatile uint32_t remember;       /* app: 1 = remember the screen choice instead of asking on every Host press          */
	volatile uint32_t change_request; /* app: incremented to ask for a new screen/window without stopping the room          */
	volatile uint32_t bitrate_total_kbps; /* app: upload budget for the whole room (0 = no limit); split between guests     */
	volatile uint32_t guest_count;        /* app: guests currently connected                                                */
	/* pixel data starts at SSB_VIDEO_HDR_BYTES */
};

#define SSB_CAP_IDLE      0u
#define SSB_CAP_WAITING   1u /* picker is open / starting */
#define SSB_CAP_STREAMING 2u
#define SSB_CAP_FAILED    3u /* cancelled or error; press Host again to retry */

#define SSB_VIDEO_FILE_BYTES (SSB_VIDEO_HDR_BYTES + SSB_VIDEO_SLOTS * SSB_VIDEO_SLOT_BYTES)

/* ------------------------------------------------------------------- audio */

#define SSB_AUDIO_MAGIC       0x53534241u /* 'SSBA' */
#define SSB_AUDIO_VERSION     1u
#define SSB_AUDIO_RATE        48000
#define SSB_AUDIO_RING_FRAMES 65536        /* power of two, ~1.36 s of audio */
#define SSB_AUDIO_HDR_BYTES   64

/* Interleaved signed 16-bit stereo at 48 kHz: the desktop's default output
 * (monitor of the default sink). write_pos counts frames since start and is
 * published after the data is in place. */
struct ssb_audio {
	uint32_t magic;
	uint32_t version;
	volatile uint32_t heartbeat;
	uint32_t rate;
	volatile uint64_t write_pos;
	uint32_t reserved[10];
	/* int16_t data[SSB_AUDIO_RING_FRAMES * 2] follows at SSB_AUDIO_HDR_BYTES */
};
#define SSB_AUDIO_FILE_BYTES (SSB_AUDIO_HDR_BYTES + SSB_AUDIO_RING_FRAMES * 2 * 2)

/* --------------------------------------------------------- per-app audio mixer */

#define SSB_APPS_FILE    "audioapps.shm"
#define SSB_APPS_MAGIC   0x53534150u /* 'SSAP' */
#define SSB_APPS_VERSION 1u
#define SSB_APPS_MAX     16

#define SSB_APPCMD_VOLUME 1u   /* value 0..100                                              */
#define SSB_APPCMD_MUTE   2u   /* value 0/1                                                 */
#define SSB_APPCMD_SHARE  3u   /* value 1 = guests hear this app, 0 = only the host does    */

struct ssb_app_entry {         /* 64 bytes */
	uint32_t id;               /* sink-input index (stable while the app's stream lives)    */
	uint8_t  volume;           /* 0..100                                                    */
	uint8_t  muted;
	uint8_t  shared;           /* guests hear it                                            */
	uint8_t  playing;          /* not paused                                                */
	uint32_t reserved[2];
	char     name[48];
};

struct ssb_apps {              /* helper (ssb-audioapps.py) publishes; the app sends commands */
	uint32_t magic;
	uint32_t version;
	volatile uint32_t heartbeat;
	volatile uint32_t count;
	volatile uint32_t cmd_seq;     /* app: written last, after type/id/value                  */
	volatile uint32_t cmd_ack;     /* helper: set to cmd_seq once the command was applied     */
	volatile uint32_t cmd_type;
	volatile uint32_t cmd_id;
	volatile int32_t  cmd_value;
	uint32_t reserved[7];
	struct ssb_app_entry apps[SSB_APPS_MAX];
};

/* ------------------------------------------------- keyboard / mouse -> host */

#define SSB_INPUT_MAGIC   0x53534249u /* 'SSBI' */
#define SSB_INPUT_VERSION 1u
#define SSB_INPUT_RING    1024

#define SSB_EV_KEY        1u  /* a = evdev KEY or BTN code, b = 1 down / 0 up */
#define SSB_EV_REL        2u  /* a = REL_* code, b = delta                     */
#define SSB_EV_ABS        3u  /* a = 0 x / 1 y,  b = 0..65535 (normalised)     */

struct ssb_input_event {
	uint32_t type;
	int32_t  a;
	int32_t  b;
	int32_t  reserved;
};

struct ssb_input {
	uint32_t magic;
	uint32_t version;
	volatile uint32_t heartbeat;      /* daemon                                */
	volatile uint32_t head;           /* writer (Wine) advances                */
	volatile uint32_t tail;           /* reader (daemon) advances              */
	uint32_t reserved[3];
	struct ssb_input_event ring[SSB_INPUT_RING];
};

#endif /* SSB_PROTO_H */
