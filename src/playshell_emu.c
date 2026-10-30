// PlayShell - games on the PS5 home screen: the games run here, in the payload, and every frame goes to the home
// screen module (UDP 127.0.0.1:9053), which draws it. GPL-3.0 (same author as LiveShell).
// The emulator: Walnut-CGB (MIT, walnut_cgb.h: Peanut-GB with Game Boy Color). The games: the user's own, in
// /data/PlayShell/roms, roms/gb and roms/gbc - .gb Game Boy, .gbc Game Boy Color. Listed by name: the file name
// without its extension and tags like "(World)" / "[!]".
// Battery saves: <rom>.sav next to the ROM.
// Running here and not inside the home screen: if a game ever crashes the emulator, the home screen goes on.
#define ENABLE_SOUND 1
#define ENABLE_LCD 1
#include <stdint.h>
uint8_t audio_read(const uint16_t addr);                  // the sound chip: minigb_apu (below)
void    audio_write(const uint16_t addr, const uint8_t val);
#include "walnut_cgb.h"
#include "minigb_apu.h" // AUDIO_SAMPLE_RATE 48000, S16 (CMakeLists)

#include <arpa/inet.h>
#include <ctype.h>
#include <dirent.h>
#include <netinet/in.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#define RETRO_DIR "/data/PlayShell/roms"
#define RETRO_PORT 9053
#define MAX_GAMES 200

typedef struct {
	char path[400]; // the ROM
	char name[96];  // shown: the file name without its extension and "(World)" / "[!]" tags
	int  console;   // 0 Game Boy (.gb), 1 Game Boy Color (.gbc); Mesen 2: 2 NES (.nes), 3 Game Gear (.gg);
	                // PCSX-ReARMed: 4 PlayStation (.cue/.bin, .chd, .pbp, .iso, .m3u); Mesen 2: 5 SNES (.sfc, .smc),
	                // 6 Game Boy Advance (.gba); (7: PSP, removed); melonDS: 8 Nintendo DS (.nds); DOSBox Pure: 9 DOS
	                // (everything in roms/dos); Mupen64Plus-Next: 10 Nintendo 64; Mesen 2: 11 PC Engine (.pce),
	                // 12 WonderSwan (.ws, .wsc); Azahar: 13 Nintendo 3DS (decrypted .3ds, .cci, .cxi, .3dsx)
	char inner[160]; // in a .zip / .7z: the game file inside ("": a plain file)
	int  has_auto;   // a "Continue" state exists (states/<file>.auto)
} Game;

static void (*g_log)(const char* line);
static void (*g_notify)(const char* line);
static struct gb_s g_gb;
static uint8_t*    g_rom;
static size_t      g_rom_size;
static uint8_t     g_cart_ram[0x20000];
static uint16_t    g_frame[144][160]; // RGB565: Game Boy Color colours, or the Game Boy's greens
static volatile int g_running_retro;
static Game        g_games[MAX_GAMES];
static volatile int g_ngames;
static int         g_cur = -1; // the game loaded (index), -1: none
static pthread_mutex_t g_games_lock = PTHREAD_MUTEX_INITIALIZER;

// Mesen 2 (playshell_mesen.cpp): NES games run in its own thread
void ps_step(const char* step); // playshell_daemon.cpp: the crash recorder's step
int  ps_mesen_load(const char* path);
void ps_mesen_pause(int paused);
void ps_mesen_pad(uint8_t joypad);
void ps_mesen_stop(void);
void ps_mesen_buttons(uint32_t buttons, int8_t lx, int8_t ly);
int  ps_mesen_state(int op, const char* path);
int      ps_archive_rom(const char* path, char* inner, size_t cap);              // playshell_mesen.cpp
uint8_t* ps_archive_read(const char* path, const char* inner, size_t* size); // (malloc'd)
// PCSX-ReARMed (playshell_ps1.c): PlayStation games in its own thread
// libretro cores (playshell_lr.c): 0 PlayStation, 1 Nintendo DS, 2 DOS, 3 Nintendo 64 - in their own thread
int  ps_lr_load(int core, const char* path, const char* state);
int  ps_lr_state(int op, const char* path);
void ps_lr_pause(int paused);
void ps_lr_stop(void);
void ps_lr_pad(uint32_t buttons, int8_t lx, int8_t ly, int8_t rx, int8_t ry);
static int lr_core(int console) { // the libretro core of a console, -1: not one
	return console == 4 ? 0 : console == 8 ? 1 : console == 9 ? 2 : console == 10 ? 3 : console == 13 && PLAYSHELL_3DS ? 4 : -1;
}
static int g_frame_sock = -1; // to the home screen (frames, list)
static struct sockaddr_in g_frame_to;
static uint32_t g_pic_n;      // frame number

// ---- sound: the Game Boy's sound chip -> the home screen module (UDP 9055), which plays it ----
// (played from here, the payload - a background program - the PS5 took every grain and stayed silent)
static struct minigb_apu_ctx g_apu;
static int                   g_snd_sock = -1;
static struct sockaddr_in    g_snd_to;

uint8_t audio_read(const uint16_t addr) {
	return minigb_apu_audio_read(&g_apu, addr);
}

void audio_write(const uint16_t addr, const uint8_t val) {
	minigb_apu_audio_write(&g_apu, addr, val);
}

// Stereo S16 frames (<= 1024) -> one packet "A" + the samples (the Game Boy's and Mesen's sound).
void ps_send_sound(const int16_t* stereo, unsigned frames) {
	static uint8_t pkt[1 + 1024 * 4];
	if (frames > 1024) {
		frames = 1024;
	}
	if (g_snd_sock < 0) {
		g_snd_sock = socket(AF_INET, SOCK_DGRAM, 0);
		memset(&g_snd_to, 0, sizeof(g_snd_to));
		g_snd_to.sin_family      = AF_INET;
		g_snd_to.sin_port        = htons(9055);
		g_snd_to.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
	}
	pkt[0] = 'A';
	memcpy(pkt + 1, stereo, (size_t)frames * 4);
	if (g_snd_sock >= 0) {
		(void)sendto(g_snd_sock, pkt, 1 + (size_t)frames * 4, 0, (const struct sockaddr*)&g_snd_to, sizeof(g_snd_to));
	}
}

// One emulated Game Boy frame of sound (AUDIO_SAMPLES stereo frames).
static void sound_frame(void) {
	static audio_sample_t buf[AUDIO_SAMPLES_TOTAL];
	minigb_apu_audio_callback(&g_apu, buf);
	ps_send_sound((const int16_t*)buf, AUDIO_SAMPLES);
}

static void rlog(const char* fmt, ...) {
	char    line[400];
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(line, sizeof(line), fmt, ap);
	va_end(ap);
	if (g_log != NULL) {
		g_log(line);
	}
}

static uint8_t rom_read(struct gb_s* gb, const uint_fast32_t addr) {
	(void)gb;
	return addr < g_rom_size ? g_rom[addr] : 0xFF;
}

static uint16_t rom_read16(struct gb_s* gb, const uint_fast32_t addr) { // little endian, as the Game Boy reads it
	return (uint16_t)(rom_read(gb, addr) | (rom_read(gb, addr + 1) << 8));
}

static uint32_t rom_read32(struct gb_s* gb, const uint_fast32_t addr) {
	return (uint32_t)rom_read16(gb, addr) | ((uint32_t)rom_read16(gb, addr + 2) << 16);
}

static uint8_t cart_ram_read(struct gb_s* gb, const uint_fast32_t addr) {
	(void)gb;
	return addr < sizeof(g_cart_ram) ? g_cart_ram[addr] : 0xFF;
}

static void cart_ram_write(struct gb_s* gb, const uint_fast32_t addr, const uint8_t val) {
	(void)gb;
	if (addr < sizeof(g_cart_ram)) {
		g_cart_ram[addr] = val;
	}
}

static void gb_error_cb(struct gb_s* gb, const enum gb_error_e err, const uint16_t addr) {
	(void)gb;
	rlog("games: emulator error %d at %04x", (int)err, addr);
}

// A Game Boy Color game: each pixel an index into its palette (RGB565); a Game Boy game: shades 0..3, shown
// in the classic green.
static void draw_line(struct gb_s* gb, const uint8_t* pixels, const uint_fast8_t line) {
	static const uint16_t green[4] = {0x9DE1, 0x8D61, 0x3186, 0x09C1}; // (155,188,15) (139,172,15) (48,98,48) (15,56,15)
	if (line >= 144) {
		return;
	}
	if (gb->cgb.cgbMode) {
		for (int x = 0; x < 160; ++x) {
			g_frame[line][x] = gb->cgb.fixPalette[pixels[x] & 0x3F];
		}
	} else {
		for (int x = 0; x < 160; ++x) {
			g_frame[line][x] = green[pixels[x] & 3];
		}
	}
}

static int exists(const char* p) {
	struct stat st;
	return stat(p, &st) == 0 && st.st_size > 0;
}

static uint8_t* read_all(const char* path, size_t* size, size_t max) {
	FILE* f = fopen(path, "rb");
	if (f == NULL) {
		return NULL;
	}
	fseek(f, 0, SEEK_END);
	const long n = ftell(f);
	fseek(f, 0, SEEK_SET);
	uint8_t* b = n > 0 && (size_t)n <= max ? (uint8_t*)malloc((size_t)n + 1) : NULL;
	if (b != NULL && fread(b, 1, (size_t)n, f) != (size_t)n) {
		free(b);
		b = NULL;
	}
	fclose(f);
	if (b != NULL) {
		b[n] = 0;
		*size = (size_t)n;
	}
	return b;
}

// ---- the games ----

// A game file's console by its extension, -1: not a game.
static int console_of(const char* file) {
	static const struct {
		const char* ext;
		int         console;
	} kExt[] = {{".gb", 0},  {".gbc", 1}, {".nes", 2}, {".gg", 3},  {".cue", 4},
	            {".bin", 4}, {".chd", 4}, {".pbp", 4}, {".iso", 4}, {".m3u", 4}, {".sfc", 5}, {".smc", 5}, {".gba", 6},
	            {".nds", 8}, {".z64", 10}, {".n64", 10}, {".v64", 10}, {".pce", 11}, {".ws", 12}, {".wsc", 12},
#if PLAYSHELL_3DS
	            {".3ds", 13}, {".cci", 13}, {".cxi", 13}, {".3dsx", 13},
#endif
	};
	const char* dot = strrchr(file, '.');
	for (size_t i = 0; dot != NULL && i < sizeof(kExt) / sizeof(kExt[0]); ++i) {
		if (strcasecmp(dot, kExt[i].ext) == 0) {
			return kExt[i].console;
		}
	}
	return -1;
}

static int is_archive(const char* file) {
	const char* dot = strrchr(file, '.');
	return dot != NULL && (strcasecmp(dot, ".zip") == 0 || strcasecmp(dot, ".7z") == 0);
}

// Shown: the file name without its extension and tags like "(World)" / "[!]".
static void name_from_file(Game* g, const char* file) {
	size_t      k   = 0;
	const char* dot = strrchr(file, '.');
	for (const char* c = file; *c && c != dot && k + 1 < sizeof(g->name) && *c != '(' && *c != '['; ++c) {
		g->name[k++] = *c == '_' ? ' ' : *c;
	}
	while (k > 0 && (g->name[k - 1] == ' ' || g->name[k - 1] == '.')) {
		--k;
	}
	g->name[k] = 0;
	// discs of one game keep their number: "Final Fantasy VII (USA) (Disc 2).cue" -> "Final Fantasy VII - Disc 2"
	const char* disc = strcasestr(file, "(Disc ");
	if (k > 0 && disc != NULL && atoi(disc + 6) > 0) {
		snprintf(g->name + k, sizeof(g->name) - k, " - Disc %d", atoi(disc + 6));
		return;
	}
	if (k == 0) { // e.g. "[BIOS] Sonic.sms": the whole name without the extension
		snprintf(g->name, sizeof(g->name), "%.*s", (int)(dot != NULL ? dot - file : (long)strlen(file)), file);
	}
}

// The files a folder's .cue sheets use ("FILE "Game (Track 2).bin" BINARY"): hidden from the list - the .cue is the
// game (a game with several tracks showed each .bin, all with the same name).
#define MAX_USED 512
static char g_used[MAX_USED][128];
static int  g_nused;

static void cue_files(const char* dir) {
	g_nused = 0;
	DIR* d  = opendir(dir);
	if (d == NULL) {
		return;
	}
	struct dirent* e;
	while ((e = readdir(d)) != NULL) {
		const char* dot = strrchr(e->d_name, '.');
		if (dot == NULL || strcasecmp(dot, ".cue") != 0) {
			continue;
		}
		char path[600];
		snprintf(path, sizeof(path), "%s/%s", dir, e->d_name);
		FILE* f = fopen(path, "r");
		char  line[400];
		while (f != NULL && fgets(line, sizeof(line), f) != NULL && g_nused < MAX_USED) {
			const char* at = line;
			while (*at == ' ' || *at == '\t') {
				++at;
			}
			if (strncasecmp(at, "FILE", 4) != 0) {
				continue;
			}
			const char* q1 = strchr(at, '"');
			const char* q2 = q1 != NULL ? strchr(q1 + 1, '"') : NULL;
			if (q2 != NULL && q2 - q1 - 1 < (long)sizeof(g_used[0])) {
				snprintf(g_used[g_nused++], sizeof(g_used[0]), "%.*s", (int)(q2 - q1 - 1), q1 + 1);
			}
		}
		if (f != NULL) {
			fclose(f);
		}
	}
	closedir(d);
}

static int used_by_cue(const char* name) {
	for (int i = 0; i < g_nused; ++i) {
		if (strcasecmp(g_used[i], name) == 0) {
			return 1;
		}
	}
	return 0;
}

static void add_games(const char* dir) {
	const char* tail = strrchr(dir, '/');
	const int   dos  = tail != NULL && strcmp(tail, "/dos") == 0; // roms/dos: every game file (a zip is a DOS game)
	DIR* d = opendir(dir);
	if (d == NULL) {
		return;
	}
	cue_files(dir);
	struct dirent* e;
	while ((e = readdir(d)) != NULL && g_ngames < MAX_GAMES) {
		if (e->d_name[0] == '.') {
			continue;
		}
		Game* g = &g_games[g_ngames];
		memset(g, 0, sizeof(*g));
		snprintf(g->path, sizeof(g->path), "%s/%s", dir, e->d_name);
		g->console = console_of(e->d_name);
		const char* ext = strrchr(e->d_name, '.');
		if (dos) { // a DOS game: a .zip / .7z / .dosz of its files, a program, a CD image
			static const char* const kDos[] = {".zip", ".7z", ".dosz", ".exe", ".com", ".bat", ".iso", ".cue", ".img", ".chd"};
			g->console = -1;
			for (size_t k = 0; ext != NULL && k < sizeof(kDos) / sizeof(kDos[0]); ++k) {
				if (strcasecmp(ext, kDos[k]) == 0) {
					g->console = 9;
				}
			}
			if (g->console < 0) {
				continue;
			}
			name_from_file(g, e->d_name);
			++g_ngames;
			continue;
		}
		if (g->console < 0 && is_archive(e->d_name) && ps_archive_rom(g->path, g->inner, sizeof(g->inner))) {
			g->console = console_of(g->inner); // the game inside says which console
		}
		if (g->console < 0) {
			continue;
		}
		const char* dot = strrchr(e->d_name, '.');
		if (g->console == 4 && used_by_cue(e->d_name)) { // a .cue's track: the .cue is the game
			continue;
		}
		if (g->console == 4 && strcasecmp(dot, ".bin") == 0) { // a .bin with a .cue of its name: the .cue is the game
			char cue[420];
			snprintf(cue, sizeof(cue), "%.*s.cue", (int)(strlen(g->path) - 4), g->path);
			if (access(cue, F_OK) == 0) {
				continue;
			}
		}
		name_from_file(g, e->d_name);
		++g_ngames;
	}
	closedir(d);
}

static int by_name(const void* a, const void* b) {
	return strcasecmp(((const Game*)a)->name, ((const Game*)b)->name);
}

static void mark_autos(void); // (below, with the save states)

static void scan_games(void) {
	pthread_mutex_lock(&g_games_lock);
	char cur_path[400] = "";
	if (g_cur >= 0) {
		snprintf(cur_path, sizeof(cur_path), "%s", g_games[g_cur].path);
	}
	g_ngames = 0;
	add_games(RETRO_DIR);
	add_games(RETRO_DIR "/gb");
	add_games(RETRO_DIR "/gbc");
	add_games(RETRO_DIR "/nes");
	add_games(RETRO_DIR "/gg");
	add_games(RETRO_DIR "/ps1");
	add_games(RETRO_DIR "/snes");
	add_games(RETRO_DIR "/gba");
	add_games(RETRO_DIR "/nds");
	add_games(RETRO_DIR "/dos");
	add_games(RETRO_DIR "/n64");
	add_games(RETRO_DIR "/pce");
	add_games(RETRO_DIR "/ws");
#if PLAYSHELL_3DS
	add_games(RETRO_DIR "/3ds");
#endif
	qsort(g_games, (size_t)g_ngames, sizeof(Game), by_name);
	if (cur_path[0] != 0) { // the loaded game keeps its place
		g_cur = -1;
		for (int i = 0; i < g_ngames; ++i) {
			if (strcmp(g_games[i].path, cur_path) == 0) {
				g_cur = i;
			}
		}
	}
	mark_autos();
	pthread_mutex_unlock(&g_games_lock);
}

// ---- save states: /data/PlayShell/states/<game file>.auto (Continue) and .state (the menu's Square / Triangle) ----
#define STATE_DIR "/data/PlayShell/states"
#define PLAYSHELL_SAVE_STATES 0 // (the home screen module's switch too: not in 0.1)

static void state_path(int i, const char* kind, char* out, size_t cap) {
	const char* base = strrchr(g_games[i].path, '/');
	snprintf(out, cap, STATE_DIR "/%s.%s", base != NULL ? base + 1 : g_games[i].path, kind);
}

static void mark_autos(void) { // (after a scan) which games have a Continue state
	for (int i = 0; i < g_ngames; ++i) {
		char p[600];
		state_path(i, "auto", p, sizeof(p));
		g_games[i].has_auto = access(p, F_OK) == 0;
	}
}

// The Game Boy's state: Walnut's whole machine + the cartridge RAM + the sound chip; its function pointers are this
// run's (kept from the game as loaded now).
#define GB_STATE_MAGIC 0x50534753u // "PSGS"
static int gb_state(int op, const char* path) {
	if (g_rom == NULL) {
		return 0;
	}
	const uint32_t head[3] = {GB_STATE_MAGIC, (uint32_t)sizeof(g_gb), (uint32_t)g_rom_size};
	int            ok      = 0;
	if (op == 1) {
		char tmp[620];
		snprintf(tmp, sizeof(tmp), "%s.part", path);
		FILE* f = fopen(tmp, "wb");
		if (f != NULL) {
			ok = fwrite(head, sizeof(head), 1, f) == 1 && fwrite(&g_gb, sizeof(g_gb), 1, f) == 1 &&
			     fwrite(g_cart_ram, sizeof(g_cart_ram), 1, f) == 1 && fwrite(&g_apu, sizeof(g_apu), 1, f) == 1;
			fclose(f);
			ok = ok && rename(tmp, path) == 0;
		}
	} else {
		FILE* f = fopen(path, "rb");
		if (f != NULL) {
			static struct gb_s gb;
			static uint8_t     ram[sizeof(g_cart_ram)];
			static struct minigb_apu_ctx apu;
			uint32_t           h[3];
			ok = fread(h, sizeof(h), 1, f) == 1 && memcmp(h, head, sizeof(h)) == 0 && fread(&gb, sizeof(gb), 1, f) == 1 &&
			     fread(ram, sizeof(ram), 1, f) == 1 && fread(&apu, sizeof(apu), 1, f) == 1;
			fclose(f);
			if (ok) {
				const struct gb_s live = g_gb;
				g_gb                   = gb;
				g_gb.gb_rom_read       = live.gb_rom_read;
				g_gb.gb_rom_read_16bit = live.gb_rom_read_16bit;
				g_gb.gb_rom_read_32bit = live.gb_rom_read_32bit;
				g_gb.gb_cart_ram_read  = live.gb_cart_ram_read;
				g_gb.gb_cart_ram_write = live.gb_cart_ram_write;
				g_gb.gb_error          = live.gb_error;
				g_gb.gb_serial_tx      = live.gb_serial_tx;
				g_gb.gb_serial_rx      = live.gb_serial_rx;
				g_gb.gb_bootrom_read   = live.gb_bootrom_read;
				g_gb.display.lcd_draw_line = live.display.lcd_draw_line;
				g_gb.direct.priv       = live.direct.priv;
				memcpy(g_cart_ram, ram, sizeof(ram));
				g_apu = apu;
			}
		}
	}
	rlog("games: Game Boy state %s: %s", ok ? (op == 1 ? "saved" : "loaded") : "FAILED", path);
	return ok;
}

// Save (op 1) or load (op 2) the running game's state, whichever emulator runs it. 1: done.
static int game_state(int op, const char* kind) {
	if (g_cur < 0) {
		return 0;
	}
	char p[600];
	state_path(g_cur, kind, p, sizeof(p));
	mkdir(STATE_DIR, 0777);
	const int con = g_games[g_cur].console;
	const int ok  = lr_core(con) >= 0 ? ps_lr_state(op, p) : con >= 2 ? ps_mesen_state(op, p) : gb_state(op, p);
	if (ok && op == 1 && strcmp(kind, "auto") == 0) {
		g_games[g_cur].has_auto = 1;
	}
	return ok;
}

// ---- one game ----

static void save_path(char* out, size_t cap) {
	snprintf(out, cap, "%s.sav", g_games[g_cur].path);
}

static void save_game(void) {
	size_t n = 0;
	if (g_cur < 0 || g_games[g_cur].console >= 2 || g_rom == NULL || gb_get_save_size_s(&g_gb, &n) != 0 || n == 0 || n > sizeof(g_cart_ram)) {
		return;
	}
	char p[512], tmp[520];
	save_path(p, sizeof(p));
	snprintf(tmp, sizeof(tmp), "%s.part", p);
	FILE* f = fopen(tmp, "wb");
	if (f != NULL && fwrite(g_cart_ram, 1, n, f) == n) {
		fclose(f);
		rename(tmp, p);
	} else if (f != NULL) {
		fclose(f);
	}
}

static int load_game(int i, int resume) { // resume: from its Continue state
	if (i < 0 || i >= g_ngames) {
		return 0;
	}
	char cont[600] = "";
	if (resume) {
		state_path(i, "auto", cont, sizeof(cont));
	}
	ps_step("load: Game Boy battery save");
	save_game();
	ps_step("load: Mesen stop");
	ps_mesen_stop(); // (a NES game running: its battery is saved)
	if (g_cur >= 0 && lr_core(g_games[g_cur].console) >= 0 && lr_core(g_games[i].console) < 0) {
		ps_step("load: libretro core stop");
		ps_lr_stop(); // (a PS1 memory card is saved; switching between libretro cores: the loader does it)
	}
	free(g_rom);
	g_rom      = NULL;
	g_rom_size = 0;
	g_cur      = -1;
	if (lr_core(g_games[i].console) >= 0) { // PlayStation, Nintendo DS, DOS, Nintendo 64
		ps_lr_load(lr_core(g_games[i].console), g_games[i].path, cont); // (the core loads the state once it runs)
		g_cur = i;
		return 1;
	}
	if (g_games[i].console >= 2) { // Mesen 2's consoles
		char what[600]; // Mesen: "<archive>\x01<game inside>" for a game in a .zip / .7z
		if (g_games[i].inner[0] != 0) {
			snprintf(what, sizeof(what), "%s\x01%s", g_games[i].path, g_games[i].inner);
		} else {
			snprintf(what, sizeof(what), "%s", g_games[i].path);
		}
		ps_step("load: Mesen load");
		if (!ps_mesen_load(what)) {
			if (g_notify != NULL) {
				char line[160];
				snprintf(line, sizeof(line), "PlayShell: %s cannot start", g_games[i].name);
				g_notify(line);
			}
			return 0;
		}
		g_cur = i;
		if (cont[0] != 0) {
			(void)ps_mesen_state(2, cont);
		}
		return 1;
	}
	size_t n   = 0;
	g_rom      = g_games[i].inner[0] != 0 ? ps_archive_read(g_games[i].path, g_games[i].inner, &n) // in a .zip / .7z
	                                      : read_all(g_games[i].path, &n, 8 * 1024 * 1024);
	if (g_rom == NULL || n <= 0x150) {
		return 0;
	}
	g_rom_size = n;
	memset(g_cart_ram, 0xFF, sizeof(g_cart_ram));
	// Game Boy games through the Game Boy Color path (no flicker at the top); /data/PlayShell/roms/.gbclassic: the
	// classic path
	ls_dmg_compat_wanted = access(RETRO_DIR "/.gbclassic", F_OK) != 0;
	const enum gb_init_error_e e = gb_init(&g_gb, rom_read, rom_read16, rom_read32, cart_ram_read, cart_ram_write, gb_error_cb, NULL);
	if (e != GB_INIT_NO_ERROR) {
		rlog("games: %s cannot start (error %d)", g_games[i].name, (int)e);
		if (g_notify != NULL) {
			char line[160];
			snprintf(line, sizeof(line), "PlayShell: %s cannot start (this cartridge type is not supported)", g_games[i].name);
			g_notify(line);
		}
		return 0;
	}
	gb_init_lcd(&g_gb, draw_line);
	rlog("games: %s: %s", g_games[i].name, ls_dmg_compat ? "Game Boy game in Game Boy Color mode" : g_games[i].console ? "Game Boy Color" : "Game Boy (classic)");
	minigb_apu_audio_init(&g_apu);
	g_cur = i;
	char p[512];
	save_path(p, sizeof(p));
	size_t sn = 0;
	FILE*  sf = gb_get_save_size_s(&g_gb, &sn) == 0 && sn > 0 ? fopen(p, "rb") : NULL;
	if (sf != NULL) {
		(void)fread(g_cart_ram, 1, sn < sizeof(g_cart_ram) ? sn : sizeof(g_cart_ram), sf);
		fclose(sf);
	}
	rlog("games: %s (%zu KB) running%s", g_games[i].name, g_rom_size / 1024, sf != NULL ? ", save loaded" : "");
	if (cont[0] != 0) {
		(void)gb_state(2, cont);
	}
	return 1;
}

static long long now_ns(void) {
	struct timespec t;
	clock_gettime(CLOCK_MONOTONIC, &t);
	return (long long)t.tv_sec * 1000000000LL + t.tv_nsec;
}

// One frame to the home screen, any size (each console its own): packets "V", frame number (4), width (2),
// height (2), first line (2), lines (1), then the lines in RGB565 - at most 7680 bytes of pixels per packet, spread
// over 4 lanes (UDP 9060-9063: four receive buffers - a PS1 frame at 2x is ~600 KB, one buffer holds 512 KB).
void ps_send_picture(const uint16_t* px, int w, int h) {
	static uint8_t pkt[12 + 7680];
	if (g_frame_sock < 0 || w <= 0 || h <= 0 || w > 1024 || h > 544) {
		return;
	}
	static struct sockaddr_in lane[4]; // the 4 frame lanes (UDP 9060-9063): each part on the next lane
	if (lane[0].sin_port == 0) {
		for (int i = 0; i < 4; ++i) {
			lane[i]                 = g_frame_to;
			lane[i].sin_port        = htons((uint16_t)(9060 + i));
		}
	}
	int part = 0;
	const uint32_t n     = ++g_pic_n;
	const int      lines = 7680 / (w * 2);
	const uint16_t w16 = (uint16_t)w, h16 = (uint16_t)h;
	for (int y = 0; y < h; y += lines) {
		const int      k   = y + lines <= h ? lines : h - y;
		const uint16_t y16 = (uint16_t)y;
		pkt[0] = 'V';
		memcpy(pkt + 1, &n, 4);
		memcpy(pkt + 5, &w16, 2);
		memcpy(pkt + 7, &h16, 2);
		memcpy(pkt + 9, &y16, 2);
		pkt[11] = (uint8_t)k;
		memcpy(pkt + 12, px + (size_t)y * w, (size_t)k * w * 2);
		(void)sendto(g_frame_sock, pkt, 12 + (size_t)k * w * 2, 0, (const struct sockaddr*)&lane[part++ % 4], sizeof(lane[0]));
	}
}

void ps_log(const char* line) {
	if (g_log != NULL) {
		g_log(line);
	}
}

// The game list for the menu: "L<total>\t<loaded>\t<first>\n" + per game "<id>\t<console>\t<name>\n",
// at most 40 games per packet.
static void send_list(int s, const struct sockaddr_in* to) {
	static char pkt[8192];
	pthread_mutex_lock(&g_games_lock);
	for (int first = 0; first < g_ngames || first == 0; first += 40) {
		int k = snprintf(pkt, sizeof(pkt), "L%d\t%d\t%d\n", g_ngames, g_cur, first);
		for (int i = first; i < g_ngames && i < first + 40 && k < (int)sizeof(pkt) - 400; ++i) {
			k += snprintf(pkt + k, sizeof(pkt) - (size_t)k, "%d\t%d\t%d\t%s\n", i, g_games[i].console, g_games[i].has_auto,
			              g_games[i].name);
		}
		(void)sendto(s, pkt, (size_t)k, 0, (const struct sockaddr*)to, sizeof(*to));
		if (g_ngames == 0) {
			break;
		}
	}
	pthread_mutex_unlock(&g_games_lock);
}

static void* retro_thread(void* arg) {
	(void)arg;
	minigb_apu_audio_init(&g_apu);
	scan_games();
	rlog("games: %d in " RETRO_DIR, g_ngames);
	const int          s = socket(AF_INET, SOCK_DGRAM, 0);
	struct sockaddr_in to;
	memset(&to, 0, sizeof(to));
	to.sin_family      = AF_INET;
	to.sin_port        = htons(RETRO_PORT);
	to.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
	g_frame_to   = to;
	g_frame_sock = s;
	if (s >= 0) {
		int big = 1024 * 1024;
		(void)setsockopt(s, SOL_SOCKET, SO_SNDBUF, &big, sizeof(big));
	}
	// from the home screen module: "J" + buttons (0 = pressed), "M" + 1/0 (playing or paused), "P<id>" (play
	// that game), "Q" (look for games again + send the list)
	const int          js = socket(AF_INET, SOCK_DGRAM, 0);
	struct sockaddr_in ja;
	memset(&ja, 0, sizeof(ja));
	ja.sin_family      = AF_INET;
	ja.sin_port        = htons(RETRO_PORT + 1);
	ja.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
	if (js < 0 || bind(js, (struct sockaddr*)&ja, sizeof(ja)) != 0) {
		rlog("games: no controls (cannot listen on %d)", RETRO_PORT + 1);
	}
	uint8_t         joypad   = 0xFF;
	int             playing  = 0;
	int             played   = 0; // played since the last Continue state
	const long long frame_ns = 16742706; // 59.7275 frames per second, as the real Game Boy
	long long       next     = now_ns(), busy = 0, t0 = now_ns(), last_save = now_ns();
	uint32_t        n        = 0;
	while (g_running_retro) {
		char    pkt[64];
		ssize_t got;
		while (js >= 0 && (got = recv(js, pkt, sizeof(pkt) - 1, MSG_DONTWAIT)) > 0) {
			pkt[got] = 0;
			if (pkt[0] == 'K' && got == 9) { // the whole DualSense: buttons (4), sticks (4 x -128..127)
				uint32_t b = 0;
				memcpy(&b, pkt + 1, 4);
				ps_lr_pad(b, (int8_t)pkt[5], (int8_t)pkt[6], (int8_t)pkt[7], (int8_t)pkt[8]);
				ps_mesen_buttons(b, (int8_t)pkt[5], (int8_t)pkt[6]);
			} else if (pkt[0] == 'J' && got == 2) {
				joypad = (uint8_t)pkt[1];
				ps_mesen_pad(joypad);
			} else if (pkt[0] == 'M' && got == 2) {
				joypad  = 0xFF;
				ps_mesen_pad(joypad);
				playing = pkt[1] != 0 && g_cur >= 0;
				ps_mesen_pause(!playing);
				if (g_cur >= 0 && lr_core(g_games[g_cur].console) >= 0) {
					ps_lr_pause(!playing);
				}
				if (!playing) {
					save_game();
					if (PLAYSHELL_SAVE_STATES && played) { // played since the last one: the Continue state (paused: it stays the same)
						played = 0;
						(void)game_state(1, "auto");
						if (s >= 0) {
							send_list(s, &to);
						}
					}
				}
			} else if (pkt[0] == 'S' || pkt[0] == 'T') { // the menu: Square saves a state, Triangle loads it (and plays)
				const int ok = game_state(pkt[0] == 'S' ? 1 : 2, "state");
				if (g_notify != NULL && g_cur >= 0) {
					char line[200];
					snprintf(line, sizeof(line), "PlayShell: %s - %s", g_games[g_cur].name,
					         pkt[0] == 'S' ? (ok ? "state saved" : "state could not be saved")
					                       : (ok ? "state loaded" : "no saved state yet"));
					g_notify(line);
				}
				if (pkt[0] == 'T' && g_cur >= 0) {
					joypad  = 0xFF;
					playing = 1;
					played  = 1;
					ps_mesen_pause(0);
					if (lr_core(g_games[g_cur].console) >= 0) {
						ps_lr_pause(0);
					}
					next = now_ns();
				}
			} else if (pkt[0] == 'P' || pkt[0] == 'C') { // play a game: "P" from its start, "C" continue (its state)
				const int id = atoi(pkt + 1);
				if (id != g_cur) {
					load_game(id, pkt[0] == 'C');
					next = now_ns(); // loading took time: no fast-forward to catch up
				}
				joypad  = 0xFF;
				playing = g_cur >= 0;
				played  = playing;
				ps_mesen_pause(!playing);
				if (g_cur >= 0 && lr_core(g_games[g_cur].console) >= 0) {
					ps_lr_pause(!playing);
				}
				if (s >= 0) {
					send_list(s, &to);
				}
			} else if (pkt[0] == 'Q') {
				scan_games();
				if (s >= 0) {
					send_list(s, &to);
				}
			}
		}
		g_gb.direct.joypad = joypad;
		const long long a  = now_ns();
		const int       gb = g_cur >= 0 && g_games[g_cur].console < 2; // a Game Boy game (NES: Mesen's thread)
		if (playing && gb) {
			gb_run_frame(&g_gb);
			sound_frame();
		}

		busy += now_ns() - a;
		if (s >= 0 && gb && (playing || n % 30 == 0)) {
			ps_send_picture(&g_frame[0][0], 160, 144);
		}
		if (s >= 0 && n % 120 == 0) {
			send_list(s, &to); // the menu's list (and which game is loaded)
		}
		if (playing && now_ns() - last_save > 60000000000LL) { // battery save every minute while playing
			save_game();
			last_save = now_ns();
		}
		++n;
		if (playing && gb && n % 600 == 0) {
			const long long t = now_ns();
			rlog("games: %.1f fps, emulator %.2f ms per frame", 600.0 * 1e9 / (double)(t - t0), busy / 600.0 / 1e6);
			t0 = t, busy = 0;
		} else if (!playing) {
			t0 = now_ns(), busy = 0;
		}
		next += frame_ns;
		const long long wait = next - now_ns();
		if (wait > 0) {
			struct timespec ts = {(time_t)(wait / 1000000000LL), (long)(wait % 1000000000LL)};
			nanosleep(&ts, NULL);
		} else if (wait < -100000000LL) {
			next = now_ns(); // fell far behind (rest mode): no catching up
		}
	}
	save_game();
	if (s >= 0) {
		close(s);
	}
	if (js >= 0) {
		close(js);
	}
	return NULL;
}

int emu_start(void (*log_line)(const char* line), void (*notify_line)(const char* line)) {
	g_log           = log_line;
	g_notify        = notify_line;
	g_running_retro = 1;
	pthread_t t;
	pthread_attr_t attr; // 8 MB of stack: the emulators (Mesen above all) need more than a thread's default
	pthread_attr_init(&attr);
	pthread_attr_setstacksize(&attr, 8u << 20);
	const int made = pthread_create(&t, &attr, retro_thread, NULL);
	pthread_attr_destroy(&attr);
	if (made != 0) {
		return 0;
	}
	pthread_detach(t);
	return 1;
}
