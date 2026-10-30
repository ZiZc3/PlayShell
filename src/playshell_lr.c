// PlayShell - the libretro cores: one small frontend for every core built into PlayShell (RetroArch's cores, the same
// interface). Each core's library has its names renamed when it is built (tools/*_lib.sh: pcsx_..., nds_...), so
// several live in playshell.elf; one runs at a time, in its own thread. GPL-3.0 (same author as LiveShell).
//   PlayStation: PCSX-ReARMed (GPL-2.0+)   games roms/ps1 (.cue/.bin, .chd, .pbp, .iso, .m3u)
//                BIOS /data/PlayShell/bios (scph*.bin, optional); memory cards /data/PlayShell/saves/<game>.srm
//                /data/PlayShell/ps1_interpreter: no JIT; /data/PlayShell/ps1_native: no 2x
//   Nintendo DS: melonDS (GPL-3.0)        games roms/nds (.nds); its own BIOS (FreeBIOS), or real ones in /data/PlayShell/bios
//                touch screen: the right stick moves the pointer, R2 touches
//   DOS:         DOSBox Pure (GPL-2.0+)     games roms/dos (.zip, .dosz, .exe, .com, .bat, .iso / .cue)
//   Nintendo 64: Mupen64Plus-Next (GPL-2.0) games roms/n64 (.z64, .n64, .v64); drawn on the CPU (Angrylion)
// The cores' JITs get runnable memory through the kernel (ps_mmap / ps_mprotect below: the PS5 makes memory runnable
// only that way). The picture goes to the home screen as RGB565 (PS1 scaled to 4:3), the sound at 48 kHz.
#include "libretro.h"

#include <errno.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

#include <ps5/kernel.h>

void ps_send_picture(const uint16_t* px, int w, int h);   // playshell_emu.c
void ps_send_sound(const int16_t* stereo, unsigned frames); // playshell_emu.c
void ps_log(const char* line);                              // playshell_emu.c
void ps_step(const char* step);                             // playshell_daemon.cpp: the crash recorder's step

// ---- runnable memory for the JITs ----
// The cores' mmap / mprotect come here (tools/*_lib.sh: -Dmmap=ps_mmap -Dmprotect=ps_mprotect). Memory that must run
// is made so through the kernel; the rest goes to the normal calls.
void* ps_mmap(void* addr, size_t len, int prot, int flags, int fd, off_t off) {
	void* p = mmap(addr, len, prot & ~PROT_EXEC, flags, fd, off);
	if (p != MAP_FAILED && (prot & PROT_EXEC) && kernel_mprotect(getpid(), (intptr_t)p, len, prot) != 0) {
		ps_log("cores: could not make JIT memory runnable");
		munmap(p, len);
		errno = EACCES;
		return MAP_FAILED;
	}
	return p;
}

int ps_mprotect(void* addr, size_t len, int prot) {
	if (prot & PROT_EXEC) {
		return kernel_mprotect(getpid(), (intptr_t)addr, len, prot);
	}
	return mprotect(addr, len, prot);
}

// ---- funopen: DOSBox Pure opens files through a frontend's virtual file system with it ("scheme://" paths only -
// PlayShell gives plain paths, so never); the PS5's libc does not have it: a stand-in that says no ----
FILE* funopen(const void* cookie, int (*readfn)(void*, char*, int), int (*writefn)(void*, const char*, int),
              fpos_t (*seekfn)(void*, fpos_t, int), int (*closefn)(void*)) {
	(void)cookie, (void)readfn, (void)writefn, (void)seekfn, (void)closefn;
	errno = ENOTSUP;
	return NULL;
}

// ---- CPU features (cores check them; the payload SDK has no compiler-rt for __builtin_cpu_*) ----
// The PS5's CPU is always the same AMD Zen 2: the feature bits as compiler-rt numbers them - CMOV 0, MMX 1, POPCNT 2,
// SSE 3, SSE2 4, SSE3 5, SSSE3 6, SSE4.1 7, SSE4.2 8, AVX 9, AVX2 10, SSE4A 11, FMA 14, BMI 16, BMI2 17, AES 18,
// PCLMUL 19.
struct ps_processor_model {
	unsigned int vendor, type, subtype, features[1];
};
struct ps_processor_model __cpu_model = {2 /* AMD */, 0, 0,
                                         {0x7FF | (1u << 11) | (1u << 14) | (1u << 16) | (1u << 17) | (1u << 18) | (1u << 19)}};
// Azahar's networking (local play, not used here) asks for sockatmark, which the PS5's libc does not have
int sockatmark(int fd) {
	(void)fd;
	return 0;
}

int __cpu_indicator_init(void) {
	return 0;
}

// ---- the cores (their renamed libretro functions) ----
#define LR_DECLARE(p)                                                                                                  \
	void   p##retro_set_environment(retro_environment_t);                                                               \
	void   p##retro_set_video_refresh(retro_video_refresh_t);                                                           \
	void   p##retro_set_audio_sample(retro_audio_sample_t);                                                             \
	void   p##retro_set_audio_sample_batch(retro_audio_sample_batch_t);                                                 \
	void   p##retro_set_input_poll(retro_input_poll_t);                                                                 \
	void   p##retro_set_input_state(retro_input_state_t);                                                               \
	void   p##retro_init(void);                                                                                         \
	void   p##retro_deinit(void);                                                                                       \
	bool   p##retro_load_game(const struct retro_game_info*);                                                           \
	void   p##retro_unload_game(void);                                                                                  \
	void   p##retro_run(void);                                                                                          \
	void   p##retro_get_system_av_info(struct retro_system_av_info*);                                                   \
	void   p##retro_set_controller_port_device(unsigned, unsigned);                                                     \
	void*  p##retro_get_memory_data(unsigned);                                                                          \
	size_t p##retro_get_memory_size(unsigned);                                                                          \
	size_t p##retro_serialize_size(void);                                                                               \
	bool   p##retro_serialize(void*, size_t);                                                                           \
	bool   p##retro_unserialize(const void*, size_t);
#define LR_FUNCS(p)                                                                                                    \
	p##retro_set_environment, p##retro_set_video_refresh, p##retro_set_audio_sample, p##retro_set_audio_sample_batch,  \
	    p##retro_set_input_poll, p##retro_set_input_state, p##retro_init, p##retro_deinit, p##retro_load_game,           \
	    p##retro_unload_game, p##retro_run, p##retro_get_system_av_info, p##retro_set_controller_port_device,             \
	    p##retro_get_memory_data, p##retro_get_memory_size, p##retro_serialize_size, p##retro_serialize, p##retro_unserialize

LR_DECLARE(pcsx_)
LR_DECLARE(nds_)
LR_DECLARE(dos_)
LR_DECLARE(n64_)
#if PLAYSHELL_3DS
LR_DECLARE(n3ds_)
#endif

typedef struct {
	void (*set_environment)(retro_environment_t);
	void (*set_video_refresh)(retro_video_refresh_t);
	void (*set_audio_sample)(retro_audio_sample_t);
	void (*set_audio_sample_batch)(retro_audio_sample_batch_t);
	void (*set_input_poll)(retro_input_poll_t);
	void (*set_input_state)(retro_input_state_t);
	void (*init)(void);
	void (*deinit)(void);
	bool (*load_game)(const struct retro_game_info*);
	void (*unload_game)(void);
	void (*run)(void);
	void (*get_system_av_info)(struct retro_system_av_info*);
	void (*set_controller_port_device)(unsigned, unsigned);
	void* (*get_memory_data)(unsigned);
	size_t (*get_memory_size)(unsigned);
	size_t (*serialize_size)(void);
	bool (*serialize)(void*, size_t);
	bool (*unserialize)(const void*, size_t);
	const char* tag;                         // log lines: "ps1: ..."
	const char* (*option)(const char* key); // the core's options: PlayShell's answers (NULL: the core's default)
	void (*before_load)(void);               // reads the switch files
	int out_w, out_h;                        // the picture sent: this size (scaled), 0: the core's own
	int memcard;                             // PlayShell keeps the core's save memory: saves/<game>.srm
	int load_data;                           // the core wants the game's bytes, not only its path (need_fullpath false)
} LrCore;

static int file_exists(const char* p) {
	return access(p, F_OK) == 0;
}

// PlayStation (PCSX-ReARMed)
static int g_ps1_hd = 1; // 2x ("Enhanced Resolution": 3D drawn at twice the PS1's resolution)

static const char* ps1_option(const char* key) {
	if (strcmp(key, "pcsx_rearmed_drc") == 0) { // the JIT (lightrec), unless asked otherwise
		return file_exists("/data/PlayShell/ps1_interpreter") ? "disabled" : "enabled";
	} else if (strcmp(key, "pcsx_rearmed_bios") == 0) {
		return "auto"; // a real BIOS in /data/PlayShell/bios if there is one, else PCSX's own
	} else if (strcmp(key, "pcsx_rearmed_memcard1") == 0) {
		return "libretro"; // card 1: PlayShell keeps it (card_load / card_save), one per game
	} else if (strcmp(key, "pcsx_rearmed_memcard2") == 0) {
		return "disabled";
	} else if (strcmp(key, "pcsx_rearmed_neon_enhancement_enable") == 0) {
		return g_ps1_hd ? "enabled" : "disabled";
	} else if (strcmp(key, "pcsx_rearmed_frameskip_type") == 0) {
		return "disabled";
	}
	return NULL;
}

static void ps1_before_load(void);
#if PLAYSHELL_3DS
// Nintendo 3DS (Azahar): drawn on the CPU (no GPU here), its JIT for the CPU and the shaders, the Original 3DS (less to
// emulate than the New 3DS), the screens above each other, fast sound
static const char* n3ds_option(const char* key) {
	if (strcmp(key, "citra_graphics_api") == 0) {
		return "Software";
	} else if (strcmp(key, "citra_use_cpu_jit") == 0 || strcmp(key, "citra_use_shader_jit") == 0) {
		return "enabled";
	} else if (strcmp(key, "citra_is_new_3ds") == 0) {
		return access("/data/PlayShell/3ds_new", F_OK) == 0 ? "New 3DS" : "Old 3DS";
	} else if (strcmp(key, "citra_layout_option") == 0) {
		return "default";
	} else if (strcmp(key, "citra_audio_emulation") == 0) {
		return "hle";
	} else if (strcmp(key, "citra_resolution_factor") == 0) {
		return "1x";
	}
	return NULL;
}
#endif

// Nintendo DS (melonDS): its JIT, the screens above each other, the touch screen by the right stick (R2 touches)
static const char* nds_option(const char* key) {
	if (strcmp(key, "melonds_jit_enable") == 0) {
		return "enabled";
	} else if (strcmp(key, "melonds_jit_fast_memory") == 0) {
		return "disabled"; // (its memory mirroring trick: not tried on the PS5 yet)
	} else if (strcmp(key, "melonds_opengl_renderer") == 0) {
		return "disabled";
	} else if (strcmp(key, "melonds_threaded_renderer") == 0) {
		return "enabled";
	} else if (strcmp(key, "melonds_touch_mode") == 0) {
		return "Joystick";
	} else if (strcmp(key, "melonds_screen_layout") == 0) {
		return "Top/Bottom";
	} else if (strcmp(key, "melonds_boot_directly") == 0) {
		return "enabled";
	} else if (strcmp(key, "melonds_console_mode") == 0) {
		return "DS";
	}
	return NULL;
}

// DOS (DOSBox Pure): its dynamic core (DOS code -> x86-64), sound at 48 kHz, its on-screen keyboard
static const char* dos_option(const char* key) {
	if (strcmp(key, "dosbox_pure_cpu_core") == 0) {
		return "dynamic";
	} else if (strcmp(key, "dosbox_pure_audiorate") == 0) {
		return "48000";
	} else if (strcmp(key, "dosbox_pure_on_screen_keyboard") == 0) {
		return "true";
	}
	return NULL;
}

// Nintendo 64 (Mupen64Plus-Next): its dynarec; drawn on the CPU by Angrylion with the low-level RSP it needs (cxd4);
// Angrylion's threads set (its "all threads" counts CPU cores the PC way: PPSSPP's count said 256 on the PS5)
static const char* n64_option(const char* key) {
	if (strcmp(key, "mupen64plus-rdp-plugin") == 0) {
		return "angrylion";
	} else if (strcmp(key, "mupen64plus-rsp-plugin") == 0) {
		return "cxd4";
	} else if (strcmp(key, "mupen64plus-cpucore") == 0) {
		return "dynamic_recompiler";
	} else if (strcmp(key, "mupen64plus-angrylion-multithread") == 0) {
		return "6";
	} else if (strcmp(key, "mupen64plus-angrylion-overscan") == 0) {
		return "enabled"; // the black borders the real N64 drew around its picture (a TV hid them): hidden
	}
	return NULL;
}

enum { LR_PS1 = 0, LR_NDS = 1, LR_DOS = 2, LR_N64 = 3, LR_3DS = 4, LR_CORES = PLAYSHELL_3DS ? 5 : 4 };
static const LrCore kCores[LR_CORES] = {
    {LR_FUNCS(pcsx_), "ps1", ps1_option, ps1_before_load, 640, 480, 1},
    {LR_FUNCS(nds_), "nds", nds_option, NULL, 0, 0, 0, 1},  // 256 x 384: both screens
    {LR_FUNCS(dos_), "dos", dos_option, NULL, 640, 480, 0}, // every DOS mode -> 4:3
    {LR_FUNCS(n64_), "n64", n64_option, NULL, 640, 480, 0, 1}, // 4:3
#if PLAYSHELL_3DS
    {LR_FUNCS(n3ds_), "3ds", n3ds_option, NULL, 0, 0, 0},  // 400 x 480: both screens
#endif
};

// ---- each console's picture edges ----
// /data/PlayShell/crop/<console>.txt (ps1, nds, dos, n64): "left right top bottom" - pixels of the core's own picture
// cut off before it is scaled (a game's black borders); read when a game of that console starts. Only that console.
static int g_crop[4];

static void read_crop(const char* tag) {
	char path[96];
	snprintf(path, sizeof(path), "/data/PlayShell/crop/%s.txt", tag);
	memset(g_crop, 0, sizeof(g_crop));
	FILE* f = fopen(path, "r");
	if (f != NULL) {
		if (fscanf(f, "%d %d %d %d", &g_crop[0], &g_crop[1], &g_crop[2], &g_crop[3]) != 4) {
			memset(g_crop, 0, sizeof(g_crop));
		}
		fclose(f);
	}
	for (int i = 0; i < 4; ++i) {
		g_crop[i] = g_crop[i] < 0 ? 0 : g_crop[i] > 200 ? 200 : g_crop[i];
	}
}

// ---- the frontend ----
static const LrCore* g_core = NULL; // the core started (init done), NULL: none
static int           g_core_id = -1;
static volatile int  g_paused = 1;
static volatile int  g_load   = 0; // a game asked for (g_path, g_load_core)
static volatile int  g_stop   = 0;
static int           g_load_core;
static char          g_path[512];
static int           g_loaded = 0;
static double        g_fps    = 59.94;
static double        g_rate   = 44100.0;
static enum retro_pixel_format g_fmt = RETRO_PIXEL_FORMAT_0RGB1555;
static volatile uint32_t g_buttons; // the DualSense (the home screen's button bits)
static volatile int8_t   g_lx, g_ly, g_rx, g_ry;
static char          g_card[600]; // memory card file
// save states: asked by the game loop (ps_lr_state), done here between frames; 1 save, 2 load
static volatile int  g_state_op = 0, g_state_res = 0;
static char          g_state_path[600];
static char          g_after_load[600]; // a game started with "Continue": this state, after its first frames
static int           g_since_load = 0;
static int           g_logs = 0;
static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;

static void ps1_before_load(void) {
	g_ps1_hd = !file_exists("/data/PlayShell/ps1_native"); // (read by the core at load: GET_VARIABLE)
}

static void lr_logf(const char* fmt, ...) {
	char    line[400];
	int     k = snprintf(line, sizeof(line), "%s: ", g_core != NULL ? g_core->tag : "cores");
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(line + k, sizeof(line) - (size_t)k, fmt, ap);
	va_end(ap);
	ps_log(line);
}

static void RETRO_CALLCONV core_log(enum retro_log_level level, const char* fmt, ...) {
	if (level < RETRO_LOG_INFO || g_logs >= 400) {
		return;
	}
	++g_logs;
	char    line[400];
	int     k = snprintf(line, sizeof(line), "%s core: ", g_core != NULL ? g_core->tag : "?");
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(line + k, sizeof(line) - (size_t)k, fmt, ap);
	va_end(ap);
	size_t n = strlen(line);
	while (n > 0 && (line[n - 1] == '\n' || line[n - 1] == '\r')) {
		line[--n] = 0;
	}
	ps_log(line);
}

static bool RETRO_CALLCONV environment(unsigned cmd, void* data) {
	switch (cmd) {
	case RETRO_ENVIRONMENT_GET_SYSTEM_DIRECTORY:
		*(const char**)data = "/data/PlayShell/bios";
		return true;
	case RETRO_ENVIRONMENT_GET_SAVE_DIRECTORY:
		*(const char**)data = "/data/PlayShell/saves";
		return true;
	case RETRO_ENVIRONMENT_SET_PIXEL_FORMAT:
		g_fmt = *(const enum retro_pixel_format*)data;
		return g_fmt == RETRO_PIXEL_FORMAT_RGB565 || g_fmt == RETRO_PIXEL_FORMAT_XRGB8888 || g_fmt == RETRO_PIXEL_FORMAT_0RGB1555;
	case RETRO_ENVIRONMENT_GET_LOG_INTERFACE:
		((struct retro_log_callback*)data)->log = core_log;
		return true;
	case RETRO_ENVIRONMENT_GET_CAN_DUPE:
		*(bool*)data = true;
		return true;
	case RETRO_ENVIRONMENT_GET_INPUT_BITMASKS:
		return true;
	case RETRO_ENVIRONMENT_GET_JIT_CAPABLE: // yes: runnable memory comes through the kernel
		*(bool*)data = true;
		return true;
	case RETRO_ENVIRONMENT_GET_USERNAME:
		*(const char**)data = "PlayShell";
		return true;
	case RETRO_ENVIRONMENT_GET_VARIABLE: {
		struct retro_variable* v = (struct retro_variable*)data;
		v->value                 = NULL;
		if (v->key == NULL || g_core == NULL || g_core->option == NULL) {
			return false;
		}
		v->value = g_core->option(v->key);
		return v->value != NULL;
	}
	case RETRO_ENVIRONMENT_GET_VARIABLE_UPDATE:
		*(bool*)data = false;
		return true;
	case RETRO_ENVIRONMENT_SET_GEOMETRY:
	case RETRO_ENVIRONMENT_SET_SYSTEM_AV_INFO:
		if (cmd == RETRO_ENVIRONMENT_SET_SYSTEM_AV_INFO) {
			const struct retro_system_av_info* av = (const struct retro_system_av_info*)data;
			if (av->timing.fps > 10.0) {
				g_fps = av->timing.fps;
			}
			if (av->timing.sample_rate > 8000.0) {
				g_rate = av->timing.sample_rate;
			}
		}
		return true;
	case RETRO_ENVIRONMENT_GET_AUDIO_VIDEO_ENABLE:
		*(int*)data = 3; // video + audio
		return true;
	default:
		return false; // (SET_HW_RENDER too: no GPU context - the cores draw in software)
	}
}

static uint16_t to565(const void* row, unsigned x) {
	if (g_fmt == RETRO_PIXEL_FORMAT_XRGB8888) {
		const uint32_t c = ((const uint32_t*)row)[x];
		return (uint16_t)(((c >> 8) & 0xF800) | ((c >> 5) & 0x07E0) | ((c >> 3) & 0x001F));
	}
	if (g_fmt == RETRO_PIXEL_FORMAT_RGB565) {
		return ((const uint16_t*)row)[x];
	}
	const uint16_t c = ((const uint16_t*)row)[x]; // 0RGB1555
	return (uint16_t)(((c & 0x7C00) << 1) | ((c & 0x03E0) << 1) | (c & 0x001F));
}

// The picture: RGB565 at the core's size, or scaled to the core's fixed size (PS1, DOS, N64: every mode -> 4:3).
// Runs on the picture thread (below), not the emulator's.
static void process_frame(const void* data, unsigned w, unsigned h, size_t pitch) {
	static uint16_t out[1024 * 544];
	if (data == NULL || w == 0 || h == 0 || g_core == NULL) {
		return;
	}
	unsigned ow = g_core->out_w, oh = g_core->out_h;
	if (g_core == &kCores[LR_PS1] && !g_ps1_hd) {
		ow = 320, oh = 240;
	}
	if (ow == 0) { // the core's own size (at most 1024 x 544: larger is scaled down)
		ow = w, oh = h;
		while (ow > 1024 || oh > 544) {
			ow /= 2, oh /= 2;
		}
	}
	// this console's crop (its own file; none: the whole picture)
	unsigned x0 = (unsigned)g_crop[0], y0 = (unsigned)g_crop[2];
	unsigned cw = w > (unsigned)(g_crop[0] + g_crop[1]) + 16 ? w - (unsigned)(g_crop[0] + g_crop[1]) : w;
	unsigned ch = h > (unsigned)(g_crop[2] + g_crop[3]) + 16 ? h - (unsigned)(g_crop[2] + g_crop[3]) : h;
	if (cw == w) {
		x0 = 0;
	}
	if (ch == h) {
		y0 = 0;
	}
	for (unsigned y = 0; y < oh; ++y) {
		const uint8_t* row = (const uint8_t*)data + (size_t)(y0 + y * ch / oh) * pitch;
		uint16_t*      o   = out + y * ow;
		for (unsigned x = 0; x < ow; ++x) {
			o[x] = to565(row, x0 + x * cw / ow);
		}
	}
	ps_send_picture(out, (int)ow, (int)oh);
}

// ---- the picture thread: the emulator hands over each frame (a copy) and goes on at once; the newest frame waiting
// replaces an older one not yet taken (the emulator never waits for xBRZ or the sending) ----
#define LR_FRAME_MAX (1024 * 1024 * 4) // the largest frame a core gives (bytes)
static uint8_t*        g_vf[2];          // [0] the frame waiting, [1] the one being worked on
static unsigned        g_vf_w, g_vf_h;   // the waiting frame's size (rows packed: pitch = w * bytes per pixel)
static size_t          g_vf_pitch;
static int             g_vf_full = 0;
static pthread_mutex_t g_vf_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t  g_vf_cond = PTHREAD_COND_INITIALIZER;

static void* picture_thread(void* arg) {
	(void)arg;
	for (;;) {
		pthread_mutex_lock(&g_vf_lock);
		while (!g_vf_full) {
			pthread_cond_wait(&g_vf_cond, &g_vf_lock);
		}
		uint8_t* t = g_vf[0]; // take it: the waiting buffer and the work buffer change places
		g_vf[0]    = g_vf[1];
		g_vf[1]    = t;
		const unsigned w = g_vf_w, h = g_vf_h;
		const size_t   pitch = g_vf_pitch;
		g_vf_full            = 0;
		pthread_mutex_unlock(&g_vf_lock);
		process_frame(g_vf[1], w, h, pitch);
	}
	return NULL;
}

static void RETRO_CALLCONV video_refresh(const void* data, unsigned w, unsigned h, size_t pitch) {
	static int started = 0;
	if (data == NULL || w == 0 || h == 0 || g_core == NULL) {
		return; // the same frame again: the home screen keeps showing it
	}
	const size_t bpp = g_fmt == RETRO_PIXEL_FORMAT_XRGB8888 ? 4 : 2, row = (size_t)w * bpp;
	if (row * h > LR_FRAME_MAX) {
		return;
	}
	if (!started) { // (first frame: the buffers and the thread)
		g_vf[0] = (uint8_t*)malloc(LR_FRAME_MAX);
		g_vf[1] = (uint8_t*)malloc(LR_FRAME_MAX);
		pthread_t t;
		if (g_vf[0] == NULL || g_vf[1] == NULL || pthread_create(&t, NULL, picture_thread, NULL) != 0) {
			process_frame(data, w, h, pitch); // (no thread: as before)
			return;
		}
		pthread_detach(t);
		started = 1;
	}
	pthread_mutex_lock(&g_vf_lock);
	for (unsigned y = 0; y < h; ++y) {
		memcpy(g_vf[0] + y * row, (const uint8_t*)data + y * pitch, row);
	}
	g_vf_w = w, g_vf_h = h, g_vf_pitch = row;
	g_vf_full = 1;
	pthread_cond_signal(&g_vf_cond);
	pthread_mutex_unlock(&g_vf_lock);
}

// The sound: the core's rate (44.1 kHz ...) stereo -> 48 kHz (linear), sent in pieces of <= 1024 frames.
static void lr_sound(const int16_t* in, size_t frames) {
	static int16_t out[1024 * 2];
	static double  pos  = 0.0; // where in the input the next output frame is (the fraction carries over)
	const double   step = g_rate / 48000.0;
	unsigned       n    = 0;
	while (pos < (double)frames) {
		const size_t   i  = (size_t)pos;
		const double   f  = pos - (double)i;
		const int16_t* p0 = in + i * 2;
		const int16_t* p1 = i + 1 < frames ? in + (i + 1) * 2 : p0;
		out[n * 2]        = (int16_t)(p0[0] + (p1[0] - p0[0]) * f);
		out[n * 2 + 1]    = (int16_t)(p0[1] + (p1[1] - p0[1]) * f);
		if (++n == 1024) {
			ps_send_sound(out, n);
			n = 0;
		}
		pos += step;
	}
	pos -= (double)frames;
	if (n > 0) {
		ps_send_sound(out, n);
	}
}

static size_t RETRO_CALLCONV audio_batch(const int16_t* data, size_t frames) {
	lr_sound(data, frames);
	return frames;
}

static void RETRO_CALLCONV audio_sample(int16_t l, int16_t r) {
	const int16_t s[2] = {l, r};
	lr_sound(s, 1);
}

static void RETRO_CALLCONV input_poll(void) {}

// The DualSense -> the RetroPad (by position: Cross = B, Circle = A, Square = Y, Triangle = X - what the cores
// expect). The home screen's bits: Left 1, Up 2, Right 4, Down 8, Square 0x10, Triangle 0x20, Circle 0x40, Cross 0x80,
// Options 0x100, Create 0x200, L1 0x400, R1 0x800, L2 0x1000, R2 0x2000, L3 0x4000, R3 0x8000.
static int16_t RETRO_CALLCONV input_state(unsigned port, unsigned device, unsigned index, unsigned id) {
	if (port != 0) {
		return 0;
	}
	const uint32_t b = g_buttons;
	if (device == RETRO_DEVICE_ANALOG) {
		const int8_t v = index == RETRO_DEVICE_INDEX_ANALOG_LEFT ? (id == RETRO_DEVICE_ID_ANALOG_X ? g_lx : g_ly)
		                                                         : (id == RETRO_DEVICE_ID_ANALOG_X ? g_rx : g_ry);
		return (int16_t)(v * 258);
	}
	if (device != RETRO_DEVICE_JOYPAD) {
		return 0;
	}
	static const struct {
		unsigned id;
		uint32_t bit;
	} kMap[] = {
	    {RETRO_DEVICE_ID_JOYPAD_LEFT, 0x1},     {RETRO_DEVICE_ID_JOYPAD_UP, 0x2},     {RETRO_DEVICE_ID_JOYPAD_RIGHT, 0x4},
	    {RETRO_DEVICE_ID_JOYPAD_DOWN, 0x8},     {RETRO_DEVICE_ID_JOYPAD_Y, 0x10},     {RETRO_DEVICE_ID_JOYPAD_X, 0x20},
	    {RETRO_DEVICE_ID_JOYPAD_A, 0x40},       {RETRO_DEVICE_ID_JOYPAD_B, 0x80},     {RETRO_DEVICE_ID_JOYPAD_START, 0x100},
	    {RETRO_DEVICE_ID_JOYPAD_SELECT, 0x200}, {RETRO_DEVICE_ID_JOYPAD_L, 0x400},    {RETRO_DEVICE_ID_JOYPAD_R, 0x800},
	    {RETRO_DEVICE_ID_JOYPAD_L2, 0x1000},    {RETRO_DEVICE_ID_JOYPAD_R2, 0x2000}, {RETRO_DEVICE_ID_JOYPAD_L3, 0x4000},
	    {RETRO_DEVICE_ID_JOYPAD_R3, 0x8000},
	};
	uint32_t bits = 0;
	for (size_t i = 0; i < sizeof(kMap) / sizeof(kMap[0]); ++i) {
		if (b & kMap[i].bit) {
			bits |= 1u << kMap[i].id;
		}
	}
	if (PLAYSHELL_3DS && g_core_id == LR_3DS) { // the 3DS: L3 touches the screen (R3 - Azahar's touch - holds PlayShell's menu)
		bits &= ~((1u << RETRO_DEVICE_ID_JOYPAD_R3) | (1u << RETRO_DEVICE_ID_JOYPAD_L3));
		if (b & 0x4000) {
			bits |= 1u << RETRO_DEVICE_ID_JOYPAD_R3;
		}
	}
	if (g_core == &kCores[LR_NDS]) { // the DS: R2 touches the screen (R3 - melonDS's touch - holds PlayShell's menu)
		bits &= ~((1u << RETRO_DEVICE_ID_JOYPAD_R3) | (1u << RETRO_DEVICE_ID_JOYPAD_R2));
		if (b & 0x2000) {
			bits |= 1u << RETRO_DEVICE_ID_JOYPAD_R3;
		}
	}
	if (g_core == &kCores[LR_PS1]) { // the left stick also moves the d-pad (PS1 games without analog support)
		if (g_lx < -64) bits |= 1u << RETRO_DEVICE_ID_JOYPAD_LEFT;
		if (g_lx > 64) bits |= 1u << RETRO_DEVICE_ID_JOYPAD_RIGHT;
		if (g_ly < -64) bits |= 1u << RETRO_DEVICE_ID_JOYPAD_UP;
		if (g_ly > 64) bits |= 1u << RETRO_DEVICE_ID_JOYPAD_DOWN;
	}
	if (id == RETRO_DEVICE_ID_JOYPAD_MASK) {
		return (int16_t)bits;
	}
	return id < 16 && (bits & (1u << id)) ? 1 : 0;
}

// ---- memory card (cores with memcard: PlayShell keeps their save memory) ----
// One card per game, shared by its discs: "Final Fantasy VII (USA) (Disc 2).cue" -> "Final Fantasy VII (USA).srm".
static void card_path(const char* game) {
	const char* base = strrchr(game, '/');
	base             = base != NULL ? base + 1 : game;
	char name[400];
	snprintf(name, sizeof(name), "%s", base);
	char* dot = strrchr(name, '.');
	if (dot != NULL) {
		*dot = 0;
	}
	char* disc = strcasestr(name, " (Disc ");
	if (disc != NULL) {
		char* end = strchr(disc + 1, ')');
		memmove(disc, end != NULL ? end + 1 : disc + strlen(disc), strlen(end != NULL ? end + 1 : "") + 1);
	}
	snprintf(g_card, sizeof(g_card), "/data/PlayShell/saves/%s.srm", name);
}

static void card_load(void) {
	void*  mem  = g_core->get_memory_data(RETRO_MEMORY_SAVE_RAM);
	size_t size = g_core->get_memory_size(RETRO_MEMORY_SAVE_RAM);
	FILE*  f    = mem != NULL && size > 0 ? fopen(g_card, "rb") : NULL;
	if (f != NULL) {
		const size_t got = fread(mem, 1, size, f);
		fclose(f);
		lr_logf("memory card loaded (%zu KB)", got / 1024);
	}
}

static void card_save(void) {
	if (!g_loaded || g_core == NULL || !g_core->memcard) {
		return;
	}
	void*  mem  = g_core->get_memory_data(RETRO_MEMORY_SAVE_RAM);
	size_t size = g_core->get_memory_size(RETRO_MEMORY_SAVE_RAM);
	if (mem == NULL || size == 0) {
		return;
	}
	char tmp[620];
	snprintf(tmp, sizeof(tmp), "%s.part", g_card);
	FILE* f = fopen(tmp, "wb");
	if (f != NULL && fwrite(mem, 1, size, f) == size) {
		fclose(f);
		rename(tmp, g_card);
	} else if (f != NULL) {
		fclose(f);
	}
}

static long long now_ns(void) {
	struct timespec t;
	clock_gettime(CLOCK_MONOTONIC, &t);
	return (long long)t.tv_sec * 1000000000LL + t.tv_nsec;
}

// ---- save states (on this thread, between frames) ----
static int state_save(const char* path) {
	const size_t size = g_loaded ? g_core->serialize_size() : 0;
	void*        buf  = size > 0 ? malloc(size) : NULL;
	int          ok   = 0;
	if (buf != NULL && g_core->serialize(buf, size)) {
		char tmp[620];
		snprintf(tmp, sizeof(tmp), "%s.part", path);
		FILE* f = fopen(tmp, "wb");
		if (f != NULL && fwrite(buf, 1, size, f) == size) {
			fclose(f);
			ok = rename(tmp, path) == 0;
		} else if (f != NULL) {
			fclose(f);
		}
	}
	free(buf);
	lr_logf("state %s: %s (%zu KB)", ok ? "saved" : "NOT saved", path, size / 1024);
	return ok;
}

static int state_load(const char* path) {
	FILE* f  = g_loaded ? fopen(path, "rb") : NULL;
	int   ok = 0;
	if (f != NULL) {
		fseek(f, 0, SEEK_END);
		const long size = ftell(f);
		fseek(f, 0, SEEK_SET);
		void* buf = size > 0 ? malloc((size_t)size) : NULL;
		if (buf != NULL && fread(buf, 1, (size_t)size, f) == (size_t)size) {
			ps_step("core: unserialize");
			ok = g_core->unserialize(buf, (size_t)size) ? 1 : 0;
		}
		free(buf);
		fclose(f);
	}
	lr_logf("state %s: %s", ok ? "loaded" : "NOT loaded", path);
	return ok;
}

static void unload(void) {
	if (g_loaded) {
		ps_step("core: memory card save");
		card_save();
		ps_step("core: unload_game");
		g_core->unload_game();
		g_loaded = 0;
		ps_step("core: unloaded");
	}
}

static void* lr_thread(void* arg) {
	(void)arg;
	long long next = now_ns(), last_save = now_ns(), t0 = now_ns(), busy = 0;
	int       frames = 0;
	for (;;) {
		if (g_load) {
			pthread_mutex_lock(&g_lock);
			char path[512];
			snprintf(path, sizeof(path), "%s", g_path);
			const int want = g_load_core;
			g_load         = 0;
			g_since_load   = 0;
			pthread_mutex_unlock(&g_lock);
			unload();
			if (g_core != NULL && g_core_id != want) { // another core: the old one ends
				ps_step("core: deinit");
				g_core->deinit();
				g_core = NULL, g_core_id = -1;
			}
			const LrCore* c = &kCores[want];
			if (c->before_load != NULL) {
				c->before_load();
			}
			read_crop(c->tag);
			if (g_crop[0] | g_crop[1] | g_crop[2] | g_crop[3]) {
				ps_log("cores: picture crop for this console (left right top bottom):");
				char line[96];
				snprintf(line, sizeof(line), "cores:   %d %d %d %d", g_crop[0], g_crop[1], g_crop[2], g_crop[3]);
				ps_log(line);
			}
			if (g_core == NULL) {
				g_core = c, g_core_id = want; // (set first: the core asks its options during init)
				g_fmt  = RETRO_PIXEL_FORMAT_0RGB1555;
				c->set_environment(environment);
				c->set_video_refresh(video_refresh);
				c->set_audio_sample(audio_sample);
				c->set_audio_sample_batch(audio_batch);
				c->set_input_poll(input_poll);
				c->set_input_state(input_state);
				ps_step("core: init");
				c->init();
			}
			struct retro_game_info info = {path, NULL, 0, NULL};
			static void*           data = NULL; // (kept until the next game: a core may keep using it)
			free(data);
			data = NULL;
			if (c->load_data) { // melonDS, Mupen64Plus: the game's bytes (a NULL there crashed the DS in load_game)
				FILE* f = fopen(path, "rb");
				if (f != NULL) {
					fseek(f, 0, SEEK_END);
					const long size = ftell(f);
					fseek(f, 0, SEEK_SET);
					data = size > 0 ? malloc((size_t)size) : NULL;
					if (data != NULL && fread(data, 1, (size_t)size, f) == (size_t)size) {
						info.data = data, info.size = (size_t)size;
					}
					fclose(f);
				}
				lr_logf("game read: %zu KB", info.size / 1024);
			}
			ps_step("core: load_game");
			if (c->load_game(&info)) {
				struct retro_system_av_info av;
				memset(&av, 0, sizeof(av));
				c->get_system_av_info(&av);
				g_fps  = av.timing.fps > 10.0 ? av.timing.fps : 59.94;
				g_rate = av.timing.sample_rate > 8000.0 ? av.timing.sample_rate : 44100.0;
				c->set_controller_port_device(0, RETRO_DEVICE_JOYPAD);
				if (c->memcard) {
					card_path(path);
					card_load();
				}
				g_loaded = 1;
				lr_logf("running %s (%.2f fps, sound %.0f Hz, %dx%d)", path, g_fps, g_rate, av.geometry.base_width, av.geometry.base_height);
			} else {
				lr_logf("could not load %s", path);
			}
			next = now_ns();
		}
		if (g_stop) {
			g_stop = 0;
			unload();
		}
		if (g_state_op != 0) { // asked by the game loop (it waits)
			pthread_mutex_lock(&g_lock);
			char path[600];
			snprintf(path, sizeof(path), "%s", g_state_path);
			pthread_mutex_unlock(&g_lock);
			g_state_res = g_state_op == 1 ? state_save(path) : state_load(path);
			g_state_op  = 0;
		}
		if (!g_loaded || g_paused) {
			usleep(10000);
			next = now_ns();
			t0 = next, busy = 0, frames = 0;
			continue;
		}
		const long long a = now_ns();
		ps_step("core: run");
		g_core->run();
		busy += now_ns() - a;
		if (++g_since_load == 3 && g_after_load[0] != 0) { // "Continue": the game's state, once it runs (some cores
			(void)state_load(g_after_load);                // cannot take one before their first frame)
			g_after_load[0] = 0;
		}
		if (++frames == 300) { // every ~5 s: speed and how much of each frame the emulator needed
			const long long t = now_ns();
			lr_logf("%.1f fps, emulator %.2f ms per frame", 300.0 * 1e9 / (double)(t - t0), busy / 300.0 / 1e6);
			t0 = t, busy = 0, frames = 0;
		}
		if (now_ns() - last_save > 60000000000LL) { // the memory card every minute while playing
			card_save();
			last_save = now_ns();
		}
		next += (long long)(1e9 / g_fps);
		const long long wait = next - now_ns();
		if (wait > 0) {
			struct timespec ts = {(time_t)(wait / 1000000000LL), (long)(wait % 1000000000LL)};
			nanosleep(&ts, NULL);
		} else if (wait < -100000000LL) {
			next = now_ns(); // fell far behind: no catching up
		}
	}
	return NULL;
}

// ---- from the game loop (playshell_emu.c) ----
// core: 0 PlayStation, 1 Nintendo DS, 2 DOS, 3 Nintendo 64
int ps_lr_load(int core, const char* path, const char* state) {
	static int thread = 0;
	if (core < 0 || core >= LR_CORES) {
		return 0;
	}
	if (!thread) {
		pthread_t t;
		pthread_attr_t attr; // 8 MB of stack: the cores need more than a thread's default
		pthread_attr_init(&attr);
		pthread_attr_setstacksize(&attr, 8u << 20);
		const int made = pthread_create(&t, &attr, lr_thread, NULL);
		pthread_attr_destroy(&attr);
		if (made != 0) {
			return 0;
		}
		pthread_detach(t);
		thread = 1;
	}
	pthread_mutex_lock(&g_lock);
	snprintf(g_path, sizeof(g_path), "%s", path);
	snprintf(g_after_load, sizeof(g_after_load), "%s", state != NULL ? state : "");
	g_load_core = core;
	g_load      = 1;
	pthread_mutex_unlock(&g_lock);
	g_paused = 0;
	return 1;
}

void ps_lr_pause(int paused) {
	g_paused = paused;
	if (paused) {
		card_save(); // (the emulator thread may be mid-frame: the card is a plain memory copy - fine)
	}
}

void ps_lr_stop(void) {
	g_stop   = 1;
	g_paused = 1;
}

// A save state (op 1) or load (op 2) of the game running, done by the core's thread between frames; waits for it
// (up to 20 s). 1: done.
int ps_lr_state(int op, const char* path) {
	if (g_core == NULL) {
		return 0;
	}
	pthread_mutex_lock(&g_lock);
	snprintf(g_state_path, sizeof(g_state_path), "%s", path);
	pthread_mutex_unlock(&g_lock);
	g_state_res = 0;
	g_state_op  = op;
	for (int i = 0; i < 4000 && g_state_op != 0; ++i) {
		usleep(5000);
	}
	return g_state_op == 0 && g_state_res;
}

void ps_lr_pad(uint32_t buttons, int8_t lx, int8_t ly, int8_t rx, int8_t ry) {
	g_buttons = buttons;
	g_lx = lx, g_ly = ly, g_rx = rx, g_ry = ry;
}
