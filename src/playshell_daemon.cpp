/*
 * playshell_daemon.cpp - PlayShell: games on the PS5 home screen, the payload the user loads.
 * Injects the home screen module (playshell_shellui.cpp, embedded) into SceShellUI, runs the emulator
 * (playshell_emu.c) and writes the log. Works on its own: no LiveShell needed. GPL-3.0; the injector and the
 * ShellUI handling come from LiveShell (same author), built on OnionHEN.
 *
 *   games:  /data/PlayShell/roms (and roms/gb, gbc, nes, gg, ps1; .zip / .7z too); PS1 BIOS /data/PlayShell/bios; Mesen 2 files in /data/PlayShell/mesen
 *   log:    /data/PlayShell/playshell.log (the home screen module's lines too)
 *   ports:  9053-9055 games (frames / controller / sound), 9056 module log, 9057 stop (one PlayShell at a time),
 *           9059 stop from the network (TCP: tools/stop_playshell.bat), 9060-9063 frame lanes
 */
#include <algorithm>
#include <atomic>
#include <cerrno>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <fcntl.h>
#include <mutex>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <pthread.h>
#include <setjmp.h>
#include <signal.h>
#include <sys/mman.h>
#include <ucontext.h>
#include <pthread_np.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>

#include "onion/proc_query.h"

#include <ps5/kernel.h>

extern "C" {
bool      Inject_Toolbox(int pid, uint8_t* elf);
int       pt_detach(pid_t pid, int sig);
uintptr_t set_ucred_to_ptrace(void);
uintptr_t set_proc_authid(pid_t pid, uintptr_t new_authid);
int       sceKernelTerminateProcess(int pid, int* ret);
int       kernel_dynlib_handle(pid_t pid, const char* basename, uint32_t* handle);
int       sceKernelSendNotificationRequest(int, void*, size_t, int);
extern uint8_t playshell_shellui_elf_start[];
extern uint8_t playshell_shellui_elf_end[];
int emu_start(void (*log_line)(const char* line), void (*notify_line)(const char* line)); // playshell_emu.c
void ps_step(const char* step);                                                                  // (below)
}

__asm__(".section .rodata\n"
        ".global playshell_shellui_elf_start\n"
        ".global playshell_shellui_elf_end\n"
        ".align  16\n"
        "playshell_shellui_elf_start:\n"
        ".incbin \"" PLAYSHELL_SHELLUI_ELF "\"\n"
        "playshell_shellui_elf_end:\n"
        ".previous\n");

namespace {

constexpr const char* kDir      = "/data/PlayShell";
constexpr const char* kLogFile  = "/data/PlayShell/playshell.log";
constexpr const char* kMarker   = "/system_tmp/playshell_injected"; // "<ShellUI pid> <module build>" (one file:
                                                                    // /system_tmp holds few files)
constexpr int kLogPort  = 9056;
constexpr int kStopPort = 9057;
constexpr int kLanStopPort = 9059;                                 // TCP, from the PC: tools/stop_playshell.bat
constexpr const char* kPidFile = "/data/PlayShell/playshell.pid"; // the running PlayShell (a stuck one is killed by it)

int        g_log = -1;
std::mutex g_log_lock;
std::atomic<long> g_ui_bytes{0}; // module log bytes received (the module is alive)

void log(const char* fmt, ...) {
	char      line[1200];
	time_t    now = time(nullptr);
	struct tm tm {};
	localtime_r(&now, &tm);
	const int head = static_cast<int>(strftime(line, sizeof(line), "%H:%M:%S ", &tm));
	va_list   args;
	va_start(args, fmt);
	int n = vsnprintf(line + head, sizeof(line) - head - 1, fmt, args);
	va_end(args);
	if (n < 0) {
		return;
	}
	n = head + (n < static_cast<int>(sizeof(line)) - head - 2 ? n : static_cast<int>(sizeof(line)) - head - 2);
	line[n++] = '\n';
	std::lock_guard<std::mutex> lock(g_log_lock);
	if (g_log >= 0) {
		(void)write(g_log, line, static_cast<size_t>(n));
	}
}

// ---- crash recorder: the step PlayShell is at (set by the risky places), written with a crash ----
std::atomic<const char*> g_step{"start"};
extern "C" char __ehdr_start[]; // (the linker's: where playshell.elf starts)

void on_crash(int sig, siginfo_t* info, void* context) {
	const ucontext_t* uc  = static_cast<const ucontext_t*>(context);
	const uintptr_t   rip = uc != nullptr ? static_cast<uintptr_t>(uc->uc_mcontext.mc_rip) : 0;
	char              line[400];
	const int         n = snprintf(line, sizeof(line),
	                               "CRASH: signal %d at address %p, code %#lx (on_crash %#lx: %+ld), thread %d, step: %s\n", sig,
	                               info != nullptr ? info->si_addr : nullptr, static_cast<unsigned long>(rip),
	                               static_cast<unsigned long>(reinterpret_cast<uintptr_t>(&on_crash)),
	                               static_cast<long>(rip - reinterpret_cast<uintptr_t>(&on_crash)), pthread_getthreadid_np(),
	                               g_step.load());
	if (g_log >= 0 && n > 0) {
		(void)write(g_log, line, static_cast<size_t>(n));
	}
	// where it came from: the code addresses of PlayShell's on the crashed thread's stack (callers, newest first), as
	// offsets in playshell.elf - looked up in the build's playshell.map
	const uintptr_t base = reinterpret_cast<uintptr_t>(__ehdr_start), end = base + (64u << 20); // (playshell.elf: < 64 MB)
	const uintptr_t rsp  = uc != nullptr ? static_cast<uintptr_t>(uc->uc_mcontext.mc_rsp) : 0;
	int             k    = snprintf(line, sizeof(line), "CRASH: in PlayShell %+ld, callers:",
	                                rip >= base && rip < end ? static_cast<long>(rip - base) : -1L);
	uintptr_t      top = rsp + 8192; // (read no further than this thread's stack goes)
	pthread_attr_t attr;
	if (pthread_attr_init(&attr) == 0) {
		void*  sa = nullptr;
		size_t ss = 0;
		if (pthread_attr_get_np(pthread_self(), &attr) == 0 && pthread_attr_getstack(&attr, &sa, &ss) == 0 && sa != nullptr) {
			top = std::min(reinterpret_cast<uintptr_t>(sa) + ss, rsp + 64 * 1024);
		}
		pthread_attr_destroy(&attr);
	}
	for (uintptr_t at = rsp, found = 0; rsp != 0 && at + 8 <= top && found < 14 && k < static_cast<int>(sizeof(line)) - 24; at += 8) {
		const uintptr_t v = *reinterpret_cast<const uintptr_t*>(at);
		if (v >= base && v < end) {
			k += snprintf(line + k, sizeof(line) - static_cast<size_t>(k), " %#lx", static_cast<unsigned long>(v - base));
			++found;
		}
	}
	k         = std::min(k, static_cast<int>(sizeof(line)) - 2);
	line[k++] = '\n';
	if (g_log >= 0) {
		(void)write(g_log, line, static_cast<size_t>(k));
	}
	// ends right here (_exit: every thread, the ports freed) - re-raising the signal left a dead PlayShell hanging in the
	// system's crash handling, holding its ports ("already running" until a restart)
	unlink(kPidFile);
	_exit(128 + sig);
}

void install_crash_recorder() {
	static char alt[64 * 1024]; // (a stack overflow still gets its line)
	stack_t     ss {};
	ss.ss_sp   = alt;
	ss.ss_size = sizeof(alt);
	(void)sigaltstack(&ss, nullptr);
	struct sigaction sa {};
	sa.sa_sigaction = on_crash;
	sa.sa_flags     = SA_SIGINFO | SA_ONSTACK;
	for (const int sig : {SIGSEGV, SIGBUS, SIGILL, SIGFPE, SIGABRT}) {
		sigaction(sig, &sa, nullptr);
	}
}

void notify(const char* text) {
	// Layout of the system's notification request (0xC30 bytes).
	struct Request {
		int32_t type, req_id, priority, msg_id, target_id, user_id, unk1, unk2, app_id, error_num, unk3;
		char    use_icon_image_uri;
		char    message[1024];
		char    uri[1024];
		char    unused[1027];
	};
	static_assert(sizeof(Request) == 0xC30, "notification request layout");
	Request req {};
	req.target_id          = -1;
	req.use_icon_image_uri = 1;
	snprintf(req.message, sizeof(req.message), "%s", text);
	snprintf(req.uri, sizeof(req.uri), "%s", "cxml://psnotification/tex_icon_system");
	sceKernelSendNotificationRequest(0, &req, sizeof(req), 0);
	log("notify: %s", text);
}

int udp_bound(int port) {
	const int   s = socket(AF_INET, SOCK_DGRAM, 0);
	sockaddr_in a {};
	a.sin_family      = AF_INET;
	a.sin_port        = htons(static_cast<uint16_t>(port));
	a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
	if (s >= 0 && bind(s, reinterpret_cast<sockaddr*>(&a), sizeof(a)) == 0) {
		return s;
	}
	if (s >= 0) {
		close(s);
	}
	return -1;
}

void udp_send(int port, const char* text) {
	const int   s = socket(AF_INET, SOCK_DGRAM, 0);
	sockaddr_in a {};
	a.sin_family      = AF_INET;
	a.sin_port        = htons(static_cast<uint16_t>(port));
	a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
	if (s >= 0) {
		(void)sendto(s, text, strlen(text), 0, reinterpret_cast<sockaddr*>(&a), sizeof(a));
		close(s);
	}
}

// ---- one PlayShell at a time: a newer one stops the running one ----
void* stop_thread(void* arg) {
	const int s = static_cast<int>(reinterpret_cast<intptr_t>(arg));
	char      buf[16];
	for (;;) {
		const ssize_t n = recv(s, buf, sizeof(buf), 0);
		if (n >= 1 && buf[0] == 'X') {
			log("stop: a newer PlayShell takes over");
			usleep(200 * 1000);
			_exit(0);
		}
	}
	return nullptr;
}

int take_over() {
	int s = udp_bound(kStopPort);
	if (s >= 0) {
		return s;
	}
	log("another PlayShell is running: stopping it");
	udp_send(kStopPort, "X");
	for (int i = 0; i < 30 && s < 0; ++i) {
		usleep(100 * 1000);
		s = udp_bound(kStopPort);
	}
	if (s < 0) { // it did not stop (stuck): killed by its pid - only while it still holds PlayShell's stop port
		long  pid = 0;
		FILE* f   = fopen(kPidFile, "r");
		if (f != nullptr) {
			if (fscanf(f, "%ld", &pid) != 1) {
				pid = 0;
			}
			fclose(f);
		}
		if (pid > 0 && pid != getpid()) {
			const int k = kill(static_cast<pid_t>(pid), SIGKILL);
			log("the older PlayShell (pid %ld) did not stop: killed (%d)", pid, k);
			for (int i = 0; i < 30 && s < 0; ++i) {
				usleep(100 * 1000);
				s = udp_bound(kStopPort);
			}
		}
	}
	usleep(300 * 1000); // its game ports close with it
	return s;
}

// The stop port on the network (TCP 9059): tools/stop_playshell.bat connects, PlayShell answers and ends.
void* lan_stop_thread(void*) {
	const int   s = socket(AF_INET, SOCK_STREAM, 0);
	int         one = 1;
	sockaddr_in a {};
	a.sin_family      = AF_INET;
	a.sin_port        = htons(kLanStopPort);
	a.sin_addr.s_addr = htonl(INADDR_ANY);
	(void)setsockopt(s, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
	if (s < 0 || bind(s, reinterpret_cast<sockaddr*>(&a), sizeof(a)) != 0 || listen(s, 2) != 0) {
		log("stop port %d unavailable", kLanStopPort);
		return nullptr;
	}
	for (;;) {
		const int c = accept(s, nullptr, nullptr);
		if (c < 0) {
			usleep(100 * 1000);
			continue;
		}
		const char* bye = "PlayShell stopping\n";
		(void)write(c, bye, strlen(bye));
		close(c);
		log("stop: asked from the network");
		unlink(kPidFile);
		usleep(200 * 1000);
		_exit(0);
	}
	return nullptr;
}

// ---- the module's log lines (UDP 9056) into our log ----
void* module_log_thread(void*) {
	int s = -1;
	for (int i = 0; i < 50 && (s = udp_bound(kLogPort)) < 0; ++i) {
		usleep(100 * 1000);
	}
	if (s < 0) {
		log("module log: cannot listen on %d", kLogPort);
		return nullptr;
	}
	char buf[1500];
	for (;;) {
		const ssize_t n = recv(s, buf, sizeof(buf), 0);
		if (n > 0) {
			g_ui_bytes.fetch_add(n);
			std::lock_guard<std::mutex> lock(g_log_lock);
			if (g_log >= 0) {
				(void)write(g_log, "  ui ", 5);
				(void)write(g_log, buf, static_cast<size_t>(n));
			}
		}
	}
	return nullptr;
}

// ---- the home screen: inject the module (the way LiveShell does) ----

// A module build id: the embedded module's bytes. Another build in the same home screen: restart it first.
uint32_t module_build() {
	uint32_t h = 2166136261u;
	for (const uint8_t* p = playshell_shellui_elf_start; p < playshell_shellui_elf_end; ++p) {
		h = (h ^ *p) * 16777619u;
	}
	return h;
}

bool read_marker(long* pid, uint32_t* build) {
	FILE* f = fopen(kMarker, "r");
	if (f == nullptr) {
		return false;
	}
	unsigned long b = 0;
	const bool    ok = fscanf(f, "%ld %lx", pid, &b) == 2;
	fclose(f);
	*build = static_cast<uint32_t>(b);
	return ok;
}

void write_marker(long pid, uint32_t build) {
	FILE* f = fopen(kMarker, "w");
	if (f != nullptr) {
		fprintf(f, "%ld %x\n", pid, build);
		fclose(f);
	}
}

void restart_shellui(pid_t pid) {
	constexpr uintptr_t kDecidAuthId = 0x4800000000000022ULL; // needed for sceKernelTerminateProcess
	const uintptr_t     old          = set_proc_authid(getpid(), kDecidAuthId);
	int                 ret          = 0;
	const int           rc           = sceKernelTerminateProcess(static_cast<int>(pid), &ret);
	set_proc_authid(getpid(), old);
	if (rc != 0) {
		kill(pid, SIGKILL);
	}
	log("restart: home screen %d ended (an older PlayShell was in it), the system starts a new one", static_cast<int>(pid));
}

// A just-started ShellUI is patched by kstuff once its trophy libraries are mapped: wait for them (LiveShell
// v1.1-rc3: injecting at once froze it).
void wait_shellui_ready(pid_t pid) {
	bool waited = false;
	for (const char* name : {"libSceNpTrophy.sprx", "libSceNpTrophy2.sprx"}) {
		int i = 0;
		for (; i < 60; ++i) { // max 30 s each, like kstuff
			uint32_t handle = 0xffffffffu;
			if (kernel_dynlib_handle(pid, name, &handle) == 0 && handle != 0xffffffffu) {
				break;
			}
			if (kill(pid, 0) != 0) {
				return;
			}
			waited = true;
			usleep(500 * 1000);
		}
	}
	if (waited) {
		sleep(3); // a fresh ShellUI: let kstuff finish with it
	}
}

// The injector stops ShellUI (ptrace) and detaches; if that detach does not take, the home screen stays frozen.
// The module logs within 2 s of starting: no lines -> detach again + SIGCONT (as LiveShell does).
void unfreeze_if_stuck(pid_t pid) {
	const long before = g_ui_bytes.load();
	for (int i = 0; i < 40; ++i) {
		usleep(100 * 1000);
		if (g_ui_bytes.load() > before) {
			log("home screen module is running");
			return;
		}
	}
	log("home screen looks FROZEN after injection: detaching again + SIGCONT");
	const uintptr_t old_authid = set_ucred_to_ptrace();
	(void)pt_detach(pid, 0);
	(void)kill(pid, SIGCONT);
	if (old_authid != 0) {
		set_proc_authid(getpid(), old_authid);
	}
}

// Injected (or already there): the home screen's pid; 0 / -1: not found / failed.
pid_t inject_shellui(bool* fresh) {
	*fresh          = false;
	const pid_t pid = onion_find_pid("SceShellUI");
	if (pid <= 0) {
		return 0;
	}
	const uint32_t build = module_build();
	long           mpid  = 0;
	uint32_t       mbuild = 0;
	if (read_marker(&mpid, &mbuild) && mpid == pid) {
		if (mbuild == build) {
			log("already in the home screen (pid %d)", static_cast<int>(pid));
			return pid;
		}
		restart_shellui(pid); // its old module holds the hooks and the ports
		unlink(kMarker);
		return 0; // the watcher injects into the new one
	}
	wait_shellui_ready(pid);
	log("injecting into the home screen (pid %d)", static_cast<int>(pid));
	if (!Inject_Toolbox(static_cast<int>(pid), playshell_shellui_elf_start)) {
		log("injection FAILED");
		return -1;
	}
	write_marker(pid, build);
	*fresh = true;
	unfreeze_if_stuck(pid);
	return pid;
}

// Keeps PlayShell in the home screen: after a restart of it (LiveShell's build swap, a crash) inject again.
void* watch_thread(void*) {
	pid_t in      = 0;
	bool  told    = false;
	int   fails   = 0;
	for (;;) {
		const pid_t now = onion_find_pid("SceShellUI");
		if (now > 0 && now != in) {
			bool fresh = false;
			const pid_t got = inject_shellui(&fresh);
			if (got > 0) {
				in    = got;
				fails = 0;
				if (!told) {
					notify("PlayShell is active - Hold R3 to play 🎮");
					told = true;
				}
			} else if (got < 0 && ++fails == 3) {
				notify("PlayShell could not start (injection failed). Log: /data/PlayShell/playshell.log");
			}
		}
		sleep(2);
	}
	return nullptr;
}

// ---- JIT test: can PlayShell write machine code and then run it? (the fastest PS1 emulators translate the PS1's
// code into the PS5's this way). 6 bytes "mov eax, 42; ret" in our own memory, made runnable through the kernel
// (as the payload loader does for every payload), then called - a fault is caught, nothing else is touched.
sigjmp_buf g_probe_jmp;

void probe_fault(int) {
	siglongjmp(g_probe_jmp, 1);
}

int run_probe(void* code) {
	struct sigaction sa {}, old[3] {};
	sa.sa_handler = probe_fault;
	sigaction(SIGSEGV, &sa, &old[0]);
	sigaction(SIGBUS, &sa, &old[1]);
	sigaction(SIGILL, &sa, &old[2]);
	int result = -1;
	if (sigsetjmp(g_probe_jmp, 1) == 0) {
		result = reinterpret_cast<int (*)()>(code)();
	}
	sigaction(SIGSEGV, &old[0], nullptr);
	sigaction(SIGBUS, &old[1], nullptr);
	sigaction(SIGILL, &old[2], nullptr);
	return result;
}

void jit_probe() {
	void* m = mmap(nullptr, 0x4000, PROT_READ | PROT_WRITE, MAP_ANON | MAP_PRIVATE, -1, 0);
	if (m == MAP_FAILED) {
		log("jit test: no memory");
		return;
	}
	uint8_t code[] = {0xB8, 0x2A, 0x00, 0x00, 0x00, 0xC3}; // mov eax, 42; ret
	memcpy(m, code, sizeof(code));
	const int rx  = kernel_mprotect(getpid(), reinterpret_cast<intptr_t>(m), 0x4000, PROT_READ | PROT_EXEC);
	const int one = rx == 0 ? run_probe(m) : -1;
	// writable AND runnable at once (simplest for an emulator: change code without switching back and forth)
	const int rwx = kernel_mprotect(getpid(), reinterpret_cast<intptr_t>(m), 0x4000, PROT_READ | PROT_WRITE | PROT_EXEC);
	int       two = -1;
	if (rwx == 0) {
		code[1] = 0x2B; // now 43
		bool wrote = false;
		struct sigaction sa {}, old {};
		sa.sa_handler = probe_fault;
		sigaction(SIGSEGV, &sa, &old);
		if (sigsetjmp(g_probe_jmp, 1) == 0) {
			memcpy(m, code, sizeof(code));
			wrote = true;
		}
		sigaction(SIGSEGV, &old, nullptr);
		two = wrote ? run_probe(m) : -2;
	}
	log("jit test: write then run: %s (mprotect %d, got %d); write and run together: %s (mprotect %d, got %d)",
	    one == 42 ? "YES" : "no", rx, one, two == 43 ? "YES" : "no", rwx, two);
	munmap(m, 0x4000);
}

void start_thread(void* (*fn)(void*), void* arg = nullptr) {
	pthread_t t;
	if (pthread_create(&t, nullptr, fn, arg) == 0) {
		pthread_detach(t);
	} else {
		log("thread start failed");
	}
}

} // namespace

extern "C" void ps_step(const char* step) { // the crash recorder's "step" (a string that lives on: literals)
	g_step.store(step);
}

extern "C" int main(int argc, char const* argv[]) {
	(void)argc;
	(void)argv;
	signal(SIGPIPE, SIG_IGN);
	mkdir(kDir, 0777);
	mkdir("/data/PlayShell/roms", 0777);
	mkdir("/data/PlayShell/roms/gb", 0777);
	mkdir("/data/PlayShell/roms/gbc", 0777);
	mkdir("/data/PlayShell/roms/nes", 0777);
	mkdir("/data/PlayShell/roms/gg", 0777);
	mkdir("/data/PlayShell/roms/ps1", 0777);
	mkdir("/data/PlayShell/roms/snes", 0777);
	mkdir("/data/PlayShell/roms/gba", 0777);
	mkdir("/data/PlayShell/roms/nds", 0777);
	mkdir("/data/PlayShell/roms/dos", 0777);
	mkdir("/data/PlayShell/roms/n64", 0777);
	mkdir("/data/PlayShell/roms/pce", 0777);
	mkdir("/data/PlayShell/roms/ws", 0777);
#if PLAYSHELL_3DS
	mkdir("/data/PlayShell/roms/3ds", 0777);
#endif
	mkdir("/data/PlayShell/crop", 0777); // per console: <console>.txt "left right top bottom" (picture edges)
	mkdir("/data/PlayShell/bios", 0777);  // PS1: an optional real BIOS (scph*.bin)
	mkdir("/data/PlayShell/saves", 0777); // PS1 memory cards
	mkdir("/data/PlayShell/mesen", 0777); // Mesen 2: NES battery saves, its settings
	g_log = open(kLogFile, O_WRONLY | O_CREAT | O_TRUNC, 0666);
	log("PlayShell starting (module build %x)", module_build());
	jit_probe();
	install_crash_recorder(); // (after the JIT test: it catches its own faults)

	const int stop = take_over();
	if (stop < 0) {
		log("the running PlayShell did not stop: giving up");
		notify("PlayShell is already running");
		return 1;
	}
	start_thread(stop_thread, reinterpret_cast<void*>(static_cast<intptr_t>(stop)));
	start_thread(lan_stop_thread);
	if (FILE* f = fopen(kPidFile, "w")) { // (a newer PlayShell kills this one by it, should it ever be stuck)
		fprintf(f, "%d\n", static_cast<int>(getpid()));
		fclose(f);
	}
	start_thread(module_log_thread);
	usleep(100 * 1000);
	emu_start([](const char* line) { log("%s", line); }, [](const char* line) { notify(line); });
	start_thread(watch_thread);
	for (;;) {
		sleep(60);
	}
}
