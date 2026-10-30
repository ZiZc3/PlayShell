/*
 * playshell_shellui.cpp - PlayShell: games on the PS5 home screen, the part injected into SceShellUI.
 * The payload (playshell_daemon.cpp) injects this as an ELF and runs the emulator (playshell_emu.c); every frame
 * comes here over UDP (127.0.0.1:9053), the controller goes back (9054), the sound comes on 9055 and is played
 * from here (the PS5 silences a payload's own sound). Hold R3: the PlayShell menu; Cross: play in a pop-up.
 * GPL-3.0. Taken from LiveShell (same author), built on OnionHEN - its injector, detour library, and the way
 * it drives ShellUI's Mono. Rules learned there:
 *   - every UI call on ShellUI's UI thread (we detour PUI.Application.Update)
 *   - pin every Mono object we keep a raw pointer to (the GC moves objects)
 *   - our widgets go on the home screen's own scene (AppScene) as its last child: a scene of our own is not drawn
 * Log: sent to the payload (UDP 9056), which writes /data/PlayShell/playshell.log.
 */
#include <atomic>
#include <algorithm>
#include <cmath>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cctype>
#include <cstdlib>
#include <cstring>
#include <string>
#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <sys/stat.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <time.h>
#include <unistd.h>

#include <ps5/kernel.h>

#include <onion/detour.h>

#include "playshell_icons.h"
#include "playshell_png.h"

#define PLAYSHELL_VERSION "0.1" // shown in the menu's top line
#define PLAYSHELL_SAVE_STATES 0 // save states + Continue (built, not in 0.1: 1 brings them back - the payload's too)

extern "C" {
int       get_module_handle(pid_t pid, const char* module_name);
uintptr_t set_ucred_to_ptrace(void);
uintptr_t set_proc_authid(pid_t pid, uintptr_t new_authid);
int       sceKernelSendNotificationRequest(int, void*, size_t, int);
}

// libonion_detour reads these as weak externs that the host module must define. Left undefined,
// the weak symbol sits at address 0 and the first InstallDetour crashes ShellUI.
int (*sceKernelMprotect)(void* addr, size_t len, int prot) = nullptr;
bool has_hv_bypass = false;

namespace {

// ---- logging: lines go to the payload (it writes the log file; ShellUI may write only ~16 KB of files) ----
int  g_tick  = 0; // UI frames since injection
long g_t0_ms = 0;

long now_ms() {
	timespec ts {};
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return static_cast<long>(ts.tv_sec) * 1000 + ts.tv_nsec / 1000000;
}

int         g_log_sock = -1;
sockaddr_in g_log_addr {};

void wp_log(const char* fmt, ...) {
	if (g_log_sock < 0) {
		g_log_sock                  = socket(AF_INET, SOCK_DGRAM, 0);
		g_log_addr.sin_family      = AF_INET;
		g_log_addr.sin_port        = htons(9056);
		g_log_addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
		if (g_log_sock < 0) {
			return;
		}
	}
	char      line[1400];
	const int head = snprintf(line, sizeof(line), "[%6ld ms] ", now_ms() - g_t0_ms);
	va_list   args;
	va_start(args, fmt);
	int n = vsnprintf(line + head, sizeof(line) - 1 - head, fmt, args);
	va_end(args);
	if (n < 0) {
		return;
	}
	n = std::min(n + head, static_cast<int>(sizeof(line)) - 2);
	line[n++] = '\n';
	if (sendto(g_log_sock, line, static_cast<size_t>(n), 0, reinterpret_cast<sockaddr*>(&g_log_addr), sizeof(g_log_addr)) < 0) {
		close(g_log_sock); // broken (rest mode): a new socket next time
		g_log_sock = -1;
	}
}

void write_status(const char* fmt, ...) { // (LiveShell's status line: here only a log line)
	char    text[300];
	va_list args;
	va_start(args, fmt);
	vsnprintf(text, sizeof(text), fmt, args);
	va_end(args);
	wp_log("status: %s", text);
}

void notify(const char* text) {
	struct {
		char unused[45];
		char message[3075];
	} req = {};
	snprintf(req.message, sizeof(req.message), "%s", text);
	sceKernelSendNotificationRequest(0, &req, sizeof(req), 0);
}

// ---- Mono ------------------------------------------------------------------------------------
using Obj = void*;
Obj    (*mono_get_root_domain)();
Obj    (*mono_thread_attach)(Obj domain);
Obj    (*mono_domain_assembly_open)(Obj domain, const char* name);
Obj    (*mono_assembly_get_image)(Obj assembly);
Obj    (*mono_class_from_name)(Obj image, const char* name_space, const char* name);
Obj    (*mono_class_get_method_from_name)(Obj klass, const char* name, int param_count);
Obj    (*mono_class_get_property_from_name)(Obj klass, const char* name);
Obj    (*mono_class_get_field_from_name)(Obj klass, const char* name);
Obj    (*mono_class_get_parent)(Obj klass);
Obj    (*mono_property_get_get_method)(Obj prop);
Obj    (*mono_property_get_set_method)(Obj prop);
Obj    (*mono_runtime_invoke)(Obj method, Obj obj, void** params, Obj* exc);
Obj    (*mono_object_new)(Obj domain, Obj klass);
Obj    (*mono_string_new)(Obj domain, const char* text);
void*  (*mono_object_unbox)(Obj obj);
Obj    (*mono_object_to_string)(Obj obj, Obj* exc);
char*  (*mono_string_to_utf8)(Obj str);
uint32_t (*mono_gchandle_new)(Obj obj, int pinned);
Obj    (*mono_method_desc_new)(const char* name, int include_namespace);
Obj    (*mono_method_desc_search_in_class)(Obj desc, Obj klass);
void   (*mono_method_desc_free)(Obj desc);
void*  (*mono_compile_method)(Obj method);
void   (*mono_field_get_value)(Obj obj, Obj field, void* value);
void   (*mono_field_set_value)(Obj obj, Obj field, void* value);
Obj    (*mono_get_corlib)();
void*  (*mono_class_get_type)(Obj klass);
Obj    (*mono_type_get_object)(Obj domain, void* type);
Obj    (*mono_method_get_object)(Obj domain, Obj method, Obj klass);
// Optional (diagnostics only): list a class's properties.
Obj         (*mono_class_get_properties)(Obj klass, void** iter);
const char* (*mono_property_get_name)(Obj prop);
Obj         (*mono_class_get_methods)(Obj klass, void** iter);
const char* (*mono_method_get_name)(Obj method);
int         (*mono_image_get_table_rows)(Obj image, int table);
Obj         (*mono_object_get_class)(Obj obj);
Obj         (*mono_class_get_fields)(Obj klass, void** iter);
const char* (*mono_field_get_name)(Obj field);
Obj         (*mono_method_signature)(Obj method);
Obj         (*mono_signature_get_return_type)(Obj sig);
char*       (*mono_type_get_name)(Obj type);
Obj         (*mono_class_from_mono_type)(Obj type);
int         (*mono_class_is_enum)(Obj klass);
int         (*mono_class_is_valuetype)(Obj klass);
Obj         (*mono_class_vtable)(Obj domain, Obj klass);
void        (*mono_field_static_get_value)(Obj vtable, Obj field, void* value);
Obj         (*mono_signature_get_params)(Obj sig, void** iter);
void        (*mono_method_get_param_names)(Obj method, const char** names);
Obj         (*mono_class_get)(Obj image, uint32_t token);
Obj         (*mono_array_new)(Obj domain, Obj eclass, uintptr_t n);
Obj         (*mono_get_byte_class)();
const char* (*mono_class_get_name)(Obj klass);

std::atomic<const char*> g_step{"start"};

// The class probes (every property / field / method of Sony's classes, used to write this file) stay out
// of a release's log: true only for development builds.
constexpr bool kProbeLogs = false;

// Heartbeat: if our thread crashes at a step, the heartbeat stops at the same moment; if it is stuck
// in a step while ShellUI keeps running until something kills it, the heartbeat keeps going.
void* heartbeat_thread(void*) {
	const char* last = nullptr;
	for (int i = 0; i < 100; ++i) { // ~70 s: a new step at once, the same step every 10 s
		const char* step = g_step.load();
		if (step != last || i % 10 == 0) {
			wp_log("heartbeat, step: %s", step);
			last = step;
		}
		usleep(i < 40 ? 250 * 1000 : 1000 * 1000);
	}
	return nullptr;
}

// Early after power-on (issue #16) the home screen died during these lookups: log each one then, so the last
// line names the call that killed it. Later starts stay quiet.
bool early_boot() {
	timespec up {};
	clock_gettime(CLOCK_MONOTONIC, &up);
	return up.tv_sec < 300;
}

template <typename T>
bool resolve(int handle, T& fn, const char* name) {
	g_step.store(name);
	static const bool early = early_boot();
	if (early) {
		wp_log("early boot: dlsym %s", name);
	}
	fn = reinterpret_cast<T>(kernel_dynlib_dlsym(-1, static_cast<uint32_t>(handle), name));
	if (fn == nullptr) {
		wp_log("missing Mono symbol: %s", name);
	}
	return fn != nullptr;
}
#define RESOLVE(f) ok &= resolve(lib, f, #f)

// Caller holds the raised authid (get_module_handle / dlsym across modules need it).
bool resolve_kernel() {
	g_step.store("get_module_handle(libkernel_sys)");
	const int lib = get_module_handle(getpid(), "libkernel_sys.sprx");
	if (lib <= 0) {
		wp_log("libkernel_sys handle %d", lib);
		return false;
	}
	sceKernelMprotect = reinterpret_cast<int (*)(void*, size_t, int)>(
	    kernel_dynlib_dlsym(-1, static_cast<uint32_t>(lib), "sceKernelMprotect"));
	wp_log("libkernel_sys handle %d, sceKernelMprotect %p", lib, reinterpret_cast<void*>(sceKernelMprotect));
	return sceKernelMprotect != nullptr;
}

bool resolve_mono() {
	g_step.store("get_module_handle(libmonosgen)");
	wp_log("looking up libmonosgen");
	const int lib = get_module_handle(getpid(), "libmonosgen-2.0.sprx");
	wp_log("libmonosgen handle %d, resolving symbols", lib);
	bool            ok         = lib > 0;
	if (ok) {
		RESOLVE(mono_get_root_domain);
		RESOLVE(mono_thread_attach);
		RESOLVE(mono_domain_assembly_open);
		RESOLVE(mono_assembly_get_image);
		RESOLVE(mono_class_from_name);
		RESOLVE(mono_class_get_method_from_name);
		RESOLVE(mono_class_get_property_from_name);
		RESOLVE(mono_class_get_field_from_name);
		RESOLVE(mono_class_get_parent);
		RESOLVE(mono_property_get_get_method);
		RESOLVE(mono_property_get_set_method);
		RESOLVE(mono_runtime_invoke);
		RESOLVE(mono_object_new);
		RESOLVE(mono_string_new);
		RESOLVE(mono_object_unbox);
		RESOLVE(mono_object_to_string);
		RESOLVE(mono_string_to_utf8);
		RESOLVE(mono_gchandle_new);
		RESOLVE(mono_method_desc_new);
		RESOLVE(mono_method_desc_search_in_class);
		RESOLVE(mono_method_desc_free);
		RESOLVE(mono_compile_method);
		RESOLVE(mono_field_get_value);
		RESOLVE(mono_field_set_value);
		RESOLVE(mono_get_corlib);
		RESOLVE(mono_class_get_type);
		RESOLVE(mono_type_get_object);
		RESOLVE(mono_method_get_object);
		resolve(lib, mono_class_get_properties, "mono_class_get_properties");
		resolve(lib, mono_property_get_name, "mono_property_get_name");
		resolve(lib, mono_class_get_methods, "mono_class_get_methods");
		resolve(lib, mono_method_get_name, "mono_method_get_name");
		resolve(lib, mono_image_get_table_rows, "mono_image_get_table_rows");
		resolve(lib, mono_object_get_class, "mono_object_get_class");
		resolve(lib, mono_class_get_fields, "mono_class_get_fields");
		resolve(lib, mono_field_get_name, "mono_field_get_name");
		resolve(lib, mono_method_signature, "mono_method_signature");
		resolve(lib, mono_signature_get_return_type, "mono_signature_get_return_type");
		resolve(lib, mono_type_get_name, "mono_type_get_name");
		resolve(lib, mono_class_from_mono_type, "mono_class_from_mono_type");
		resolve(lib, mono_class_is_enum, "mono_class_is_enum");
		resolve(lib, mono_class_is_valuetype, "mono_class_is_valuetype");
		resolve(lib, mono_class_vtable, "mono_class_vtable");
		resolve(lib, mono_field_static_get_value, "mono_field_static_get_value");
		resolve(lib, mono_signature_get_params, "mono_signature_get_params");
		resolve(lib, mono_method_get_param_names, "mono_method_get_param_names");
		resolve(lib, mono_class_get, "mono_class_get");
		resolve(lib, mono_class_get_name, "mono_class_get_name");
		resolve(lib, mono_array_new, "mono_array_new");
		resolve(lib, mono_get_byte_class, "mono_get_byte_class");
	}
	wp_log("libmonosgen handle %d, symbols %s", lib, ok ? "ok" : "MISSING");
	return ok;
}

#undef RESOLVE

Obj g_domain = nullptr;

// ---- Mono helpers ----------------------------------------------------------------------------

void log_exception(const char* what, Obj exc) {
	if (exc == nullptr) {
		return;
	}
	Obj         inner = nullptr;
	Obj         text  = mono_object_to_string(exc, &inner);
	const char* utf8  = text != nullptr ? mono_string_to_utf8(static_cast<char*>(text)) : nullptr;
	wp_log("%s threw: %s", what, utf8 != nullptr ? utf8 : "(no text)");
}

bool invoke(Obj method, Obj obj, void** args, const char* what, Obj* result = nullptr) {
	if (method == nullptr) {
		wp_log("no method for %s", what);
		return false;
	}
	Obj exc = nullptr;
	Obj ret = mono_runtime_invoke(method, obj, args, &exc);
	log_exception(what, exc);
	if (result != nullptr) {
		*result = exc == nullptr ? ret : nullptr;
	}
	return exc == nullptr;
}

Obj property_get(Obj klass, Obj obj, const char* name) {
	Obj prop = mono_class_get_property_from_name(klass, name);
	Obj get  = prop != nullptr ? mono_property_get_get_method(prop) : nullptr;
	if (get == nullptr) {
		wp_log("no getter %s", name);
		return nullptr;
	}
	Obj exc    = nullptr;
	Obj result = mono_runtime_invoke(get, obj, nullptr, &exc);
	log_exception(name, exc);
	return exc != nullptr ? nullptr : result;
}

bool property_set(Obj klass, Obj obj, const char* name, void* value) {
	Obj prop = mono_class_get_property_from_name(klass, name);
	Obj set  = prop != nullptr ? mono_property_get_set_method(prop) : nullptr;
	if (set == nullptr) {
		wp_log("no setter %s", name);
		return false;
	}
	void* args[1] = {value};
	Obj   exc     = nullptr;
	mono_runtime_invoke(set, obj, args, &exc);
	log_exception(name, exc);
	return exc == nullptr;
}



// The home screen's widget classes (found at setup)
Obj         g_ui3_widget  = nullptr;
Obj         g_color_class = nullptr;
Obj         g_dim_class   = nullptr;
const char* g_dim_prop    = nullptr; // the colour property of g_dim_class

const char* type_name_of(Obj type) {
	const char* n = type != nullptr && mono_type_get_name != nullptr ? mono_type_get_name(type) : nullptr;
	return n != nullptr ? n : "?";
}

Obj open_image(const char* path) {
	Obj assembly = mono_domain_assembly_open(g_domain, path);
	Obj image    = assembly != nullptr ? mono_assembly_get_image(assembly) : nullptr;
	if (image == nullptr) {
		wp_log("cannot open assembly %s", path);
	}
	return image;
}

uint64_t get_method_address(Obj image, const char* name_space, const char* class_name,
                            const char* method_name, int param_count) {
	Obj klass = mono_class_from_name(image, name_space, class_name);
	if (klass == nullptr) {
		wp_log("class %s.%s not found", name_space, class_name);
		return 0;
	}
	Obj method = mono_class_get_method_from_name(klass, method_name, param_count);
	if (method == nullptr) {
		wp_log("method %s.%s.%s(%d) not found", name_space, class_name, method_name, param_count);
		return 0;
	}
	return reinterpret_cast<uint64_t>(mono_compile_method(method));
}

Obj find_method_up(Obj klass, const char* name, int argc) {
	for (Obj k = klass; k != nullptr; k = mono_class_get_parent != nullptr ? mono_class_get_parent(k) : nullptr) {
		Obj m = mono_class_get_method_from_name(k, name, argc);
		if (m != nullptr) {
			return m;
		}
	}
	return nullptr;
}

Obj method_by_desc(Obj klass, const char* desc_text) {
	Obj desc   = mono_method_desc_new(desc_text, 1);
	Obj method = desc != nullptr ? mono_method_desc_search_in_class(desc, klass) : nullptr;
	if (desc != nullptr) {
		mono_method_desc_free(desc);
	}
	if (method == nullptr) {
		wp_log("method %s not found", desc_text);
	}
	return method;
}

bool has_setter(Obj klass, const char* name) {
	Obj prop = mono_class_get_property_from_name(klass, name);
	return prop != nullptr && mono_property_get_set_method(prop) != nullptr;
}

// A pinned, boxed UIColor(0, 0, 0, alpha); returns the unboxed value to pass to a setter.
void* black(float alpha, bool pin = true) {
	float r = 0.0f, g = 0.0f, b = 0.0f;
	Obj   boxed = mono_object_new(g_domain, g_color_class);
	if (boxed == nullptr) {
		return nullptr;
	}
	if (pin) {
		mono_gchandle_new(boxed, 1);
	}
	void* raw     = mono_object_unbox(boxed);
	void* args[4] = {&r, &g, &b, &alpha};
	return invoke(mono_class_get_method_from_name(g_color_class, ".ctor", 4), raw, args, "UIColor()") ? raw : nullptr;
}

Obj find_class(const char* dll, const char* name) {
	char path[160];
	snprintf(path, sizeof(path), "/system_ex/common_ex/lib/%s.dll", dll);
	Obj       image = open_image(path);
	const int rows  = image != nullptr && mono_image_get_table_rows != nullptr ? mono_image_get_table_rows(image, 2) : 0;
	for (int i = 2; i <= rows; ++i) {
		Obj         k = mono_class_get(image, 0x02000000u | static_cast<uint32_t>(i));
		const char* n = k != nullptr ? mono_class_get_name(k) : nullptr;
		if (n != nullptr && strcmp(n, name) == 0) {
			return k;
		}
	}
	return nullptr;
}

bool detour_method(const char* dll, const char* klass_name, const char* method, int argc, void* hook, void** orig) {
	Obj k = find_class(dll, klass_name);
	Obj m = k != nullptr ? mono_class_get_method_from_name(k, method, argc) : nullptr;
	const uint64_t addr = m != nullptr ? reinterpret_cast<uint64_t>(mono_compile_method(m)) : 0;
	const uintptr_t old_authid = set_ucred_to_ptrace();
	const bool ok = addr != 0 && InstallDetour(addr, hook, orig) && *orig != nullptr;
	if (old_authid != 0) {
		set_proc_authid(getpid(), old_authid);
	}
	wp_log("hook %s.%s(%d): %s", klass_name, method, argc, ok ? "installed" : "FAILED");
	return ok;
}


// ---- games on the home screen ----
// The payload runs the emulator (playshell_emu.c) and sends every frame here (UDP 127.0.0.1:9053, 3 packets of
// 48 lines, one byte per pixel: the shade 0..3); this module turns it into Game Boy green and puts it into the
// texture. Before: a 60 fps test pattern proved the picture can change every frame (SetPixels ~0.05 ms).
// A Game Boy sized texture (160 x 144, RGBA: Sce.PlayStation.Core.Graphics.Texture2D) in an image box on the
// wallpaper layer, 3x (480 x 432), new pixels every frame (SetPixels from a byte[] we write straight into).
// The log says how many frames per second the pixels changed and how long SetPixels took.
constexpr int kRw = 160, kRh = 144;
// The game's pictures for one frame size (each console its own): two textures + their assets + the pixel array.
// Made the first time a size comes, then kept (switching consoles back and forth makes nothing new).
struct PicSet {
	int w = 0, h = 0, n = 0;
	Obj pic[2] {}, asset[2] {}, arr = nullptr;
};
PicSet  g_sets[6];
int     g_nsets = 0;
PicSet* g_set   = nullptr;
Obj     g_tk = nullptr, g_ak = nullptr, g_tctr = nullptr, g_actr = nullptr;
Obj     g_bezel = nullptr; // the pop-up's bezel (its picture and size follow the console)
bool    g_await = false;   // a game was started: its picture and bezel wait for its first frame
Obj     g_black = nullptr; // fullscreen: black over the whole home screen, under the game
Obj     g_spin  = nullptr; // the loading spinner: from picking a game to its first frame (big games load for a while)
Obj     g_spin_class = nullptr; // it is the PS5's own BusyIndicator (it turns by itself, as in the system's menus: our
                                // own pictures, swapped or shown in turn, did not animate on the home screen)
Obj     g_spin_name = nullptr; // under it: the game's name
char    g_loading[96] = "";    // the game being started
std::atomic<int>  g_full_flip{0}; // touchpad presses (the frame tick toggles fullscreen)
bool    g_full = false;
int     g_game_w = 0, g_game_h = 0; // the game's frame size (place_game)
Obj         g_panel_class = nullptr;
const char* g_panel_prop  = nullptr;
void        find_panel_class(Obj pui);
Obj         home_scene_root(Obj pui);
Obj   g_rt_tex = nullptr, g_rt_set = nullptr, g_rt_arr = nullptr, g_rt_box = nullptr;
// two pictures, in turn: one on screen, one written (shown at the next refresh)
constexpr int kRtPics = 2;
Obj   g_rt_pic[kRtPics] {}, g_rt_asset[kRtPics] {}, g_pop_img = nullptr;
int   g_rt_npic = 1;
int   g_rt_frame = 0, g_rt_start = 0, g_rt_fail = 0;
long  g_rt_t0 = 0, g_rt_us = 0, g_rt_n = 0;

Obj method_where(Obj klass, const char* name, int argc, int at, const char* type);

// A built-in picture (playshell_icons.h) as an ImageAsset: decoded once (a few ms), then shown at once.
Obj pic_asset(const char* name) {
	struct Cached {
		const char* name;
		Obj         asset;
	};
	static Cached cache[64];
	static int    cached = 0;
	for (int i = 0; i < cached; ++i) {
		if (strcmp(cache[i].name, name) == 0) {
			return cache[i].asset;
		}
	}
	const Pic* pic = nullptr;
	for (const Pic& p : kPics) {
		if (strcmp(p.name, name) == 0) {
			pic = &p;
		}
	}
	Obj core = open_image("/system_ex/common_ex/lib/Sce.PlayStation.Core.dll");
	Obj pui  = open_image("/system_ex/common_ex/lib/Sce.PlayStation.PUI.dll");
	Obj tk   = core != nullptr ? mono_class_from_name(core, "Sce.PlayStation.Core.Graphics", "Texture2D") : nullptr;
	Obj ak   = pui != nullptr ? mono_class_from_name(pui, "Sce.PlayStation.PUI", "ImageAsset") : nullptr;
	if (pic == nullptr || tk == nullptr || ak == nullptr || cached >= 64) {
		wp_log("picture %s: not available", name);
		return nullptr;
	}
	int      w = 0, h = 0;
	uint8_t* px = png::decode(pic->png, pic->size, &w, &h);
	if (px == nullptr) {
		wp_log("picture %s: cannot decode", name);
		return nullptr;
	}
	Obj   tex  = mono_object_new(g_domain, tk);
	int   fmt  = 1; // PixelFormat.Rgba
	bool  no   = false;
	void* ta[4] = {&w, &h, &no, &fmt};
	Obj   arr  = mono_array_new(g_domain, mono_get_byte_class(), static_cast<uintptr_t>(w) * h * 4);
	Obj   set  = method_where(tk, "SetPixels", 2, 1, "System.Array");
	Obj   actr = method_where(ak, ".ctor", 1, 0, "Texture");
	Obj   asset = mono_object_new(g_domain, ak);
	bool  ok    = tex != nullptr && arr != nullptr && set != nullptr && actr != nullptr && asset != nullptr &&
	          invoke(mono_class_get_method_from_name(tk, ".ctor", 4), tex, ta, "Texture2D(picture)");
	if (ok) {
		memcpy(static_cast<char*>(arr) + 32, px, static_cast<size_t>(w) * h * 4); // MonoArray data (64-bit)
		int   level   = 0;
		void* sa[2]   = {&level, arr};
		void* aa[1]   = {tex};
		ok = invoke(set, tex, sa, "SetPixels(picture)") && invoke(actr, asset, aa, "ImageAsset(picture)");
	}
	free(px);
	if (!ok) {
		return nullptr;
	}
	mono_gchandle_new(tex, 1);
	mono_gchandle_new(asset, 1);
	cache[cached++] = {name, asset};
	return asset;
}

// A method by name, parameter count and the type name of one parameter (overloads with the same count).
Obj method_where(Obj klass, const char* name, int argc, int at, const char* type) {
	void* iter = nullptr;
	while (Obj m = mono_class_get_methods(klass, &iter)) {
		const char* n = mono_method_get_name(m);
		if (n == nullptr || strcmp(n, name) != 0) {
			continue;
		}
		Obj   sig = mono_method_signature(m);
		void* pi  = nullptr;
		int   i = 0, hit = 0;
		while (Obj t = mono_signature_get_params(sig, &pi)) {
			const char* tn = type_name_of(t);
			if (i == at && strstr(tn, type) != nullptr) {
				hit = 1;
			}
			++i;
		}
		if (i == argc && hit) {
			return m;
		}
	}
	return nullptr;
}

constexpr int     kMaxPx = 1024 * 544; // the largest frame (any console; PSP at 2x: 960 x 540, PS1 at 2x: 640 x 480)
// Whole frames from the payload, in order, shown one per refresh: the emulator's frames do not come exactly on
// the home screen's refreshes (NES 60.1 fps vs 60; sometimes two in one refresh) - showing only the newest
// skipped frames (a slight stutter). At most 3 waiting: more, the oldest goes (the delay stays <= ~2 frames).
struct QFrame {
	int      w = 0, h = 0;
	uint16_t px[kMaxPx];
};
constexpr int   kQueue = 3;
QFrame          g_q[kQueue];
int             g_q_head = 0, g_q_n = 0;
pthread_mutex_t g_q_lock = PTHREAD_MUTEX_INITIALIZER;
char              g_rf_title[96] = "Game Boy"; // the game's name (from the payload)
// the menu's game list (from the payload): names, which one is loaded
constexpr int     kMaxGames = 200;
constexpr int     kConsoles = 14; // 0 Game Boy, 1 Game Boy Color, 2 NES, 3 Game Gear, 4 PlayStation, 5 SNES, 6 GBA, 7 PSP,
                                  // 8 Nintendo DS, 9 DOS, 10 Nintendo 64, 11 PC Engine, 12 WonderSwan, 13 Nintendo 3DS
char              g_gl_name[kMaxGames][96];
int               g_gl_con[kMaxGames]; // its console (kConsoles)
int               g_gl_auto[kMaxGames]; // it has a Continue state (the payload saved one)
std::atomic<int>  g_gl_total{0}, g_gl_cur{-1}, g_gl_new{0};
std::atomic<int>  g_rf_title_new{0};
std::atomic<uint32_t> g_rf_ready{0};   // its number + 1 (0: none yet)
std::atomic<uint32_t> g_rf_seq{0};     // odd while g_rf is being written (the reader copies again: no torn frames)

// A frame being put together from its parts. Two at a time: with several lanes the next frame's first parts can
// come before the last ones of this frame.
struct Assembly {
	uint32_t  f  = 0; // frame number (0: free)
	int       w  = 0, h = 0, got = 0;
	uint16_t* px = nullptr;
};


void frame_done(const Assembly& as) { // a whole frame: into the queue
	pthread_mutex_lock(&g_q_lock);
	if (g_q_n == kQueue) { // full: the oldest goes
		g_q_head = (g_q_head + 1) % kQueue;
		--g_q_n;
	}
	QFrame& q = g_q[(g_q_head + g_q_n) % kQueue];
	memcpy(q.px, as.px, static_cast<size_t>(as.w) * as.h * 2);
	q.w = as.w, q.h = as.h;
	++g_q_n;
	pthread_mutex_unlock(&g_q_lock);
	g_rf_ready.store(as.f + 1);
}

void rx_packet(uint8_t* pkt, ssize_t n, Assembly* as) {
	if (n > 1 && pkt[0] == 'L') { // the game list: "L<total>\t<loaded>\t<first>\n" + "<id>\t<console>\t<name>\n" ...
		pkt[n < 8199 ? n : 8199] = 0;
		char*     line  = reinterpret_cast<char*>(pkt) + 1;
		const int total = atoi(line);
		char*     t1    = strchr(line, '\t');
		const int cur   = t1 != nullptr ? atoi(t1 + 1) : -1;
		for (char* nl = strchr(line, '\n'); nl != nullptr;) {
			char* row = nl + 1;
			nl        = strchr(row, '\n');
			if (nl == nullptr) {
				break;
			}
			*nl         = 0;
			char*     tab  = strchr(row, '\t'); // id, console, has a Continue state, name
			char*     tab2 = tab != nullptr ? strchr(tab + 1, '\t') : nullptr;
			char*     tab3 = tab2 != nullptr ? strchr(tab2 + 1, '\t') : nullptr;
			const int id   = atoi(row);
			if (tab3 != nullptr && id >= 0 && id < kMaxGames) {
				const int con = atoi(tab + 1);
				g_gl_con[id]  = con >= 0 && con < kConsoles ? con : 0;
				g_gl_auto[id] = atoi(tab2 + 1);
				snprintf(g_gl_name[id], sizeof(g_gl_name[id]), "%s", tab3 + 1);
			}
		}
		g_gl_total.store(total < kMaxGames ? total : kMaxGames);
		g_gl_cur.store(cur);
		g_gl_new.store(1);
		return;
	}
	if (n > 1 && n < 96 && pkt[0] == 'T') { // the game's name
		memcpy(g_rf_title, pkt + 1, static_cast<size_t>(n - 1));
		g_rf_title[n - 1] = 0;
		g_rf_title_new.store(1);
		return;
	}
	// a frame part: "V", frame (4), width (2), height (2), first line (2), lines (1), RGB565 lines
	if (n < 12 || pkt[0] != 'V') {
		return;
	}
	uint32_t f = 0;
	uint16_t w = 0, h = 0, y = 0;
	memcpy(&f, pkt + 1, 4);
	memcpy(&w, pkt + 5, 2);
	memcpy(&h, pkt + 7, 2);
	memcpy(&y, pkt + 9, 2);
	const int k = pkt[11];
	if (f == 0 || w == 0 || h == 0 || w * h > kMaxPx || y + k > h || n != static_cast<ssize_t>(12 + k * w * 2)) {
		return;
	}
	Assembly* a = as[0].f == f && as[0].w == w && as[0].h == h ? &as[0] : as[1].f == f && as[1].w == w && as[1].h == h ? &as[1] : nullptr;
	if (a == nullptr) { // a new frame: in the free / older slot
		a = as[0].f == 0 || (as[1].f != 0 && as[0].f < as[1].f) ? &as[0] : &as[1];
		if (a->f > f && a->f != 0) {
			return; // older than both being put together: too late
		}
		a->f = f, a->w = w, a->h = h, a->got = 0;
	}
	memcpy(a->px + y * w, pkt + 12, static_cast<size_t>(k) * w * 2);
	a->got += k;
	if (a->got >= h) { // whole (no part comes twice)
		frame_done(*a);
		for (int i = 0; i < 2; ++i) { // an older frame still waiting for parts: after this one it is too late
			if (as[i].f != 0 && as[i].f <= f) {
				as[i].f = 0;
			}
		}
	}
}

int rx_socket(int port, int* buf_kb) {
	const int   s = socket(AF_INET, SOCK_DGRAM, 0);
	sockaddr_in a {};
	a.sin_family      = AF_INET;
	a.sin_port        = htons(static_cast<uint16_t>(port));
	a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
	int big = 2 * 1024 * 1024; // the PS5 gives at most 512 KB
	if (s >= 0 && setsockopt(s, SOL_SOCKET, SO_RCVBUF, &big, sizeof(big)) != 0) {
		big = 512 * 1024;
		(void)setsockopt(s, SOL_SOCKET, SO_RCVBUF, &big, sizeof(big));
	}
	int       got = 0;
	socklen_t gl  = sizeof(got);
	(void)getsockopt(s, SOL_SOCKET, SO_RCVBUF, &got, &gl);
	*buf_kb += got / 1024;
	if (s < 0 || bind(s, reinterpret_cast<sockaddr*>(&a), sizeof(a)) != 0) {
		wp_log("retro: cannot listen on %d", port);
		if (s >= 0) {
			close(s);
		}
		return -1;
	}
	return s;
}

// The payload's frames and list: 9053 (list, title, frames of older payloads) + the 4 frame lanes 9060-9063.
void* retro_rx_thread(void*) {
	int    kb = 0;
	pollfd fds[5] {};
	int    nfds = 0;
	for (const int port : {9053, 9060, 9061, 9062, 9063}) {
		const int s = rx_socket(port, &kb);
		if (s >= 0) {
			fds[nfds].fd     = s;
			fds[nfds].events = POLLIN;
			++nfds;
		}
	}
	wp_log("retro: %d frame lanes, %d KB to receive into", nfds, kb);
	if (nfds == 0) {
		return nullptr;
	}
	static uint8_t  pkt[8200]; // a frame part (<= 12 + 7680) or the game list (<= 8192)
	static uint16_t buf[2][kMaxPx];
	Assembly        as[2];
	as[0].px = buf[0], as[1].px = buf[1];
	for (;;) {
		if (poll(fds, static_cast<nfds_t>(nfds), -1) <= 0) {
			continue;
		}
		for (int i = 0; i < nfds; ++i) {
			if (!(fds[i].revents & POLLIN)) {
				continue;
			}
			ssize_t n;
			while ((n = recv(fds[i].fd, pkt, sizeof(pkt) - 1, MSG_DONTWAIT)) > 0) {
				rx_packet(pkt, n, as);
			}
		}
	}
	return nullptr;
}

// ---- controls: hold R3 0.5 s -> the controller plays the game (and back) ----
struct PadData { // Sce.PlayStation.Core.Input.GamePadData
	bool     Skip;
	uint32_t Buttons, ButtonsPrev, ButtonsDown, ButtonsUp;
	float    LeftX, LeftY, RightX, RightY;
};
using GetDataFn = PadData (*)(int device);
GetDataFn   g_getdata_orig = nullptr;
bool        g_game_mode = false; // playing (the pop-up)
std::atomic<int> g_mode{0};      // 0 home screen, 1 PlayShell menu, 2 playing
std::atomic<uint32_t> g_menu_keys{0}; // menu presses (edges) for the frame tick: bit 0 up, 1 down, 2 left, 3 right, 4 cross, 5 circle,
                                      // 6 square, 7 triangle
long        g_r3_since  = 0;
bool        g_r3_done   = false;
int         g_joy_sock  = -1;
sockaddr_in g_joy_to {};

void joy_send(char kind, uint8_t value) {
	if (g_joy_sock < 0) {
		g_joy_sock               = socket(AF_INET, SOCK_DGRAM, 0);
		g_joy_to.sin_family      = AF_INET;
		g_joy_to.sin_port        = htons(9054);
		g_joy_to.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
	}
	const char pkt[2] = {kind, static_cast<char>(value)};
	(void)sendto(g_joy_sock, pkt, 2, 0, reinterpret_cast<sockaddr*>(&g_joy_to), sizeof(g_joy_to));
}

PadData GetData_Hook(int device) {
	PadData d = g_getdata_orig(device);
	if (d.Buttons & 0x8000u) { // R3 held
		const long t = now_ms();
		if (g_r3_since == 0) {
			g_r3_since = t;
		} else if (!g_r3_done && t - g_r3_since >= 500) {
			g_r3_done   = true;
			const int m = g_mode.load();
			// home -> menu, menu -> home, playing -> menu (the game pauses)
			g_mode.store(m == 1 ? 0 : 1);
			g_game_mode = false;
			joy_send('M', 0);
			joy_send('J', 0xFF);
			if (m == 0) {
				joy_send('Q', 0);
			}
			wp_log("retro: %s", m == 1 ? "menu closed" : "PlayShell menu");
		}
	} else {
		g_r3_since = 0;
		g_r3_done  = false;
	}
	if (g_mode.load() == 0) {
		return d;
	}
	if (g_mode.load() == 1) { // the menu: presses (with repeat while held) for the frame tick; the home screen sees nothing
		static uint32_t held = 0;
		static long     since = 0, rep_at = 0;
		const uint32_t  dirs  = d.Buttons & 0xFu;
		const uint32_t  now_b = d.Buttons;
		const long      tm    = now_ms();
		uint32_t        keys  = 0;
		auto edge = [&](uint32_t mask) { return (now_b & mask) && !(held & mask); };
		if (edge(0x2u)) keys |= 1;
		if (edge(0x8u)) keys |= 2;
		if (edge(0x1u)) keys |= 4;
		if (edge(0x4u)) keys |= 8;
		if (edge(0x80u)) keys |= 16;
		if (edge(0x40u)) keys |= 32;
		if (edge(0x10u)) keys |= 64;
		if (edge(0x20u)) keys |= 128;
		if (edge(0x400u)) keys |= 256; // L1
		if (edge(0x800u)) keys |= 512; // R1
		if (dirs != (held & 0xFu)) {
			since = tm, rep_at = tm + 380;
		} else if (dirs != 0 && tm >= rep_at) { // held: repeat up / down
			rep_at = tm + 110;
			keys |= (dirs & 0x2u ? 1 : 0) | (dirs & 0x8u ? 2 : 0);
		}
		(void)since;
		held = now_b;
		if (keys != 0) {
			g_menu_keys.fetch_or(keys);
		}
		d.Buttons &= 0x8000u;
		d.ButtonsPrev = d.ButtonsDown = d.ButtonsUp = 0;
		d.LeftX = d.LeftY = d.RightX = d.RightY = 0.0f;
		return d;
	}
	{ // the touchpad (a button beyond the known 0x3FFFF: d-pad, shapes, Options, Create, L1-R3, confirm / back)
		static uint32_t prev = 0;
		const uint32_t  more = d.Buttons & ~0x3FFFFu;
		if ((more & ~prev) != 0) {
			g_full_flip.fetch_add(1);
			wp_log("retro: button %#x (touchpad?): fullscreen", more & ~prev);
		}
		prev = more;
		d.Buttons &= 0x3FFFFu; // not for the game
	}
	// playing: Game Boy buttons (0 = pressed): d-pad / left stick, cross or circle = A, square or triangle = B,
	// Options = START, Create = SELECT
	const uint32_t b = d.Buttons;
	uint8_t        j = 0xFF;
	if ((b & 0x80u) || (b & 0x40u)) j &= ~0x01; // A
	if ((b & 0x10u) || (b & 0x20u)) j &= ~0x02; // B
	if (b & 0x200u) j &= ~0x04;                 // SELECT
	if (b & 0x100u) j &= ~0x08;                 // START
	if ((b & 0x4u) || d.LeftX > 0.5f) j &= ~0x10;  // right
	if ((b & 0x1u) || d.LeftX < -0.5f) j &= ~0x20; // left
	if ((b & 0x2u) || d.LeftY < -0.5f) j &= ~0x40; // up
	if ((b & 0x8u) || d.LeftY > 0.5f) j &= ~0x80;  // down
	static uint8_t last = 0xFF;
	static long    sent = 0;
	const long     now  = now_ms();
	if (j != last || now - sent >= 100) {
		joy_send('J', j);
		last = j;
		sent = now;
	}
	{ // "K": every button + both sticks (-128..127)
		auto stick = [](float v) { return static_cast<int8_t>(std::max(-128.0f, std::min(127.0f, v * 127.0f))); };
		char k[9];
		k[0] = 'K';
		memcpy(k + 1, &b, 4);
		k[5] = stick(d.LeftX), k[6] = stick(d.LeftY), k[7] = stick(d.RightX), k[8] = stick(d.RightY);
		static char last_k[9] = {};
		static long sent_k    = 0;
		if (memcmp(k, last_k, 9) != 0 || now - sent_k >= 100) {
			if (g_joy_sock >= 0) {
				(void)sendto(g_joy_sock, k, 9, 0, reinterpret_cast<sockaddr*>(&g_joy_to), sizeof(g_joy_to));
			}
			memcpy(last_k, k, 9);
			sent_k = now;
		}
		static uint32_t seen = 0; // which button bits this controller uses (the first time each is pressed)
		if ((b & ~seen) != 0 && seen != 0xFFFFFFFFu) {
			wp_log("retro: buttons %#x (new bits %#x)", b, b & ~seen);
			seen |= b;
		}
	}
	// the home screen sees an idle controller (R3 stays visible: holding it again gives it back)
	d.Buttons &= 0x8000u;
	d.ButtonsPrev = d.ButtonsDown = d.ButtonsUp = 0;
	d.LeftX = d.LeftY = d.RightX = d.RightY = 0.0f;
	return d;
}

// The pop-up: a container (a dim over everything + the game 5x, 800 x 720, in the middle) appended as the LAST
// child of the home screen's own scene (the AppScene in SceneManager's render list): drawn on top of the home
// screen's content. Shown / hidden with Visible from the frame tick. (A scene of our own was Show()n "ok" but
// never drawn: only scenes registered with the home screen's scene system are.)
Obj  g_pop_box = nullptr, g_pop_wk = nullptr, g_pop_host = nullptr;
bool g_pop_on  = false;

Obj prop_up(Obj obj, const char* name) { // a property's value, looked up in the class and its parents
	for (Obj k = obj != nullptr ? mono_object_get_class(obj) : nullptr; k != nullptr; k = mono_class_get_parent(k)) {
		Obj prop = mono_class_get_property_from_name(k, name);
		Obj get  = prop != nullptr ? mono_property_get_get_method(prop) : nullptr;
		if (get != nullptr) {
			Obj exc = nullptr;
			Obj r   = mono_runtime_invoke(get, obj, nullptr, &exc);
			return exc == nullptr ? r : nullptr;
		}
	}
	return nullptr;
}

bool prop_set_up(Obj obj, const char* name, void* value) { // set a property found in the class or its parents
	for (Obj k = obj != nullptr ? mono_object_get_class(obj) : nullptr; k != nullptr; k = mono_class_get_parent(k)) {
		Obj prop = mono_class_get_property_from_name(k, name);
		Obj set  = prop != nullptr ? mono_property_get_set_method(prop) : nullptr;
		if (set != nullptr) {
			void* args[1] = {value};
			return invoke(set, obj, args, name);
		}
	}
	wp_log("no setter %s (in any parent)", name);
	return false;
}

// The home screen's scene root: the first AppScene in SceneManager's render list.
Obj home_scene_root(Obj pui) {
	Obj sm   = mono_class_from_name(pui, "Sce.PlayStation.PUI", "SceneManager");
	Obj ip   = sm != nullptr ? mono_class_get_property_from_name(sm, "Instance") : nullptr;
	Obj ig   = ip != nullptr ? mono_property_get_get_method(ip) : nullptr;
	Obj exc  = nullptr;
	Obj inst = ig != nullptr ? mono_runtime_invoke(ig, nullptr, nullptr, &exc) : nullptr;
	Obj list = exc == nullptr && inst != nullptr ? prop_up(inst, "RenderList") : nullptr;
	Obj cnt  = list != nullptr ? prop_up(list, "Count") : nullptr;
	Obj item = list != nullptr ? mono_class_get_method_from_name(mono_object_get_class(list), "get_Item", 1) : nullptr;
	const int n = cnt != nullptr ? *reinterpret_cast<int*>(static_cast<char*>(cnt) + 16) : 0;
	for (int i = 0; i < n && item != nullptr; ++i) {
		void* args[1] = {&i};
		Obj   e       = nullptr;
		Obj   sc      = mono_runtime_invoke(item, list, args, &e);
		const char* cn = e == nullptr && sc != nullptr ? mono_class_get_name(mono_object_get_class(sc)) : nullptr;
		if (cn != nullptr && strcmp(cn, "AppScene") == 0) {
			return prop_up(sc, "RootWidget");
		}
	}
	return nullptr;
}

Obj panel(Obj parent, Obj wk, float x, float y, float w, float h, float grey, float alpha, float radius);

void retro_popup_setup(Obj pui, Obj asset, Obj wk) {
	Obj bk  = mono_class_from_name(pui, "Sce.PlayStation.PUI.UI3", "ImageBox");
	Obj box = mono_object_new(g_domain, wk);
	Obj wc  = mono_class_get_method_from_name(wk, ".ctor", 0);
	if (box == nullptr || wc == nullptr || !invoke(wc, box, nullptr, "Widget(pop)")) {
		wp_log("retro: no pop-up container");
		return;
	}
	mono_gchandle_new(box, 1);
	float z = 0.0f, fw = 1920.0f, fh = 1080.0f;
	property_set(wk, box, "X", &z);
	property_set(wk, box, "Y", &z);
	property_set(wk, box, "Width", &fw);
	property_set(wk, box, "Height", &fh);
	Obj  apnd  = find_method_up(wk, "AppendChild", 1);
	bool dimok = false;
	if (g_dim_class != nullptr && g_dim_prop != nullptr) { // the dim: as the wallpaper's dim layer
		Obj dim = mono_object_new(g_domain, g_dim_class);
		Obj dc  = mono_class_get_method_from_name(g_dim_class, ".ctor", 0);
		if (dim != nullptr && dc != nullptr && invoke(dc, dim, nullptr, "dim()")) {
			property_set(wk, dim, "X", &z);
			property_set(wk, dim, "Y", &z);
			property_set(wk, dim, "Width", &fw);
			property_set(wk, dim, "Height", &fh);
			if (has_setter(g_dim_class, "BackgroundVisibility")) {
				bool on = true;
				property_set(g_dim_class, dim, "BackgroundVisibility", &on);
			}
			void* c    = black(0.72f);
			void* a[1] = {dim};
			dimok      = c != nullptr && property_set(g_dim_class, dim, g_dim_prop, c) && invoke(apnd, box, a, "AppendChild(pop dim)");
		}
	}
	if ((g_black = panel(box, wk, 0.0f, 0.0f, 1920.0f, 1080.0f, 0.0f, 1.0f, 0.0f)) != nullptr) { // fullscreen's black
		bool off = false;
		prop_set_up(g_black, "Visible", &off);
	}
	{ // the bezel: a retro frame around the screen (built-in picture, half size, shown 2x)
		Obj bz = bk != nullptr ? mono_object_new(g_domain, bk) : nullptr;
		Obj zc = bk != nullptr ? mono_class_get_method_from_name(bk, ".ctor", 0) : nullptr;
		if (bz != nullptr && zc != nullptr && invoke(zc, bz, nullptr, "ImageBox(bezel)")) {
			mono_gchandle_new(bz, 1);
			float bx = 420.0f, by = 48.0f, bw2 = 1080.0f, bh2 = 984.0f;
			property_set(wk, bz, "X", &bx);
			property_set(wk, bz, "Y", &by);
			property_set(wk, bz, "Width", &bw2);
			property_set(wk, bz, "Height", &bh2);
			if (Obj pic = pic_asset("bezel_160x144")) { // the Game Boy's; place_game fits it to each console
				property_set(bk, bz, "Image", pic);
			}
			g_bezel = bz;
			void* ba[1] = {bz};
			invoke(apnd, box, ba, "AppendChild(bezel)");
		}
	}
	Obj big = bk != nullptr ? mono_object_new(g_domain, bk) : nullptr;
	Obj bc  = bk != nullptr ? mono_class_get_method_from_name(bk, ".ctor", 0) : nullptr;
	if (big == nullptr || bc == nullptr || !invoke(bc, big, nullptr, "ImageBox(pop)")) {
		return;
	}
	mono_gchandle_new(big, 1);
	g_pop_img = big;
	property_set(bk, big, "Image", asset); // the same texture as the mini screen
	float x = 480.0f, y = 108.0f, w = 960.0f, h = 864.0f; // 6x: the whole Game Boy screen, sharp
	property_set(wk, big, "X", &x);
	property_set(wk, big, "Y", &y);
	property_set(wk, big, "Width", &w);
	property_set(wk, big, "Height", &h);
	void* a[1] = {big};
	invoke(apnd, box, a, "AppendChild(pop game)");
	{ // the loading spinner, in the middle: the PS5's own (BusyIndicator, square, 72 px)
		Obj bi = mono_class_from_name(pui, "Sce.PlayStation.PUI.UI3", "BusyIndicator");
		Obj sp = bi != nullptr ? mono_object_new(g_domain, bi) : nullptr;
		Obj sc = bi != nullptr ? mono_class_get_method_from_name(bi, ".ctor", 0) : nullptr;
		if (sp != nullptr && sc != nullptr && invoke(sc, sp, nullptr, "BusyIndicator()")) {
			mono_gchandle_new(sp, 1);
			int   style = 3; // FreeSizeSquare: its size as set
			float sx = 924.0f, sy = 504.0f, ss = 72.0f;
			prop_set_up(sp, "Style", &style);
			prop_set_up(sp, "X", &sx);
			prop_set_up(sp, "Y", &sy);
			prop_set_up(sp, "Width", &ss);
			prop_set_up(sp, "Height", &ss);
			bool hide = false;
			prop_set_up(sp, "Visible", &hide);
			void* sa[1] = {sp};
			if (invoke(apnd, box, sa, "AppendChild(spinner)")) {
				g_spin       = sp;
				g_spin_class = bi;
			}
		}
		wp_log("retro: loading spinner %s", g_spin != nullptr ? "ready (BusyIndicator)" : "NOT available");
	}
	bool off = false;
	prop_set_up(box, "Visible", &off);
	g_pop_box = box;
	g_pop_wk  = wk;
	wp_log("retro: pop-up ready (dim %s)", dimok ? "on" : "none");
}

// ---- the card: in the middle of the home screen when you stop playing (6 s) ----
Obj g_card = nullptr, g_card_title = nullptr, g_label_class = nullptr;
int g_card_until = 0;

void* rgba(float r, float g, float b, float a) { // a UIColor
	Obj boxed = g_color_class != nullptr ? mono_object_new(g_domain, g_color_class) : nullptr;
	if (boxed == nullptr) {
		return nullptr;
	}
	mono_gchandle_new(boxed, 1);
	void* raw     = mono_object_unbox(boxed);
	void* args[4] = {&r, &g, &b, &a};
	return invoke(mono_class_get_method_from_name(g_color_class, ".ctor", 4), raw, args, "UIColor()") ? raw : nullptr;
}

Obj add_widget(Obj klass, Obj parent, Obj wk, float x, float y, float w, float h) {
	Obj o = klass != nullptr ? mono_object_new(g_domain, klass) : nullptr;
	Obj c = klass != nullptr ? mono_class_get_method_from_name(klass, ".ctor", 0) : nullptr;
	if (o == nullptr || c == nullptr || !invoke(c, o, nullptr, "new widget")) {
		return nullptr;
	}
	mono_gchandle_new(o, 1);
	prop_set_up(o, "X", &x);
	prop_set_up(o, "Y", &y);
	prop_set_up(o, "Width", &w);
	prop_set_up(o, "Height", &h);
	if (parent != nullptr) {
		void* a[1] = {o};
		invoke(find_method_up(wk, "AppendChild", 1), parent, a, "AppendChild(card)");
	}
	return o;
}

Obj add_label(Obj parent, Obj wk, float y, float h, const char* text, float grey) {
	Obj l = add_widget(g_label_class, parent, wk, 24.0f, y, 472.0f, h);
	if (l != nullptr) {
		prop_set_up(l, "Text", mono_string_new(g_domain, text));
		if (void* c = rgba(grey, grey, grey, 1.0f)) {
			prop_set_up(l, "TextColor", c);
		}
	}
	return l;
}

void retro_card_setup(Obj pui, Obj asset, Obj wk) {
	g_label_class = mono_class_from_name(pui, "Sce.PlayStation.PUI.UI3", "Label");
	Obj bk        = mono_class_from_name(pui, "Sce.PlayStation.PUI.UI3", "ImageBox");
	Obj card      = add_widget(wk, nullptr, wk, 700.0f, 250.0f, 520.0f, 560.0f); // the middle of the screen
	if (card == nullptr) {
		return;
	}
	if (g_dim_class != nullptr && g_dim_prop != nullptr) { // its background: a dark panel
		Obj bg = add_widget(g_dim_class, card, wk, 0.0f, 0.0f, 520.0f, 560.0f);
		if (bg != nullptr) {
			if (has_setter(g_dim_class, "BackgroundVisibility")) {
				bool on = true;
				property_set(g_dim_class, bg, "BackgroundVisibility", &on);
			}
			if (void* c = rgba(0.07f, 0.07f, 0.08f, 0.94f)) {
				property_set(g_dim_class, bg, g_dim_prop, c);
			}
		}
	}
	Obj img = add_widget(bk, card, wk, 20.0f, 20.0f, 480.0f, 432.0f); // the paused game, 3x
	if (img != nullptr) {
		prop_set_up(img, "Image", asset);
	}
	g_card_title = add_label(card, wk, 466.0f, 40.0f, g_rf_title, 1.0f);
	add_label(card, wk, 506.0f, 34.0f, "Hold R3 to play", 0.62f);
	bool off = false;
	prop_set_up(card, "Visible", &off);
	g_card = card;
	wp_log("retro: card ready (label %p)", g_label_class);
}

// Put a widget on top of the home screen's own scene (its last child) and show / hide it.
// ---- the layer PlayShell draws in ----
// The first MenuStackScene after the first AppScene in SceneManager's render list: over the focused tile's outline
// and the profile picture (found with a layer test on the PS5: "layer 7"; in the AppScene they were drawn over the
// game). Not found: the AppScene.
Obj host_root(Obj pui) {
	Obj sm   = mono_class_from_name(pui, "Sce.PlayStation.PUI", "SceneManager");
	Obj ip   = sm != nullptr ? mono_class_get_property_from_name(sm, "Instance") : nullptr;
	Obj ig   = ip != nullptr ? mono_property_get_get_method(ip) : nullptr;
	Obj exc  = nullptr;
	Obj inst = ig != nullptr ? mono_runtime_invoke(ig, nullptr, nullptr, &exc) : nullptr;
	Obj list = exc == nullptr && inst != nullptr ? prop_up(inst, "RenderList") : nullptr;
	Obj cnt  = list != nullptr ? prop_up(list, "Count") : nullptr;
	Obj item = list != nullptr ? mono_class_get_method_from_name(mono_object_get_class(list), "get_Item", 1) : nullptr;
	const int n        = cnt != nullptr ? *reinterpret_cast<int*>(static_cast<char*>(cnt) + 16) : 0;
	bool      after_app = false;
	for (int i = 0; i < n && item != nullptr; ++i) {
		void* args[1] = {&i};
		Obj   e       = nullptr;
		Obj   sc      = mono_runtime_invoke(item, list, args, &e);
		const char* cn = e == nullptr && sc != nullptr ? mono_class_get_name(mono_object_get_class(sc)) : nullptr;
		if (cn == nullptr) {
			continue;
		}
		if (!after_app) {
			after_app = strcmp(cn, "AppScene") == 0;
			continue;
		}
		if (strcmp(cn, "MenuStackScene") == 0) {
			Obj  root = prop_up(sc, "RootWidget");
			bool ui3  = false;
			for (Obj c = root != nullptr ? mono_object_get_class(root) : nullptr; c != nullptr && !ui3; c = mono_class_get_parent(c)) {
				ui3 = c == g_ui3_widget;
			}
			if (ui3) {
				static bool told = false;
				if (!told) {
					told = true;
					wp_log("retro: drawn in %s #%d (over the home screen's tile outline)", cn, i);
				}
				return root;
			}
		}
	}
	return home_scene_root(pui);
}

// Fades in: a widget's Opacity from 0 to 1 over some frames (fade_tick, every frame).
struct Fade {
	Obj   w;
	float a, step;
};
Fade g_fades[6];

void fade_in(Obj w, int frames) {
	if (w == nullptr) {
		return;
	}
	float zero = 0.0f;
	prop_set_up(w, "Opacity", &zero);
	for (Fade& f : g_fades) { // (already fading: from the start again)
		if (f.w == w || f.w == nullptr) {
			f = {w, 0.0f, 1.0f / static_cast<float>(frames > 0 ? frames : 1)};
			return;
		}
	}
	float one = 1.0f; // (no free place: shown at once)
	prop_set_up(w, "Opacity", &one);
}

void fade_tick() {
	for (Fade& f : g_fades) {
		if (f.w == nullptr) {
			continue;
		}
		f.a = std::min(1.0f, f.a + f.step);
		const float eased = f.a * f.a * (3.0f - 2.0f * f.a); // (smooth start and end)
		prop_set_up(f.w, "Opacity", const_cast<float*>(&eased));
		if (f.a >= 1.0f) {
			f.w = nullptr;
		}
	}
}

void show_on_home(Obj w, bool on) {
	if (w == nullptr) {
		return;
	}
	if (on) {
		Obj pui  = open_image("/system_ex/common_ex/lib/Sce.PlayStation.PUI.dll");
		Obj root = pui != nullptr ? host_root(pui) : nullptr;
		if (root != nullptr) {
			if (prop_up(w, "Parent") != nullptr) {
				invoke(find_method_up(g_pop_wk, "RemoveFromParent", 0), w, nullptr, "RemoveFromParent");
			}
			void* a[1] = {w};
			invoke(find_method_up(g_pop_wk, "AppendChild", 1), root, a, "AppendChild(home)");
			static Obj told = nullptr;
			if (told != root) { // the layer's size and clipping (once each): PlayShell's 1920 x 1080 must not be cut
				told       = root;
				Obj  cx    = prop_up(root, "X"), cy = prop_up(root, "Y"), cw = prop_up(root, "Width"), ch = prop_up(root, "Height");
				Obj  cc    = prop_up(root, "ClipChildren");
				auto f     = [](Obj o) { return o != nullptr ? *static_cast<float*>(mono_object_unbox(o)) : -1.0f; };
				const bool clip = cc != nullptr && *static_cast<bool*>(mono_object_unbox(cc));
				wp_log("retro: the layer: %.0f,%.0f %.0f x %.0f, clipping %s", f(cx), f(cy), f(cw), f(ch), clip ? "ON - turned off" : "off");
				if (clip) {
					bool off = false;
					prop_set_up(root, "ClipChildren", &off);
				}
			}
		}
	}
	prop_set_up(w, "Visible", &on);
}

// ---- the PlayShell menu: page 1 the consoles (big icon tiles), page 2 one console's games ----
constexpr int kCons = kConsoles;
const char* const kConName[kCons] = {"Game Boy", "Game Boy Color", "NES", "Game Gear", "PlayStation", "SNES", "Game Boy Advance", "PSP",
                                     "Nintendo DS", "DOS", "Nintendo 64", "PC Engine", "WonderSwan", "Nintendo 3DS"};
const char* const kConIcon[kCons] = {"console_gb", "console_gbc", "console_nes", "console_gg", "console_ps1", "console_snes", "console_gba", "console_psp",
                                     "console_nds", "console_dos", "console_n64", "console_pce", "console_ws", "console_3ds"};
#if PLAYSHELL_3DS
const int         kConOrder[] = {4, 10, 13, 8, 6, 5, 2, 11, 1, 0, 3, 12, 9};
#else
const int         kConOrder[] = {4, 10, 8, 6, 5, 2, 11, 1, 0, 3, 12, 9}; // (13 Nintendo 3DS: off - tools/azahar_lib.sh)
#endif // the order shown (console ids): PlayStation, N64, DS first
constexpr int     kShown      = sizeof(kConOrder) / sizeof(kConOrder[0]); // consoles in the menu (PSP's id 7: removed)
constexpr int kCols = 6, kTileRows = 3, kTiles = kCols * kTileRows; // console tiles on page 1 (more: it scrolls)
constexpr int kRows = 9;                                             // game rows on page 2
constexpr float kCardW = 1280.0f, kCardH = 780.0f;
Obj   g_menu = nullptr, g_page1 = nullptr, g_page2 = nullptr, g_head = nullptr, g_empty = nullptr, g_box_class = nullptr;
Obj   g_tile_bg[kTiles] {}, g_tile_icon[kTiles] {}, g_tile_name[kTiles] {}, g_tile_count[kTiles] {};
Obj   g_row_bg[kRows] {}, g_row_name[kRows] {}, g_row_tag[kRows] {}, g_hint_word[3] {}, g_page2_icon = nullptr;
Obj   g_hint_more = nullptr; // the footer's right side: the save state buttons / the Continue question's Square
int   g_ask = -1;            // a game with a Continue state was picked: Continue or Start over? (its id)
int   g_con = 0, g_con_top = 0; // the chosen console (place in kConOrder), the first tile row shown
int   g_sel = 0, g_top = 0, g_page = 1; // selected game, first row shown, the page shown

void* ui_font(int size, bool bold) { // a UIFont (a struct: boxed, the data passed)
	static Obj fk = nullptr;
	if (fk == nullptr) {
		Obj pui = open_image("/system_ex/common_ex/lib/Sce.PlayStation.PUI.dll");
		fk      = pui != nullptr ? mono_class_from_name(pui, "Sce.PlayStation.PUI.UI3", "UIFont") : nullptr;
	}
	Obj boxed = fk != nullptr ? mono_object_new(g_domain, fk) : nullptr;
	if (boxed == nullptr) {
		return nullptr;
	}
	mono_gchandle_new(boxed, 1);
	void* raw     = mono_object_unbox(boxed);
	int   style = 0, weight = bold ? 2 : 1;
	void* args[3] = {&size, &style, &weight};
	return invoke(mono_class_get_method_from_name(fk, ".ctor", 3), raw, args, "UIFont()") ? raw : nullptr;
}

Obj panel(Obj parent, Obj wk, float x, float y, float w, float h, float grey, float alpha, float radius = 0.0f) {
	Obj p = add_widget(g_panel_class, parent, wk, x, y, w, h);
	if (p != nullptr) {
		if (radius > 0.0f) {
			prop_set_up(p, "BorderRadius", &radius);
		}
		if (has_setter(g_panel_class, "BackgroundVisibility")) {
			bool on = true;
			property_set(g_panel_class, p, "BackgroundVisibility", &on);
		}
		if (void* c = rgba(grey, grey, grey, alpha)) {
			property_set(g_panel_class, p, g_panel_prop, c);
		}
	}
	return p;
}

void tint(Obj p, float grey, float alpha) {
	if (p != nullptr) {
		if (void* c = rgba(grey, grey, grey, alpha)) {
			property_set(g_panel_class, p, g_panel_prop, c);
		}
	}
}

Obj text(Obj parent, Obj wk, float x, float y, float w, float h, const char* t, int size, bool bold, float grey) {
	Obj l = add_widget(g_label_class, parent, wk, x, y, w, h);
	if (l != nullptr) {
		if (void* f = ui_font(size, bold)) {
			prop_set_up(l, "Font", f);
		}
		prop_set_up(l, "Text", mono_string_new(g_domain, t));
		if (void* c = rgba(grey, grey, grey, 1.0f)) {
			prop_set_up(l, "TextColor", c);
		}
	}
	return l;
}

void set_text(Obj l, const char* t, float grey) {
	if (l != nullptr) {
		prop_set_up(l, "Text", mono_string_new(g_domain, t));
		if (void* c = rgba(grey, grey, grey, 1.0f)) {
			prop_set_up(l, "TextColor", c);
		}
	}
}

// A widget that can be filled with a colour (as LiveShell's dim layer finds it); the pop-up's dim uses it too.
void find_panel_class(Obj pui) {
	static const char* const kClasses[] = {"Panel", "Rectangle", "ColorRect", "Plane", "Box", "Widget"};
	static const char* const kColors[]  = {"BackgroundColor", "Color", "FillColor", "BaseColor"};
	for (const char* cn : kClasses) {
		Obj k = mono_class_from_name(pui, "Sce.PlayStation.PUI.UI3", cn);
		for (const char* c : kColors) {
			if (k != nullptr && g_panel_prop == nullptr && has_setter(k, c)) {
				g_panel_class = k;
				g_panel_prop  = c;
			}
		}
		if (g_panel_prop != nullptr) {
			break;
		}
	}
	g_dim_class = g_panel_class;
	g_dim_prop  = g_panel_prop;
}

void retro_menu_setup(Obj pui, Obj wk) {
	g_label_class = mono_class_from_name(pui, "Sce.PlayStation.PUI.UI3", "Label");
	if (g_color_class == nullptr) {
		g_color_class = mono_class_from_name(pui, "Sce.PlayStation.PUI", "UIColor");
	}
	find_panel_class(pui);
	if (g_panel_class == nullptr || g_label_class == nullptr || g_color_class == nullptr) {
		wp_log("retro: menu classes panel %p label %p colour %p", g_panel_class, g_label_class, g_color_class);
		return;
	}
	Obj m = add_widget(wk, nullptr, wk, 0.0f, 0.0f, 1920.0f, 1080.0f);
	if (m == nullptr) {
		return;
	}
	panel(m, wk, 0.0f, 0.0f, 1920.0f, 1080.0f, 0.0f, 0.6f); // the home screen a little darker behind it
	// the card: solid, rounded, in the middle; everything else inside it
	Obj card = panel(m, wk, (1920.0f - kCardW) / 2, (1080.0f - kCardH) / 2, kCardW, kCardH, 0.07f, 1.0f, 30.0f);
	if (card == nullptr) {
		return;
	}
	g_box_class = mono_class_from_name(pui, "Sce.PlayStation.PUI.UI3", "ImageBox");
	if (Obj logo = add_widget(g_box_class, card, wk, 48.0f, 33.0f, 46.0f, 46.0f)) {
		if (Obj pic = pic_asset("playshell")) { // PlayShell's icon (a screen with a gamepad), before the title
			prop_set_up(logo, "Image", pic);
		}
	}
	text(card, wk, 106.0f, 30.0f, 600.0f, 54.0f, "PlayShell", 36, true, 1.0f);
	text(card, wk, 270.0f, 46.0f, 100.0f, 28.0f, "v" PLAYSHELL_VERSION, 18, false, 0.5f); // after the title, small
	text(card, wk, kCardW - 140.0f, 46.0f, 100.0f, 28.0f, "By AZiZ", 18, false, 0.5f);  // top right corner
	g_page2_icon = add_widget(g_box_class, card, wk, 48.0f, 86.0f, 34.0f, 34.0f); // page 2: the console's icon
	g_head = text(card, wk, 48.0f, 86.0f, 900.0f, 34.0f, "Choose a console", 21, false, 0.6f);
	panel(card, wk, 48.0f, 134.0f, kCardW - 96.0f, 2.0f, 0.2f, 1.0f); // divider
	// page 1: the console tiles - icon, name, number of games
	g_page1 = add_widget(wk, card, wk, 0.0f, 0.0f, kCardW, kCardH);
	const float tw = 180.0f, th = 158.0f, gap = 20.8f; // 6 a row in the card's 1184 px, 3 rows
	for (int t = 0; t < kTiles; ++t) {
		const float x = 48.0f + (t % kCols) * (tw + gap), y = 158.0f + (t / kCols) * (th + 22.0f);
		g_tile_bg[t]    = panel(g_page1, wk, x, y, tw, th, 0.12f, 1.0f, 18.0f);
		g_tile_icon[t]  = add_widget(g_box_class, g_page1, wk, x + (tw - 64.0f) / 2, y + 18.0f, 64.0f, 64.0f);
		g_tile_name[t]  = text(g_page1, wk, x + 10.0f, y + 92.0f, tw - 16.0f, 30.0f, "", 18, true, 1.0f);
		g_tile_count[t] = text(g_page1, wk, x + 16.0f, y + 122.0f, tw - 24.0f, 26.0f, "", 16, false, 0.6f);
	}
	// page 2: the games, full width
	g_page2 = add_widget(wk, card, wk, 0.0f, 0.0f, kCardW, kCardH);
	for (int i = 0; i < kRows; ++i) {
		const float y = 158.0f + i * 58.0f;
		g_row_bg[i]   = panel(g_page2, wk, 48.0f, y, kCardW - 96.0f, 50.0f, 0.13f, 1.0f, 12.0f);
		g_row_name[i] = text(g_page2, wk, 72.0f, y + 9.0f, kCardW - 360.0f, 34.0f, "", 22, false, 1.0f);
		g_row_tag[i]  = text(g_page2, wk, kCardW - 220.0f, y + 12.0f, 150.0f, 28.0f, "", 18, false, 0.6f);
	}
	g_empty = text(g_page2, wk, 48.0f, 170.0f, kCardW - 96.0f, 80.0f, "", 21, false, 0.7f);
	// the footer: the hints on their own bar
	panel(card, wk, 0.0f, kCardH - 76.0f, kCardW, 76.0f, 0.12f, 1.0f, 30.0f); // rounded like the card at the bottom
	panel(card, wk, 0.0f, kCardH - 76.0f, kCardW, 38.0f, 0.12f, 1.0f);        // square at the top
	static const char* const icon[3] = {"cross", "circle", "r3"};
	float x = 48.0f;
	for (int i = 0; i < 3; ++i) {
		Obj ib  = add_widget(g_box_class, card, wk, x, kCardH - 58.0f, 38.0f, 38.0f);
		Obj pic = pic_asset(icon[i]);
		if (ib != nullptr && pic != nullptr) {
			property_set(g_box_class, ib, "Image", pic);
		}
		g_hint_word[i] = text(card, wk, x + 48.0f, kCardH - 54.0f, 170.0f, 32.0f, i == 2 ? "Hold  Menu" : "", 21, false, 0.88f);
		x += i == 1 ? 210.0f : 180.0f;
	}
	g_hint_more = text(card, wk, 640.0f, kCardH - 54.0f, kCardW - 680.0f, 32.0f, "", 21, false, 0.88f);
	bool off = false;
	prop_set_up(m, "Visible", &off);
	g_menu = m;
	wp_log("retro: menu ready (panel %s.%s)", mono_class_get_name(g_panel_class), g_panel_prop);
}

void play_game(int id, bool resume); // (below)

// The chosen console's games (ids in list order).
int con_games(int con, int* ids) {
	const int total = g_gl_total.load();
	int       k     = 0;
	for (int id = 0; id < total; ++id) {
		if (g_gl_con[id] == con) {
			ids[k++] = id;
		}
	}
	return k;
}

void menu_render() {
	bool on1 = g_page == 1, on2 = g_page == 2;
	prop_set_up(g_page1, "Visible", &on1);
	prop_set_up(g_page2, "Visible", &on2);
	prop_set_up(g_page2_icon, "Visible", &on2);
	set_text(g_hint_word[0], g_ask >= 0 ? "Continue" : g_page == 1 ? "Open" : "Play", 0.88f);
	set_text(g_hint_word[1], g_ask >= 0 ? "Cancel" : g_page == 1 ? "Close" : "Back", 0.88f);
	if (g_hint_more != nullptr) { // a game loaded: its save state buttons; the question: Square starts over
		set_text(g_hint_more, g_ask >= 0 ? "Square  Start over"
		                      : PLAYSHELL_SAVE_STATES && g_gl_cur.load() >= 0 ? "Square  Save state      Triangle  Load state"
		                      : g_page == 2 ? "L1 / R1  Jump by letter" : "",
		         0.88f);
	}
	if (g_page == 1) {
		// the tile rows scroll with the chosen console
		const int row = g_con / kCols;
		if (row < g_con_top) {
			g_con_top = row;
		} else if (row >= g_con_top + kTileRows) {
			g_con_top = row - kTileRows + 1;
		}
		for (int t = 0; t < kTiles; ++t) {
			const int pos = g_con_top * kCols + t;
			bool      vis = pos < kShown;
			prop_set_up(g_tile_bg[t], "Visible", &vis);
			prop_set_up(g_tile_icon[t], "Visible", &vis);
			prop_set_up(g_tile_name[t], "Visible", &vis);
			prop_set_up(g_tile_count[t], "Visible", &vis);
			if (!vis) {
				continue;
			}
			const int  c = kConOrder[pos];
			static int tmp[kMaxGames];
			const int  k = con_games(c, tmp);
			char       n[24];
			snprintf(n, sizeof(n), "%d game%s", k, k == 1 ? "" : "s");
			const bool chosen = pos == g_con;
			tint(g_tile_bg[t], chosen ? 0.32f : 0.12f, 1.0f);
			static void* const kFontName = ui_font(18, true), *const kFontLong = ui_font(15, true); // (made once)
			const bool         is_long   = strlen(kConName[c]) > 14; // "Game Boy Advance": the smaller one, so it fits
			if (void* f = is_long ? kFontLong : kFontName) {
				prop_set_up(g_tile_name[t], "Font", f);
			}
			set_text(g_tile_name[t], kConName[c], chosen ? 1.0f : 0.8f);
			set_text(g_tile_count[t], n, chosen ? 0.85f : 0.55f);
			if (Obj pic = pic_asset(kConIcon[c])) {
				prop_set_up(g_tile_icon[t], "Image", pic);
			}
		}
		const float hx = 48.0f;
		prop_set_up(g_head, "X", const_cast<float*>(&hx));
		set_text(g_head, "Choose a console", 0.6f);
		return;
	}
	// page 2: the chosen console's games
	static int ids[kMaxGames];
	const int  c = kConOrder[g_con], total = con_games(c, ids), cur = g_gl_cur.load();
	g_sel = total > 0 ? std::max(0, std::min(g_sel, total - 1)) : 0;
	if (g_sel < g_top) {
		g_top = g_sel;
	} else if (g_sel >= g_top + kRows) {
		g_top = g_sel - kRows + 1;
	}
	if (Obj pic = pic_asset(kConIcon[c])) {
		prop_set_up(g_page2_icon, "Image", pic);
	}
	const float hx = 94.0f; // after the console's icon
	prop_set_up(g_head, "X", const_cast<float*>(&hx));
	char head[160];
	if (g_ask >= 0) {
		snprintf(head, sizeof(head), "%s  -  continue where you left off?", g_gl_name[g_ask]);
	} else {
		snprintf(head, sizeof(head), "%s  -  %d game%s", kConName[c], total, total == 1 ? "" : "s");
	}
	set_text(g_head, head, 0.75f);
	static const char* const kFolder[kCons] = {"gb", "gbc", "nes", "gg", "ps1", "snes", "gba", "psp", "nds", "dos", "n64", "pce", "ws", "3ds"};
	char empty[160];
	snprintf(empty, sizeof(empty), "No games yet. Put them in /data/PlayShell/roms/%s", kFolder[c]);
	set_text(g_empty, empty, 0.7f);
	bool show_empty = total == 0;
	prop_set_up(g_empty, "Visible", &show_empty);
	for (int i = 0; i < kRows; ++i) {
		const int row = g_top + i;
		bool      vis = row < total;
		prop_set_up(g_row_bg[i], "Visible", &vis);
		prop_set_up(g_row_name[i], "Visible", &vis);
		prop_set_up(g_row_tag[i], "Visible", &vis);
		if (!vis) {
			continue;
		}
		const int  id = ids[row];
		const bool on = row == g_sel;
		tint(g_row_bg[i], on ? 1.0f : 0.13f, 1.0f);
		set_text(g_row_name[i], g_gl_name[id], on ? 0.0f : 1.0f);
		set_text(g_row_tag[i], id == cur ? "Playing" : PLAYSHELL_SAVE_STATES && g_gl_auto[id] ? "Saved" : "", on ? 0.3f : 0.6f);
	}
}

// From the frame tick: open / close, the presses, the list.
void retro_menu_tick() {
	static int shown = 0;
	const int  mode  = g_mode.load();
	if (g_menu == nullptr) {
		return;
	}
	if ((mode == 1) != (shown == 1)) {
		shown = mode == 1;
		if (shown) { // opens on the consoles, the first one chosen - a game loaded: on its console's games, on it
			g_page = 1;
			g_ask  = -1;
			g_con = g_con_top = 0;
			g_sel = g_top = 0;
			const int cur = g_gl_cur.load();
			for (int pos = 0; cur >= 0 && cur < kMaxGames && pos < kShown; ++pos) {
				if (kConOrder[pos] == g_gl_con[cur]) {
					g_con = pos;
					g_con_top = std::max(0, pos / kCols - kTileRows + 1);
					g_page    = 2;
					static int ids[kMaxGames];
					const int  k = con_games(kConOrder[pos], ids);
					for (int i = 0; i < k; ++i) {
						if (ids[i] == cur) {
							g_sel = i;
						}
					}
				}
			}
			menu_render();
		}
		show_on_home(g_menu, shown == 1);
		if (shown == 1) {
			fade_in(g_menu, 9); // (~0.15 s)
		}
	}
	if (mode != 1) {
		g_menu_keys.store(0);
		return;
	}
	const uint32_t keys = g_menu_keys.exchange(0);
	bool           redo = g_gl_new.exchange(0) != 0;
	if (g_ask >= 0) { // the question: Cross continue, Square start over, Circle back to the list
		if (keys & (16 | 64)) {
			const int id = g_ask;
			g_ask        = -1;
			play_game(id, (keys & 16) != 0);
			return;
		}
		if (keys & 32) {
			g_ask = -1;
			redo  = true;
		}
		if (redo) {
			menu_render();
		}
		return;
	}
	if (PLAYSHELL_SAVE_STATES && (keys & (64 | 128)) && g_gl_cur.load() >= 0) { // a game loaded: Square saves its state, Triangle loads it
		if (g_joy_sock < 0) {
			joy_send('J', 0xFF);
		}
		const char cmd = (keys & 64) ? 'S' : 'T';
		(void)sendto(g_joy_sock, &cmd, 1, 0, reinterpret_cast<sockaddr*>(&g_joy_to), sizeof(g_joy_to));
		wp_log("retro: %s state", cmd == 'S' ? "save" : "load");
		if (cmd == 'T') { // loaded: back into the game
			g_mode.store(2);
			g_game_mode = true;
		}
		return;
	}
	if (g_page == 1) { // the consoles: left / right one, up / down a row; Cross opens it, Circle closes the menu
		if (keys & 4) { if (g_con > 0) --g_con; redo = true; }
		if (keys & 8) { if (g_con < kShown - 1) ++g_con; redo = true; }
		if (keys & 1) { if (g_con >= kCols) g_con -= kCols; redo = true; }
		if (keys & 2) { if (g_con + kCols < kShown) g_con += kCols; else if (g_con / kCols < (kShown - 1) / kCols) g_con = kShown - 1; redo = true; }
		if (keys & 32) {
			g_mode.store(0);
			return;
		}
		if (keys & 16) {
			g_page = 2;
			g_sel = g_top = 0;
			static int ids[kMaxGames];
			const int  cur = g_gl_cur.load(), k = con_games(kConOrder[g_con], ids);
			for (int i = 0; i < k; ++i) { // on the game you were playing, if it is this console's
				if (ids[i] == cur) {
					g_sel = i;
				}
			}
			redo = true;
		}
		if (redo) {
			menu_render();
		}
		return;
	}
	// page 2: the games - up / down, Cross plays, Circle back to the consoles
	static int ids[kMaxGames];
	const int  total = con_games(kConOrder[g_con], ids);
	if (keys & 1) { --g_sel; redo = true; }
	if (keys & 2) { ++g_sel; redo = true; }
	if (keys & 4) { g_sel -= kRows; redo = true; } // left / right: a page of games
	if (keys & 8) { g_sel += kRows; redo = true; }
	if ((keys & (256 | 512)) && total > 0) { // L1 / R1: the previous / next first letter ("#": numbers, signs)
		auto letter = [&](int row) {
			const int c = toupper(static_cast<unsigned char>(g_gl_name[ids[row]][0]));
			return isalpha(c) ? c : '#';
		};
		int r = std::max(0, std::min(g_sel, total - 1));
		const int at = letter(r);
		if (keys & 512) {
			while (r < total && letter(r) == at) {
				++r;
			}
			g_sel = r < total ? r : g_sel;
		} else {
			while (r > 0 && letter(r - 1) == at) {
				--r;
			}
			if (r == std::max(0, std::min(g_sel, total - 1)) && r > 0) { // already its first: the letter before
				const int before = letter(r - 1);
				while (r > 0 && letter(r - 1) == before) {
					--r;
				}
			}
			g_sel = r;
		}
		redo = true;
	}
	if (keys & 32) {
		g_page = 1;
		menu_render();
		return;
	}
	if ((keys & 16) && total > 0) { // Cross: play it (a Continue state, not the game loaded: ask first)
		const int id = ids[std::max(0, std::min(g_sel, total - 1))];
		if (PLAYSHELL_SAVE_STATES && g_gl_auto[id] && id != g_gl_cur.load()) {
			g_ask = id;
			menu_render();
			return;
		}
		play_game(id, false);
		return;
	}
	if (redo) {
		menu_render();
	}
}

// Plays a game ("C": from its Continue state, "P": from its start; the game loaded: it goes on).
void play_game(int id, bool resume) {
	{
		char cmd[16];
		const int k  = snprintf(cmd, sizeof(cmd), "%c%d", resume ? 'C' : 'P', id);
		if (g_joy_sock < 0) {
			joy_send('J', 0xFF);
		}
		(void)sendto(g_joy_sock, cmd, static_cast<size_t>(k), 0, reinterpret_cast<sockaddr*>(&g_joy_to), sizeof(g_joy_to));
		g_mode.store(2);
		g_game_mode = true;
		// until this game's first frame: no picture, no bezel (before, the last game's picture and bezel size showed
		// for a moment, then jumped - the box flickered at the start); frames still waiting are the old game's
		pthread_mutex_lock(&g_q_lock);
		g_q_n = 0;
		pthread_mutex_unlock(&g_q_lock);
		g_await = true;
		bool off = false;
		if (g_pop_img != nullptr) {
			prop_set_up(g_pop_img, "Visible", &off);
		}
		if (g_bezel != nullptr) {
			prop_set_up(g_bezel, "Visible", &off);
		}
		wp_log("retro: play %s%s", g_gl_name[id], resume ? " (continue)" : "");
		snprintf(g_loading, sizeof(g_loading), "%s", g_gl_name[id]);
	}
}

// From the frame tick (the UI thread): the pop-up follows the controller mode. Shown: (again) the last child of
// the home screen's scene, so it is on top of whatever the home screen added since.
void retro_popup_tick(bool playing) {
	if (g_rf_title_new.exchange(0) && g_card_title != nullptr) {
		prop_set_up(g_card_title, "Text", mono_string_new(g_domain, g_rf_title));
	}
	if (g_card_until != 0 && g_tick >= g_card_until) { // the card's 6 s are over
		g_card_until = 0;
		show_on_home(g_card, false);
	}
	if (g_spin != nullptr) { // loading: the spinner turns until the game's first frame
		static bool spin_on = false;
		const bool  want    = g_await && playing;
		if (want != spin_on) { // shown and turning while loading; stopped and hidden at the game's first frame
			spin_on = want;
			prop_set_up(g_spin, "Visible", &spin_on);
			if (want) {
				invoke(mono_class_get_method_from_name(g_spin_class, "Start", 0), g_spin, nullptr, "BusyIndicator.Start");
			} else {
				bool  quick   = false;
				void* sa[1]   = {&quick};
				invoke(mono_class_get_method_from_name(g_spin_class, "Stop", 1), g_spin, sa, "BusyIndicator.Stop");
			}
			if (g_spin_name == nullptr && g_pop_box != nullptr && g_label_class != nullptr) { // (made once: the labels'
				g_spin_name = text(g_pop_box, g_pop_wk, 0.0f, 600.0f, 1200.0f, 40.0f, "", 24, false, 0.9f); // class known)
			}
			if (g_spin_name != nullptr) {
				if (want) { // the name, centred under the spinner (a label is left-aligned: placed by its length)
					char name[64];
					snprintf(name, sizeof(name), "%s", g_loading);
					const float w = static_cast<float>(strlen(name)) * 12.0f;
					const float x = std::max(40.0f, 960.0f - w / 2.0f);
					prop_set_up(g_spin_name, "X", const_cast<float*>(&x));
					set_text(g_spin_name, name, 0.9f);
					fade_in(g_spin_name, 12);
				}
				prop_set_up(g_spin_name, "Visible", &spin_on);
			}
		}
	}
	if (g_pop_box == nullptr || playing == g_pop_on) {
		return;
	}
	g_pop_on = playing;
	if (playing) { // playing: no card
		g_card_until = 0;
		show_on_home(g_card, false);
	}
	if (playing) {
		Obj pui  = open_image("/system_ex/common_ex/lib/Sce.PlayStation.PUI.dll");
		Obj root = pui != nullptr ? host_root(pui) : nullptr;
		if (root != nullptr) {
			Obj   rm      = find_method_up(g_pop_wk, "RemoveFromParent", 0);
			Obj   parent  = prop_up(g_pop_box, "Parent");
			void* a[1]    = {g_pop_box};
			if (parent != nullptr && rm != nullptr) {
				invoke(rm, g_pop_box, nullptr, "RemoveFromParent(pop)");
			}
			invoke(find_method_up(g_pop_wk, "AppendChild", 1), root, a, "AppendChild(pop)");
			g_pop_host = root;
		}
		wp_log("retro: pop-up on the home screen's scene (%s)", root != nullptr ? "found" : "NOT found");
	}
	bool vis = playing;
	const bool ok = prop_set_up(g_pop_box, "Visible", &vis);
	if (playing) {
		fade_in(g_pop_box, 12); // (~0.2 s)
	}
	wp_log("retro: pop-up %s (%s)", playing ? "shown" : "hidden", ok ? "ok" : "FAILED");
}

// ---- the game's sound, played from inside the home screen (the PS5 lets the home screen be heard) ----
// the PS5's audio out, looked up in the home screen's own libSceAudioOut (no link: normal builds stay as they are)
int (*sceAudioOutInit)(void)                                                                         = nullptr;
int (*sceAudioOutOpen)(int32_t user_id, int32_t type, int32_t index, uint32_t len, uint32_t freq, uint32_t param) = nullptr;
int (*sceAudioOutOutput)(int32_t handle, const void* buf)                                            = nullptr;

void* retro_sound_thread(void*) {
	const int   s = socket(AF_INET, SOCK_DGRAM, 0);
	sockaddr_in a {};
	a.sin_family      = AF_INET;
	a.sin_port        = htons(9055);
	a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
	int big = 256 * 1024;
	setsockopt(s, SOL_SOCKET, SO_RCVBUF, &big, sizeof(big));
	if (s < 0 || bind(s, reinterpret_cast<sockaddr*>(&a), sizeof(a)) != 0) {
		wp_log("retro: sound: cannot listen on 9055");
		return nullptr;
	}
	// Right after the console starts, the home screen has not loaded its sound library yet (and cannot be made to: the
	// load below answers "not found") - PlayShell loaded early had no sound at all. So: look again every second until
	// the home screen has it.
	int lib = 0;
	for (int waited = 0;; ++waited) {
		const uintptr_t cred = set_ucred_to_ptrace();
		lib                  = get_module_handle(getpid(), "libSceAudioOut.sprx");
		if (cred != 0) {
			set_proc_authid(getpid(), cred);
		}
		if (lib > 0) {
			if (waited > 0) {
				wp_log("retro: sound: the home screen loaded its sound library after %d s", waited);
			}
			break;
		}
		if (waited == 0) {
			wp_log("retro: sound: the home screen's sound library is not loaded yet - waiting for it");
		}
		if (waited == 10) {
			break; // (10 s: the load below once more, then waiting on)
		}
		sleep(1);
	}
	// looking up / loading modules in the home screen needs the raised authid (as for Mono's functions)
	const uintptr_t old = set_ucred_to_ptrace();
	if (lib <= 0) { // not loaded in the home screen: load it (as the payload loads system libraries for the tile)
		const int kh   = get_module_handle(getpid(), "libkernel_sys.sprx");
		using LoadFn   = int (*)(const char*, size_t, const void*, uint32_t, const void*, int*);
		LoadFn    load = kh > 0 ? reinterpret_cast<LoadFn>(kernel_dynlib_dlsym(-1, static_cast<uint32_t>(kh), "sceKernelLoadStartModule")) : nullptr;
		for (const char* root : {"/system/common/lib/", "/system/priv/lib/", "/system_ex/common_ex/lib/"}) {
			if (load == nullptr || lib > 0) {
				break;
			}
			char path[96];
			snprintf(path, sizeof(path), "%slibSceAudioOut.sprx", root);
			int       res = 0;
			const int id  = load(path, 0, nullptr, 0, nullptr, &res);
			wp_log("retro: sound: load %s -> %#x (res %d)", path, static_cast<unsigned>(id), res);
			if (id >= 0) {
				lib = get_module_handle(getpid(), "libSceAudioOut.sprx");
				lib = lib > 0 ? lib : id;
			}
		}
	}
	bool found = lib > 0 && resolve(lib, sceAudioOutInit, "sceAudioOutInit") && resolve(lib, sceAudioOutOpen, "sceAudioOutOpen") &&
	                   resolve(lib, sceAudioOutOutput, "sceAudioOutOutput");
	if (old != 0) {
		set_proc_authid(getpid(), old);
	}
	if (!found) {
		wp_log("retro: sound: libSceAudioOut not found in the home screen (handle %d) - waiting on", lib);
		for (int waited = 11; lib <= 0; ++waited) { // until the home screen loads it (it does once it plays a sound)
			sleep(1);
			const uintptr_t cred = set_ucred_to_ptrace();
			lib                  = get_module_handle(getpid(), "libSceAudioOut.sprx");
			if (lib > 0) {
				found = resolve(lib, sceAudioOutInit, "sceAudioOutInit") && resolve(lib, sceAudioOutOpen, "sceAudioOutOpen") &&
				        resolve(lib, sceAudioOutOutput, "sceAudioOutOutput");
				wp_log("retro: sound: the home screen loaded its sound library after %d s", waited);
			}
			if (cred != 0) {
				set_proc_authid(getpid(), cred);
			}
		}
		if (!found) {
			wp_log("retro: sound: libSceAudioOut functions not found (handle %d)", lib);
			return nullptr;
		}
	}
	wp_log("retro: sound: libSceAudioOut handle %d", lib);
	const int init = sceAudioOutInit();
	const int port = sceAudioOutOpen(0xff, 0, 0, 256, 48000, 1); // system user, main port, S16 stereo, 48 kHz
	wp_log("retro: sound: init %#x, port %#x (in the home screen)", init, port);
	if (port < 0) {
		return nullptr;
	}
	constexpr unsigned kRing = 16384; // stereo frames (~0.34 s)
	static int16_t     ring[kRing * 2];
	static int16_t     grain[256 * 2];
	static uint8_t     pkt[1 + 1024 * 4];
	unsigned           w = 0, r = 0;
	long               last_audio = 0;
	for (;;) {
		ssize_t n;
		while ((n = recv(s, pkt, sizeof(pkt), MSG_DONTWAIT)) > 1) {
			if (pkt[0] != 'A') {
				continue;
			}
			const int16_t* smp    = reinterpret_cast<const int16_t*>(pkt + 1);
			const unsigned frames = static_cast<unsigned>(n - 1) / 4;
			for (unsigned i = 0; i < frames; ++i) {
				if (w - r >= kRing) {
					++r; // full: the oldest goes
				}
				ring[(w % kRing) * 2]     = smp[i * 2];
				ring[(w % kRing) * 2 + 1] = smp[i * 2 + 1];
				++w;
			}
			last_audio = now_ms();
		}
		// a little in hand before playing (no crackle at the start); paused / no game: silence
		const bool play = g_mode.load() == 2 && now_ms() - last_audio < 200 && w - r >= 256;
		for (int i = 0; i < 256; ++i) {
			if (play && r != w) {
				grain[i * 2]     = ring[(r % kRing) * 2];
				grain[i * 2 + 1] = ring[(r % kRing) * 2 + 1];
				++r;
			} else {
				grain[i * 2] = grain[i * 2 + 1] = 0;
			}
		}
		if (g_mode.load() != 2) {
			r = w; // nothing old plays when it goes on
		}
		if (sceAudioOutOutput(port, grain) < 0) { // waits for the previous grain: it sets the pace
			usleep(5000);
		}
	}
	return nullptr;
}

bool retro_setup() {
	void* lib = nullptr;
	(void)lib;
	Obj core = open_image("/system_ex/common_ex/lib/Sce.PlayStation.Core.dll");
	Obj pui  = open_image("/system_ex/common_ex/lib/Sce.PlayStation.PUI.dll");
	Obj tk   = core != nullptr ? mono_class_from_name(core, "Sce.PlayStation.Core.Graphics", "Texture2D") : nullptr;
	Obj ak   = pui != nullptr ? mono_class_from_name(pui, "Sce.PlayStation.PUI", "ImageAsset") : nullptr;
	Obj bk   = pui != nullptr ? mono_class_from_name(pui, "Sce.PlayStation.PUI.UI3", "ImageBox") : nullptr;
	if (tk == nullptr || ak == nullptr || bk == nullptr || mono_array_new == nullptr || mono_get_byte_class == nullptr) {
		wp_log("retro: classes tex %p asset %p box %p, array_new %p", tk, ak, bk, reinterpret_cast<void*>(mono_array_new));
		return false;
	}
	// texture: new Texture2D(160, 144, false, PixelFormat.Rgba)
	Obj tex  = mono_object_new(g_domain, tk);
	Obj tctr = mono_class_get_method_from_name(tk, ".ctor", 4);
	int w = kRw, h = kRh, fmt = 1; // PixelFormat.Rgba
	bool no = false;
	void* targs[4] = {&w, &h, &no, &fmt};
	if (tex == nullptr || tctr == nullptr || !invoke(tctr, tex, targs, "Texture2D(160,144,Rgba)")) {
		return false;
	}
	g_rt_set = method_where(tk, "SetPixels", 2, 1, "System.Array");
	g_rt_arr = mono_array_new(g_domain, mono_get_byte_class(), static_cast<uintptr_t>(kRw * kRh * 4));
	// asset + image box on the wallpaper layer
	Obj actr  = method_where(ak, ".ctor", 1, 0, "Texture");
	Obj asset = mono_object_new(g_domain, ak);
	void* aargs[1] = {tex};
	if (g_rt_set == nullptr || g_rt_arr == nullptr || actr == nullptr || asset == nullptr || !invoke(actr, asset, aargs, "ImageAsset(Texture)")) {
		wp_log("retro: SetPixels %p array %p asset ctor %p", g_rt_set, g_rt_arr, actr);
		return false;
	}
	Obj box  = mono_object_new(g_domain, bk);
	Obj bctr = mono_class_get_method_from_name(bk, ".ctor", 0);
	if (box == nullptr || bctr == nullptr || !invoke(bctr, box, nullptr, "ImageBox()")) {
		return false;
	}
	mono_gchandle_new(tex, 1);
	mono_gchandle_new(g_rt_arr, 1);
	mono_gchandle_new(asset, 1);
	g_rt_pic[0]   = tex;
	g_rt_asset[0] = asset;
	for (int i = 1; i < kRtPics; ++i) { // the other pictures (if one cannot be made: fewer, as before)
		Obj   t = mono_object_new(g_domain, tk);
		Obj   a = t != nullptr ? mono_object_new(g_domain, ak) : nullptr;
		void* ta[1] = {t};
		if (a == nullptr || !invoke(tctr, t, targs, "Texture2D(more)") || !invoke(actr, a, ta, "ImageAsset(more)")) {
			break;
		}
		mono_gchandle_new(t, 1);
		mono_gchandle_new(a, 1);
		g_rt_pic[i]   = t;
		g_rt_asset[i] = a;
		g_rt_npic     = i + 1;
	}
	wp_log("retro: %d pictures", g_rt_npic);
	g_tk = tk, g_ak = ak, g_tctr = tctr, g_actr = actr;
	g_sets[0].w = kRw, g_sets[0].h = kRh, g_sets[0].n = g_rt_npic, g_sets[0].arr = g_rt_arr;
	for (int i = 0; i < g_rt_npic; ++i) {
		g_sets[0].pic[i] = g_rt_pic[i], g_sets[0].asset[i] = g_rt_asset[i];
	}
	g_nsets = 1;
	g_set   = &g_sets[0];
	mono_gchandle_new(box, 1);
	property_set(bk, box, "Image", asset);
	// the wallpaper's own layers (the dim layer sits above the video; else the scene root / boot root)
	g_ui3_widget  = mono_class_from_name(pui, "Sce.PlayStation.PUI.UI3", "Widget");
	g_color_class = mono_class_from_name(pui, "Sce.PlayStation.PUI", "UIColor");
	Obj wk        = g_ui3_widget;
	find_panel_class(pui);
	Obj parent    = home_scene_root(pui);
	float x = 1550.0f, y = 740.0f, bw = 320.0f, bh = 288.0f; // the mini screen: bottom right, 2x (the game paused)
	property_set(wk, box, "X", &x);
	property_set(wk, box, "Y", &y);
	property_set(wk, box, "Width", &bw);
	property_set(wk, box, "Height", &bh);
	void* cargs[1] = {box};
	(void)cargs;
	// the menu and the pop-up go on the home screen's own scene (not the wallpaper's layer): PlayShell works with
	// the wallpaper off too
	const bool ok = parent != nullptr && wk != nullptr && g_color_class != nullptr;
	g_rt_tex = ok ? tex : nullptr; // not ready: retro_tick tries again in 5 s
	g_rt_box = box;
	wp_log("retro: PlayShell %s", ok ? "ready" : "NOT ready (no home screen scene yet: tried again later)");
	pthread_t rt, st;
	if (ok && pthread_create(&rt, nullptr, retro_rx_thread, nullptr) == 0) {
		pthread_detach(rt);
	}
	if (ok && pthread_create(&st, nullptr, retro_sound_thread, nullptr) == 0) {
		pthread_detach(st);
	}
	if (ok) {
		retro_popup_setup(pui, asset, wk);
		retro_menu_setup(pui, wk);
		detour_method("Sce.PlayStation.Core", "GamePad", "GetData", 1, reinterpret_cast<void*>(&GetData_Hook),
		              reinterpret_cast<void**>(&g_getdata_orig));
	}
	return ok;
}

PicSet* pic_set(int w, int h) {
	for (int i = 0; i < g_nsets; ++i) {
		if (g_sets[i].w == w && g_sets[i].h == h) {
			return &g_sets[i];
		}
	}
	if (g_nsets >= 6 || g_tk == nullptr) {
		return nullptr;
	}
	PicSet set;
	set.w = w, set.h = h;
	set.arr = mono_array_new(g_domain, mono_get_byte_class(), static_cast<uintptr_t>(w) * h * 4);
	if (set.arr == nullptr) {
		return nullptr;
	}
	mono_gchandle_new(set.arr, 1);
	int   fmt = 1; // PixelFormat.Rgba
	bool  no  = false;
	void* targs[4] = {&w, &h, &no, &fmt};
	for (int i = 0; i < 2; ++i) {
		Obj   t     = mono_object_new(g_domain, g_tk);
		Obj   a     = t != nullptr ? mono_object_new(g_domain, g_ak) : nullptr;
		void* ta[1] = {t};
		if (a == nullptr || !invoke(g_tctr, t, targs, "Texture2D(size)") || !invoke(g_actr, a, ta, "ImageAsset(size)")) {
			break;
		}
		mono_gchandle_new(t, 1);
		mono_gchandle_new(a, 1);
		set.pic[i]   = t;
		set.asset[i] = a;
		set.n        = i + 1;
		if (w >= 480) { // big pictures (PS1 2x, PSP): smooth scaling to the screen; small ones (Game Boy, NES) stay crisp
			static Obj filter = nullptr;
			if (filter == nullptr) {
				filter = find_method_up(g_tk, "SetFilter", 3);
			}
			uint8_t mag = 1, min = 1, mip = 0; // TextureFilterMode: Nearest 0, Linear 1
			void*   fa[3] = {&mag, &min, &mip};
			const bool ok = filter != nullptr && invoke(filter, t, fa, "Texture.SetFilter");
			if (i == 0) {
				wp_log("retro: smooth scaling for %d x %d: %s", w, h, ok ? "on" : "not available");
			}
		}
	}
	if (set.n == 0) {
		return nullptr;
	}
	wp_log("retro: pictures for %d x %d", w, h);
	g_sets[g_nsets] = set;
	return &g_sets[g_nsets++];
}

// The game in the pop-up: as big as fits 1600 x 864, in the middle of the screen - the height decides for the 4:3 and
// square consoles (PS1 1152 x 864, Game Boy 960 x 864), wide ones get the width (PSP 1536 x 864, GBA 1296 x 864); the
// bezel made for that screen size around it (tools/make_bezels.py: its 60 px frame, the same fit).
void place_game(int w, int h) {
	if (g_pop_img == nullptr) {
		return;
	}
	g_game_w = w, g_game_h = h;
	if (g_black != nullptr) {
		prop_set_up(g_black, "Visible", &g_full);
	}
	if (g_full) { // fullscreen: as big as the screen (keeping the console's shape), black around it, no bezel
		const float k  = std::min(1920.0f / w, 1080.0f / h);
		float       gw = std::round(w * k), gh = std::round(h * k), x = std::round(960.0f - gw / 2), y = std::round(540.0f - gh / 2);
		prop_set_up(g_pop_img, "X", &x);
		prop_set_up(g_pop_img, "Y", &y);
		prop_set_up(g_pop_img, "Width", &gw);
		prop_set_up(g_pop_img, "Height", &gh);
		bool off = false;
		if (g_bezel != nullptr) {
			prop_set_up(g_bezel, "Visible", &off);
		}
		return;
	}
	const float k = std::min(1600.0f / w, 864.0f / h);
	float       gw = std::round(w * k), gh = std::round(h * k), x = std::round(960.0f - gw / 2), y = std::round(540.0f - gh / 2);
	prop_set_up(g_pop_img, "X", &x);
	prop_set_up(g_pop_img, "Y", &y);
	prop_set_up(g_pop_img, "Width", &gw);
	prop_set_up(g_pop_img, "Height", &gh);
	char name[32];
	snprintf(name, sizeof(name), "bezel_%dx%d", w, h);
	Obj  pic = g_bezel != nullptr ? pic_asset(name) : nullptr;
	bool on  = pic != nullptr; // no bezel drawn for this size: none (better than one that does not fit)
	if (pic != nullptr) {
		float bx = x - 60.0f, by = y - 60.0f, bw = gw + 120.0f, bh = gh + 120.0f;
		prop_set_up(g_bezel, "Image", pic);
		prop_set_up(g_bezel, "X", &bx);
		prop_set_up(g_bezel, "Y", &by);
		prop_set_up(g_bezel, "Width", &bw);
		prop_set_up(g_bezel, "Height", &bh);
	}
	if (g_bezel != nullptr) {
		prop_set_up(g_bezel, "Visible", &on);
	}
}

// Every frame: the payload's newest frame into the picture not on screen; shown at the next refresh.
void retro_tick() {
	if (g_rt_fail > 3) {
		return;
	}
	if (g_rt_tex == nullptr) {
		if (g_rt_start == 0) {
			g_rt_start = g_tick + 240; // ~4 s after the module started (the home screen is up)
		}
		if (g_tick >= g_rt_start && !retro_setup()) {
			++g_rt_fail;
			g_rt_start = g_tick + 300;
		}
		return;
	}
	g_game_mode = g_mode.load() == 2;
	retro_menu_tick();
	retro_popup_tick(g_game_mode);
	fade_tick();
	if ((g_full_flip.exchange(0) & 1) && g_game_mode) { // touchpad: fullscreen on / off
		g_full = !g_full;
		if (g_game_w > 0) {
			place_game(g_game_w, g_game_h);
		}
		wp_log("retro: fullscreen %s", g_full ? "on" : "off");
	}
	// the oldest waiting frame (one per refresh)
	static uint16_t frame[kMaxPx];
	int             fw = 0, fh = 0;
	pthread_mutex_lock(&g_q_lock);
	if (g_q_n > 0) {
		const QFrame& q = g_q[g_q_head];
		fw = q.w, fh = q.h;
		memcpy(frame, q.px, static_cast<size_t>(fw) * fh * 2);
		g_q_head = (g_q_head + 1) % kQueue;
		--g_q_n;
		// more than one still waiting: the emulator is ahead (NES 60.1 fps vs 60) - drop the extra, so the queue
		// never builds up into delay (it only covers two frames coming in one refresh)
		while (g_q_n > 1) {
			g_q_head = (g_q_head + 1) % kQueue;
			--g_q_n;
		}
	}
	pthread_mutex_unlock(&g_q_lock);
	if (fw == 0) {
		return; // no new frame from the payload yet
	}
	const bool first = g_await; // a game just started: this is its first frame
	static int on = 0, pending = -1;
	if (g_set == nullptr || fw != g_set->w || fh != g_set->h) { // another console: its own size
		PicSet* set = fw > 0 && fh > 0 ? pic_set(fw, fh) : nullptr;
		if (set == nullptr) {
			return;
		}
		g_set   = set;
		on      = 0;
		pending = -1;
		place_game(fw, fh);
		if (g_pop_img != nullptr) {
			prop_set_up(g_pop_img, "Image", g_set->asset[0]);
		}
	}
	(void)pending;
	if (first) { // the new game's first frame: its size, its bezel, then the picture (below)
		g_await = false;
		place_game(fw, fh);
	}
	const int w   = (on + 1) % g_set->n; // the picture not on screen
	Obj       arr = g_set->arr;
	uint8_t*  px  = reinterpret_cast<uint8_t*>(static_cast<char*>(arr) + 32); // MonoArray data (64-bit)
	for (int i = 0; i < fw * fh; ++i) { // RGB565 -> RGBA
		const uint16_t v = frame[i];
		const int      r = v >> 11, g = (v >> 5) & 63, b = v & 31;
		px[i * 4]     = static_cast<uint8_t>((r << 3) | (r >> 2));
		px[i * 4 + 1] = static_cast<uint8_t>((g << 2) | (g >> 4));
		px[i * 4 + 2] = static_cast<uint8_t>((b << 3) | (b >> 2));
		px[i * 4 + 3] = 255;
	}
	const long t = now_ms();
	struct timespec a {}, b {};
	clock_gettime(CLOCK_MONOTONIC, &a);
	int        level   = 0;
	void*      args[2] = {&level, arr};
	Obj        exc     = nullptr;
	// two pictures: the frame goes into the one not on screen, then that one is shown - in this same refresh (the
	// wait for the next refresh was for the Game Boy's top flicker, which was the emulator's: 16 ms less delay)
	mono_runtime_invoke(g_rt_set, g_set->pic[w], args, &exc);
	if (exc == nullptr && g_set->n > 1 && g_pop_img != nullptr) {
		prop_set_up(g_pop_img, "Image", g_set->asset[w]);
		on = w;
		if (first) { // the game's first picture and its frame fade in (where the spinner was)
			bool vis = true;
			prop_set_up(g_pop_img, "Visible", &vis);
			fade_in(g_pop_img, 14);
			fade_in(g_bezel, 14);
		}
	}
	clock_gettime(CLOCK_MONOTONIC, &b);
	if (exc != nullptr) {
		log_exception("Texture2D.SetPixels", exc);
		g_rt_fail = 99;
		return;
	}
	g_rt_us += (b.tv_sec - a.tv_sec) * 1000000L + (b.tv_nsec - a.tv_nsec) / 1000;
	++g_rt_n;
	if (g_rt_t0 == 0) {
		g_rt_t0 = t;
	} else if (t - g_rt_t0 >= 2000) {
		static int logs = 0;
		if (logs++ < 30) {
			wp_log("retro: %ld frames in %ld ms (%.1f fps), SetPixels %ld us avg", g_rt_n, t - g_rt_t0, g_rt_n * 1000.0 / (t - g_rt_t0),
			       g_rt_us / g_rt_n);
		}
		g_rt_t0 = t, g_rt_n = 0, g_rt_us = 0;
	}
}

using UpdateFn = void (*)(Obj self);
UpdateFn g_update_orig = nullptr;
Obj      g_app         = nullptr; // PUI's Application (Update's this)

// The home screen ignores the controller while PlayShell's menu or a game is open (Application.SuspendInputEvents;
// only blocking GamePad.GetData left another path: the tiles moved behind the menu). Back: its pending presses
// dropped (CancelKeyEvents - the Circle that closed the menu), then ResumeInputEvents.
void home_input_tick() {
	static int  was = 0;
	const int   now = g_mode.load() != 0;
	if (now == was || g_app == nullptr) {
		return;
	}
	was        = now;
	Obj      k = mono_object_get_class(g_app);
	Obj      m = find_method_up(k, now ? "SuspendInputEvents" : "CancelKeyEvents", 0);
	const bool a = m != nullptr && invoke(m, g_app, nullptr, now ? "SuspendInputEvents" : "CancelKeyEvents");
	bool       b = true;
	if (!now) {
		Obj r = find_method_up(k, "ResumeInputEvents", 0);
		b     = r != nullptr && invoke(r, g_app, nullptr, "ResumeInputEvents");
	}
	wp_log("retro: home screen input %s (%s)", now ? "suspended" : "back", a && b ? "ok" : "not available");
}

// Every home screen frame, on its UI thread.
void Update_Hook(Obj self) {
	g_update_orig(self);
	++g_tick;
	g_app = self;
	retro_tick();
	home_input_tick();
}

} // namespace


extern "C" int main(int argc, char const* argv[]) {
	(void)argc;
	(void)argv;

	g_t0_ms         = now_ms();
	const pid_t pid = getpid();
	wp_log("PlayShell home screen module starting (pid %d)", static_cast<int>(pid));

	// The injector is still inside ShellUI for a few ms after starting this thread (breakpoint,
	// register restore, detach). Kernel reads cut by that stop hung the whole process (tests 2, 4, 5),
	// so touch nothing until it is long gone.
	usleep(1500 * 1000);
	wp_log("injector gone, starting");

	pthread_t hb;
	if (pthread_create(&hb, nullptr, heartbeat_thread, nullptr) == 0) {
		pthread_detach(hb);
	}

	// Raised authid for the whole setup, like OnionHEN's toolbox: symbol lookups and the detour's
	// code-page mprotect need it. Dropped once the hook is in.
	g_step.store("set_ucred_to_ptrace");
	const uintptr_t old_authid = set_ucred_to_ptrace();
	wp_log("authid raised (old %#lx)", static_cast<unsigned long>(old_authid));
	auto fail = [&](const char* text, int rc) {
		wp_log("setup failed: %s", text);
		if (old_authid != 0) {
			set_proc_authid(pid, old_authid);
		}
		notify(text);
		return rc;
	};

	if (!resolve_kernel()) {
		return fail("PlayShell could not start (kernel symbols missing)", 1);
	}
	if (!resolve_mono()) {
		return fail("PlayShell could not start (Mono symbols missing)", 1);
	}
	g_step.store("mono_get_root_domain / mono_thread_attach");
	g_domain = mono_get_root_domain();
	mono_thread_attach(g_domain);
	wp_log("Mono domain %p, thread attached", g_domain);

	g_step.store("mprotect probe");
	char probe[100];
	has_hv_bypass = sceKernelMprotect(probe, sizeof(probe), 0x7) == 0;
	wp_log("mprotect probe: has_hv_bypass=%d", has_hv_bypass ? 1 : 0);

	g_step.store("open PUI assembly");
	Obj pui_image = open_image("/system_ex/common_ex/lib/Sce.PlayStation.PUI.dll");
	wp_log("PUI image %p", pui_image);
	if (pui_image == nullptr) {
		return fail("PlayShell could not start (PUI not found)", 2);
	}
	g_step.store("compile Application.Update");
	const uint64_t update_addr =
	    get_method_address(pui_image, "Sce.PlayStation.PUI", "Application", "Update", 0);
	wp_log("Application.Update compiled at %#lx", static_cast<unsigned long>(update_addr));
	if (update_addr == 0) {
		return fail("PlayShell could not start (Update not found)", 3);
	}
	g_step.store("InstallDetour(Application.Update)");
	if (!InstallDetour(update_addr, reinterpret_cast<void*>(&Update_Hook),
	                   reinterpret_cast<void**>(&g_update_orig)) ||
	    g_update_orig == nullptr) {
		// Another tool (a HEN or toolbox) hooks the same Application.Update and, loaded first, its jump
		// sits at the start: a second detour there fails ("hook failed", v1.1 next to OnionHEN; issue #8).
		// Follow that jump and hook the function it leads to: PlayShell -> that tool -> Sony.
		g_update_orig = nullptr;
		const uint64_t page = update_addr & ~static_cast<uint64_t>(0x3fff);
		if (sceKernelMprotect != nullptr) {
			(void)sceKernelMprotect(reinterpret_cast<void*>(page), 0x8000, 0x7); // execute-only code: readable first
		}
		const uint8_t* b = reinterpret_cast<const uint8_t*>(update_addr);
		wp_log("Application.Update starts with %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x",
		       b[0], b[1], b[2], b[3], b[4], b[5], b[6], b[7], b[8], b[9], b[10], b[11], b[12], b[13]);
		// The jump's target lives in that tool's own memory (another ELF), execute-only too: it must be made
		// readable before the detour can copy its first bytes (after rest mode OnionHEN is back first and the
		// chain failed: "no other hook to chain after"). Up to 3 jumps in a row (two tools chained).
		auto jump_target = [](uint64_t at) -> uint64_t {
			const uint8_t* c = reinterpret_cast<const uint8_t*>(at);
			uint64_t       t = 0;
			if (c[0] == 0xe9) { // jmp rel32
				int32_t rel = 0;
				memcpy(&rel, c + 1, 4);
				t = at + 5 + static_cast<int64_t>(rel);
			} else if (c[0] == 0xff && c[1] == 0x25 && c[2] == 0 && c[3] == 0 && c[4] == 0 && c[5] == 0) { // jmp [rip]
				memcpy(&t, c + 6, 8);
			} else if (c[0] == 0x48 && c[1] == 0xb8 && c[10] == 0xff && c[11] == 0xe0) { // mov rax, imm64; jmp rax
				memcpy(&t, c + 2, 8);
			}
			return t;
		};
		uint64_t next = jump_target(update_addr);
		// Best: the other tool's hook is "jmp [rip+0] <address>" at the start of Update. Point that address at
		// us and call the old one when we are done: Sony -> PlayShell -> that tool -> Sony. Only these 8 bytes
		// in Sony's page change (writable: we hook there anyway); the other tool's code is never touched (after
		// rest mode OnionHEN's toolbox is back first, and patching its code failed: "chaining ... failed").
		if (next != 0 && b[0] == 0xff && b[1] == 0x25 && b[2] == 0 && b[3] == 0 && b[4] == 0 && b[5] == 0) {
			g_update_orig       = reinterpret_cast<UpdateFn>(next);
			const uint64_t mine = reinterpret_cast<uint64_t>(&Update_Hook);
			memcpy(reinterpret_cast<void*>(update_addr + 6), &mine, sizeof(mine));
			uint64_t now = 0;
			memcpy(&now, reinterpret_cast<const void*>(update_addr + 6), sizeof(now));
			if (now == mine) {
				wp_log("Application.Update already hooked by another tool: its jump now leads to us first, then to it (%#lx)",
				       static_cast<unsigned long>(next));
				next = 0; // done
			} else {
				g_update_orig = nullptr;
			}
		}
		for (int depth = 0; next != 0 && g_update_orig == nullptr && depth < 3; ++depth) {
			if (sceKernelMprotect != nullptr) {
				(void)sceKernelMprotect(reinterpret_cast<void*>(next & ~static_cast<uint64_t>(0x3fff)), 0x8000, 0x7);
			}
			if (InstallDetour(next, reinterpret_cast<void*>(&Update_Hook), reinterpret_cast<void**>(&g_update_orig)) &&
			    g_update_orig != nullptr) {
				break;
			}
			g_update_orig = nullptr;
			const uint64_t further = jump_target(next);
			wp_log("Application.Update: chaining at %#lx failed, next jump %#lx", static_cast<unsigned long>(next),
			       static_cast<unsigned long>(further));
			next = further;
		}
		if (g_update_orig == nullptr) {
			wp_log("Application.Update: no other hook to chain after (next %#lx)", static_cast<unsigned long>(next));
			write_status("failed hook");
			return fail("PlayShell could not start (hook failed)", 4);
		}
		if (next != 0) {
			wp_log("Application.Update already hooked by another tool: chained after it at %#lx",
			       static_cast<unsigned long>(next));
		}
	}
	wp_log("PUI.Application.Update hooked");
	g_step.store("hooked, waiting for UI ticks");

	if (old_authid != 0) {
		set_proc_authid(pid, old_authid);
	}

	wp_log("init done");

	// Never return: the injected ELF (and the detour hook code in it) must stay alive, the same
	// way OnionHEN's toolbox parks its main thread.
	for (;;) {
		sleep(60);
	}
}
