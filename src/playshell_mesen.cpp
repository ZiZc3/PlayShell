/*
 * playshell_mesen.cpp - PlayShell: Mesen 2 (GPL-3.0, github.com/SourMesen/Mesen2) as PlayShell's emulator for NES
 * and Game Gear (later SNES / GBA / PC Engine). Mesen runs its own emulation thread; this is the small
 * frontend it talks to: the picture (UpdateFrame) and the sound (PlayBuffer) go to the home screen module like the
 * Game Boy's (playshell_emu.c sends them), the controller comes from it (IsKeyPressed).
 * Mesen's own files (battery saves, settings): /data/PlayShell/mesen.
 */
#include "Core/Shared/Emulator.h"
#include "Core/Shared/EmuSettings.h"
#include "Core/Shared/KeyManager.h"
#include "Core/Shared/RenderedFrame.h"
#include "Core/Shared/SaveStateManager.h"
#include "Core/Shared/SettingTypes.h"
#include "Core/Shared/Audio/SoundMixer.h"
#include "Core/Shared/Video/VideoRenderer.h"
#include "Core/Shared/Interfaces/IAudioDevice.h"
#include "Core/Shared/Interfaces/IKeyManager.h"
#include "Core/Shared/Interfaces/IRenderingDevice.h"
#include "Utilities/ArchiveReader.h"
#include "Utilities/FolderUtilities.h"
#include "Utilities/VirtualFile.h"
#include "playshell_pce_palette.h"

#include <algorithm>
#include <atomic>
#include <ctime>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <strings.h>
#include <sys/stat.h>
#include <memory>

extern "C" {
void ps_send_picture(const uint16_t* px, int w, int h);   // playshell_emu.c: the frame to the home screen
void ps_send_sound(const int16_t* stereo, unsigned frames); // playshell_emu.c: the sound to the home screen
void ps_log(const char* line);                              // playshell_emu.c
void ps_step(const char* step);                             // playshell_daemon.cpp: the crash recorder's step: a log line
}

namespace {

// The controller: the home screen sends the Game Boy layout (bit = 0: pressed); Mesen asks per key code.
// Key codes 1..8 = A, B, Select, Start, Right, Left, Up, Down (the bits in that order).
std::atomic<uint8_t> g_pad{0xFF};
// The whole DualSense ("K" from the home screen): key codes 100 + bit for SNES / GBA - Left 0, Up 1, Right 2, Down 3,
// Square 4, Triangle 5, Circle 6, Cross 7, Options 8, Create 9, L1 10, R1 11. The left stick moves the d-pad too.
std::atomic<uint32_t> g_ds{0};
std::atomic<int>      g_ds_lx{0}, g_ds_ly{0};
constexpr uint16_t    kDs = 100;

class Keys : public IKeyManager {
public:
	void RefreshState() override {}
	void UpdateDevices() override {}
	bool IsMouseButtonPressed(MouseButton) override { return false; }
	bool IsKeyPressed(uint16_t code) override {
		if (code >= kDs && code < kDs + 20) {
			const int bit = code - kDs;
			if ((g_ds.load() >> bit) & 1u) {
				return true;
			}
			const int lx = g_ds_lx.load(), ly = g_ds_ly.load();
			return (bit == 0 && lx < -64) || (bit == 2 && lx > 64) || (bit == 1 && ly < -64) || (bit == 3 && ly > 64);
		}
		return code >= 1 && code <= 8 && !(g_pad.load() & (1u << (code - 1)));
	}
	vector<uint16_t> GetPressedKeys() override {
		vector<uint16_t> keys;
		for (uint16_t c = 1; c <= 8; ++c) {
			if (IsKeyPressed(c)) {
				keys.push_back(c);
			}
		}
		for (uint16_t c = kDs; c < kDs + 12; ++c) { // (12: through R1)
			if (IsKeyPressed(c)) {
				keys.push_back(c);
			}
		}
		return keys;
	}
	string   GetKeyName(uint16_t) override { return ""; }
	uint16_t GetKeyCode(string) override { return 0; }
	bool     SetKeyState(uint16_t, bool) override { return false; }
	void     ResetKeyState() override {}
	void     SetDisabled(bool) override {}
};

// The picture: Mesen's finished frame (32-bit ARGB) -> RGB565 -> the home screen.
class Screen : public IRenderingDevice {
public:
	explicit Screen(Emulator* emu) : _emu(emu) { _emu->GetVideoRenderer()->RegisterRenderingDevice(this); }
	~Screen() override { _emu->GetVideoRenderer()->UnregisterRenderingDevice(this); }
	void UpdateFrame(RenderedFrame& frame) override {
		pace();
		const uint32_t w = frame.Width, h = frame.Height;
		if (frame.FrameBuffer == nullptr || w == 0 || h == 0 || w > 4096 || h > 4096) {
			return;
		}
		// bigger than 512 (the PC Engine's frames mixing resolutions: Mesen draws them 4x): every n-th pixel
		const uint32_t  step = std::max((w + 511) / 512, (h + 511) / 512);
		const uint32_t  ow = w / step, oh = h / step;
		_px.resize(size_t(ow) * oh);
		const uint32_t* src = static_cast<const uint32_t*>(frame.FrameBuffer);
		for (uint32_t y = 0; y < oh; ++y) {
			const uint32_t* row = src + size_t(y * step) * w;
			uint16_t*       o   = _px.data() + size_t(y) * ow;
			for (uint32_t x = 0; x < ow; ++x) {
				const uint32_t c = row[x * step];
				o[x] = static_cast<uint16_t>(((c >> 8) & 0xF800) | ((c >> 5) & 0x07E0) | ((c >> 3) & 0x001F));
			}
		}
		ps_send_picture(_px.data(), int(ow), int(oh));
	}
	void ClearFrame() override {}
	void Render(RenderSurfaceInfo&, RenderSurfaceInfo&) override {}
	void Reset() override {}
	void SetExclusiveFullscreenMode(bool, void*) override {}

private:
	Emulator*        _emu;
	vector<uint16_t> _px;
	long long        _last = 0, _t0 = 0, _max_gap = 0, _late = 0;
	int              _n = 0, _logs = 0;

	static long long now_us() {
		timespec t {};
		clock_gettime(CLOCK_MONOTONIC, &t);
		return (long long)t.tv_sec * 1000000LL + t.tv_nsec / 1000;
	}
	// Frame pacing, every 5 s while running (30 lines): frames per second, the longest gap, gaps over 20 ms.
	void pace() {
		const long long t = now_us();
		if (_last != 0) {
			const long long gap = t - _last;
			_max_gap = gap > _max_gap ? gap : _max_gap;
			_late += gap > 20000 ? 1 : 0;
		}
		_last = t;
		if (_t0 == 0) {
			_t0 = t;
		}
		++_n;
		if (t - _t0 >= 5000000 && _logs < 30) {
			char line[160];
			snprintf(line, sizeof(line), "games: Mesen %.1f fps, longest gap %lld ms, %lld gaps over 20 ms", _n * 1e6 / (t - _t0),
			         _max_gap / 1000, _late);
			ps_log(line);
			++_logs;
			_t0 = t, _n = 0, _max_gap = 0, _late = 0;
		}
	}
};

// The sound: 48 kHz stereo, in pieces the home screen's receiver takes (<= 1024 frames per packet).
class Sound : public IAudioDevice {
public:
	explicit Sound(Emulator* emu) : _emu(emu) { _emu->GetSoundMixer()->RegisterAudioDevice(this); }
	~Sound() override { _emu->GetSoundMixer()->RegisterAudioDevice(nullptr); }
	void PlayBuffer(int16_t* buf, uint32_t frames, uint32_t rate, bool stereo) override {
		if (!stereo || rate != 48000) {
			if (!_told) {
				_told = true;
				char line[96];
				snprintf(line, sizeof(line), "games: Mesen sound %u Hz %s: not played", rate, stereo ? "stereo" : "mono");
				ps_log(line);
			}
			return;
		}
		for (uint32_t at = 0; at < frames; at += 1024) {
			const uint32_t n = frames - at < 1024 ? frames - at : 1024;
			ps_send_sound(buf + at * 2, n);
		}
	}
	void            Stop() override {}
	void            Pause() override {}
	void            ProcessEndOfFrame() override {}
	string          GetAvailableDevices() override { return ""; }
	void            SetAudioDevice(string) override {}
	AudioStatistics GetStatistics() override { return {}; }

private:
	Emulator* _emu;
	bool      _told = false;
};

std::unique_ptr<Emulator> g_emu;
std::unique_ptr<Screen>   g_screen;
std::unique_ptr<Sound>    g_sound;
std::unique_ptr<Keys>     g_keys;

KeyMapping pad_mapping() {
	KeyMapping k;
	k.A = 1, k.B = 2, k.Select = 3, k.Start = 4, k.Right = 5, k.Left = 6, k.Up = 7, k.Down = 8;
	return k;
}

// A BIOS from PlayShell's BIOS folder into Mesen's firmware folder (when missing there or a different size).
void copy_bios(const char* from, const char* to) {
	FILE* in = fopen(from, "rb");
	if (in == nullptr) {
		return;
	}
	vector<uint8_t> data;
	static uint8_t  buf[65536]; // (not on the stack: a payload thread's stack is small - 64 KB there crashed it)
	size_t          n;
	while ((n = fread(buf, 1, sizeof(buf), in)) > 0) {
		data.insert(data.end(), buf, buf + n);
	}
	fclose(in);
	FILE* old = fopen(to, "rb");
	if (old != nullptr) {
		fseek(old, 0, SEEK_END);
		const long size = ftell(old);
		fclose(old);
		if (size == (long)data.size()) {
			return;
		}
	}
	mkdir("/data/PlayShell/mesen/Firmware", 0777);
	FILE* out = fopen(to, "wb");
	if (out != nullptr) {
		fwrite(data.data(), 1, data.size(), out);
		fclose(out);
		ps_log("games: BIOS copied for Mesen");
	}
}

bool start_mesen() {
	if (g_emu) {
		return true;
	}
	ps_step("Mesen: home folder");
	FolderUtilities::SetHomeFolder("/data/PlayShell/mesen");
	ps_step("Mesen: new Emulator");
	g_emu.reset(new Emulator());
	ps_step("Mesen: Initialize");
	g_emu->Initialize(false); // no shortcut keys
	ps_step("Mesen: devices + settings");
	KeyManager::SetSettings(g_emu->GetSettings());
	g_keys.reset(new Keys());
	KeyManager::RegisterKeyManager(g_keys.get());
	g_screen.reset(new Screen(g_emu.get()));
	g_sound.reset(new Sound(g_emu.get()));

	EmuSettings* settings = g_emu->GetSettings();
	PreferencesConfig prefs = settings->GetPreferences();
	prefs.AllowBackgroundInput = true; // no window to have the focus
	settings->SetPreferences(prefs);
	// run-ahead: Mesen runs one frame ahead in secret and shows that, so a press shows a frame sooner (the games'
	// own one-frame delay gone); costs a second emulated frame - cheap for NES / Game Gear
	EmulationConfig emu_cfg = settings->GetEmulationConfig();
	emu_cfg.RunAheadFrames  = 1;
	settings->SetEmulationConfig(emu_cfg);
	AudioConfig audio = settings->GetAudioConfig();
	audio.SampleRate               = 48000;
	audio.DisableDynamicSampleRate = true;
	settings->SetAudioConfig(audio);
	NesConfig nes = settings->GetNesConfig();
	// the NES colours: Mesen's app sets them at start (the core's default palette is all black)
	static const uint32_t kNesPalette[64] = {
		0xFF666666, 0xFF002A88, 0xFF1412A7, 0xFF3B00A4, 0xFF5C007E, 0xFF6E0040, 0xFF6C0600, 0xFF561D00, 0xFF333500, 0xFF0B4800,
		0xFF005200, 0xFF004F08, 0xFF00404D, 0xFF000000, 0xFF000000, 0xFF000000, 0xFFADADAD, 0xFF155FD9, 0xFF4240FF, 0xFF7527FE,
		0xFFA01ACC, 0xFFB71E7B, 0xFFB53120, 0xFF994E00, 0xFF6B6D00, 0xFF388700, 0xFF0C9300, 0xFF008F32, 0xFF007C8D, 0xFF000000,
		0xFF000000, 0xFF000000, 0xFFFFFEFF, 0xFF64B0FF, 0xFF9290FF, 0xFFC676FF, 0xFFF36AFF, 0xFFFE6ECC, 0xFFFE8170, 0xFFEA9E22,
		0xFFBCBE00, 0xFF88D800, 0xFF5CE430, 0xFF45E082, 0xFF48CDDE, 0xFF4F4F4F, 0xFF000000, 0xFF000000, 0xFFFFFEFF, 0xFFC0DFFF,
		0xFFD3D2FF, 0xFFE8C8FF, 0xFFFBC2FF, 0xFFFEC4EA, 0xFFFECCC5, 0xFFF7D8A5, 0xFFE4E594, 0xFFCFEF96, 0xFFBDF4AB, 0xFFB3F3CC,
		0xFFB5EBF2, 0xFFB8B8B8, 0xFF000000, 0xFF000000};
	memset(nes.UserPalette, 0, sizeof(nes.UserPalette));
	memcpy(nes.UserPalette, kNesPalette, sizeof(kNesPalette));
	nes.IsFullColorPalette = false; // the 448 emphasis colours: made from these
	for (uint32_t& v : nes.ChannelVolumes) { // the core's default is 0 (silent); Mesen's app sets 100
		v = 100;
	}
	nes.Port1.Type          = ControllerType::NesController;
	nes.Port1.Keys.Mapping1 = pad_mapping();
	settings->SetNesConfig(nes);
	// Game Gear (Mesen's Master System core): sound on, the empty border cut off (as Mesen's app: 160 x 144);
	// Start = its Start button
	SmsConfig sms = settings->GetSmsConfig();
	for (uint32_t& v : sms.ChannelVolumes) {
		v = 100;
	}
	sms.NtscOverscan.Top = sms.NtscOverscan.Bottom = 24;
	sms.PalOverscan.Top = sms.PalOverscan.Bottom = 24;
	sms.GameGearOverscan.Top = sms.GameGearOverscan.Bottom = sms.GameGearOverscan.Left = sms.GameGearOverscan.Right = 48;
	sms.Port1.Type          = ControllerType::SmsController;
	sms.Port1.Keys.Mapping1 = pad_mapping();
	settings->SetSmsConfig(sms);
	// SNES: the Nintendo layout by position (B = Cross, A = Circle, Y = Square, X = Triangle), L1 / R1 = L / R; the
	// empty lines above and below cut off (as Mesen's app: 256 x 224)
	SnesConfig snes = settings->GetSnesConfig();
	KeyMapping sk;
	sk.B = kDs + 7, sk.A = kDs + 6, sk.Y = kDs + 4, sk.X = kDs + 5, sk.L = kDs + 10, sk.R = kDs + 11;
	sk.Start = kDs + 8, sk.Select = kDs + 9, sk.Left = kDs + 0, sk.Up = kDs + 1, sk.Right = kDs + 2, sk.Down = kDs + 3;
	snes.Port1.Type          = ControllerType::SnesController;
	snes.Port1.Keys.Mapping1 = sk;
	snes.Overscan.Top        = 7;
	snes.Overscan.Bottom     = 8;
	settings->SetSnesConfig(snes);
	// GBA: as the Game Boy - A = Cross or Circle, B = Square or Triangle; L1 / R1 = L / R. Its BIOS (needed):
	// /data/PlayShell/bios/gba_bios.bin, copied to Mesen's firmware folder
	GbaConfig  gba = settings->GetGbaConfig();
	KeyMapping g1, g2;
	g1.A = kDs + 7, g1.B = kDs + 4, g1.L = kDs + 10, g1.R = kDs + 11, g1.Start = kDs + 8, g1.Select = kDs + 9;
	g1.Left = kDs + 0, g1.Up = kDs + 1, g1.Right = kDs + 2, g1.Down = kDs + 3;
	g2.A = kDs + 6, g2.B = kDs + 5;
	gba.Controller.Type          = ControllerType::GbaController;
	gba.Controller.Keys.Mapping1 = g1;
	gba.Controller.Keys.Mapping2 = g2;
	settings->SetGbaConfig(gba);
	copy_bios("/data/PlayShell/bios/gba_bios.bin", "/data/PlayShell/mesen/Firmware/gba_bios.bin");
	// PC Engine (HuCards): its colours (Mesen's app palette), I = Circle, II = Cross (by position, as the SNES), Run =
	// Options, Select = Create; edges as Mesen's app (left / right 18) but 1 line top and bottom: 256 x 240
	PcEngineConfig pce = settings->GetPcEngineConfig();
	memcpy(pce.Palette, kPcePalette, sizeof(kPcePalette));
	pce.PreventSelectRunReset = true;
	pce.Overscan.Left = pce.Overscan.Right = 18;
	pce.Overscan.Top = pce.Overscan.Bottom = 1;
	KeyMapping pk;
	pk.A = kDs + 6, pk.B = kDs + 7, pk.Start = kDs + 8, pk.Select = kDs + 9;
	pk.Left = kDs + 0, pk.Up = kDs + 1, pk.Right = kDs + 2, pk.Down = kDs + 3;
	pce.Port1.Type          = ControllerType::PceController;
	pce.Port1.Keys.Mapping1 = pk;
	settings->SetPcEngineConfig(pce);
	// WonderSwan / Color: turned with the game (vertical games: the picture turned, the pad too - as Mesen's app);
	// LCD look as Mesen's app, no icon strip (224 x 144). Held sideways: X pad = d-pad, A = Circle, B = Cross, Y pad =
	// Triangle / Square / L1 / R1 (up / left / down / right). Vertical: Y pad = d-pad, X pad = the face buttons,
	// A = R1, B = L1. Start = Options, Sound = Create.
	WsConfig ws = settings->GetWsConfig();
	ws.AutoRotate      = true;
	ws.BlendFrames     = true;
	ws.LcdAdjustColors = true;
	ws.LcdShowIcons    = false;
	KeyMapping wh, wv;
	wh.A = kDs + 6, wh.B = kDs + 7, wh.Start = kDs + 8, wh.GenericKey1 = kDs + 9;
	wh.Left = kDs + 0, wh.Up = kDs + 1, wh.Right = kDs + 2, wh.Down = kDs + 3;
	wh.U = kDs + 5, wh.L = kDs + 4, wh.D = kDs + 10, wh.R = kDs + 11;
	wv.A = kDs + 11, wv.B = kDs + 10, wv.Start = kDs + 8, wv.GenericKey1 = kDs + 9;
	wv.U = kDs + 0, wv.D = kDs + 2, wv.L = kDs + 3, wv.R = kDs + 1;              // Y pad: the d-pad (turned)
	wv.Up = kDs + 4, wv.Down = kDs + 6, wv.Left = kDs + 7, wv.Right = kDs + 5; // X pad: the face buttons (turned)
	ws.ControllerHorizontal.Type          = ControllerType::WsController;
	ws.ControllerHorizontal.Keys.Mapping1 = wh;
	ws.ControllerVertical.Type            = ControllerType::WsControllerVertical;
	ws.ControllerVertical.Keys.Mapping1   = wv;
	settings->SetWsConfig(ws);
	ps_log("games: Mesen 2 ready");
	return true;
}

} // namespace

extern "C" {

// Loads and runs a game (Mesen's own thread). 1: running.
int ps_mesen_load(const char* path) {
	if (!start_mesen()) {
		return 0;
	}
	g_pad.store(0xFF);
	// run-ahead (a press shows a frame sooner) costs a second emulated frame: kept for the light consoles (NES, Game
	// Gear, PC Engine, WonderSwan); off for GBA and SNES - with it GBA ran at 40-50 fps (log 2026-10-30), which felt
	// like more delay than run-ahead takes away
	{
		const char* dot   = strrchr(path, '.');
		const bool  heavy = dot != nullptr && (strcasecmp(dot, ".gba") == 0 || strcasecmp(dot, ".sfc") == 0 || strcasecmp(dot, ".smc") == 0);
		EmulationConfig emu_cfg = g_emu->GetSettings()->GetEmulationConfig();
		emu_cfg.RunAheadFrames  = heavy ? 0 : 1;
		g_emu->GetSettings()->SetEmulationConfig(emu_cfg);
		ps_log(heavy ? "games: Mesen run-ahead off (GBA / SNES)" : "games: Mesen run-ahead 1 frame");
	}
	ps_step("Mesen: LoadRom");
	const bool ok = g_emu->LoadRom(VirtualFile(string(path)), VirtualFile());
	ps_step("Mesen: running");
	char line[600];
	snprintf(line, sizeof(line), "games: Mesen %s %s", ok ? "running" : "could not load", path);
	ps_log(line);
	return ok ? 1 : 0;
}

void ps_mesen_pause(int paused) {
	if (!g_emu || !g_emu->IsRunning()) {
		return;
	}
	if (paused) {
		g_emu->Pause();
	} else {
		g_emu->Resume();
	}
}

void ps_mesen_pad(uint8_t joypad) {
	g_pad.store(joypad);
}

void ps_mesen_buttons(uint32_t buttons, int8_t lx, int8_t ly) {
	g_ds.store(buttons);
	g_ds_lx.store(lx);
	g_ds_ly.store(ly);
}

// A save state (op 1) or load (op 2) of the game running (Mesen locks its own thread for it). 1: done.
int ps_mesen_state(int op, const char* path) {
	if (!g_emu || !g_emu->IsRunning()) {
		return 0;
	}
	ps_step(op == 1 ? "Mesen: save state" : "Mesen: load state");
	bool ok = false;
	try {
		ok = op == 1 ? g_emu->GetSaveStateManager()->SaveState(string(path), false)
		             : g_emu->GetSaveStateManager()->LoadState(string(path), false);
	} catch (...) {
		ok = false;
	}
	ps_step("Mesen: running");
	char line[700];
	snprintf(line, sizeof(line), "games: Mesen state %s: %s", ok ? (op == 1 ? "saved" : "loaded") : "FAILED", path);
	ps_log(line);
	return ok ? 1 : 0;
}

void ps_mesen_stop(void) {
	if (g_emu && g_emu->IsRunning()) {
		g_emu->Stop(false); // battery saved
	}
}

} // extern "C"

// ---- games in .zip / .7z (Mesen's archive readers: miniz, 7-Zip) ----
extern "C" {

// The first game file inside (a known console's extension); 1: found.
int ps_archive_rom(const char* path, char* inner, size_t cap) {
	static const char* const kExt[] = {".gb", ".gbc", ".nes", ".gg", ".sfc", ".smc", ".gba", ".pce", ".ws", ".wsc"};
	try {
		unique_ptr<ArchiveReader> reader = ArchiveReader::GetReader(string(path));
		if (!reader) {
			return 0;
		}
		for (const string& name : reader->GetFileList()) {
			const size_t dot = name.rfind('.');
			if (dot == string::npos) {
				continue;
			}
			for (const char* ext : kExt) {
				if (strcasecmp(name.c_str() + dot, ext) == 0) {
					snprintf(inner, cap, "%s", name.c_str());
					return 1;
				}
			}
		}
	} catch (...) {
	}
	return 0;
}

// That file's bytes (malloc'd, at most 8 MB), or nullptr.
uint8_t* ps_archive_read(const char* path, const char* inner, size_t* size) {
	try {
		unique_ptr<ArchiveReader> reader = ArchiveReader::GetReader(string(path));
		vector<uint8_t>           data;
		if (!reader || !reader->ExtractFile(string(inner), data) || data.empty() || data.size() > 8 * 1024 * 1024) {
			return nullptr;
		}
		uint8_t* out = static_cast<uint8_t*>(malloc(data.size()));
		if (out != nullptr) {
			memcpy(out, data.data(), data.size());
			*size = data.size();
		}
		return out;
	} catch (...) {
		return nullptr;
	}
}

} // extern "C"
