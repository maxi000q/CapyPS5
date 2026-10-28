// SPDX-FileCopyrightText: Copyright 2026 CapyPS5 Project
// SPDX-License-Identifier: GPL-2.0-only

#include "android/capyps5_jni.h"

#include "common/common.h"
#include "common/archive.h"
#include "common/contentProfile.h"
#include "common/file.h"
#include "common/debugFlags.h"
#include "common/perfCores.h"
#include "common/logging/log.h"
#include "common/passInspector.h"
#include "common/stringUtils.h"
#include "common/telemetry.h"
#include "common/threads.h"
#include "common/virtualMemory.h"
#include "emulator.h"
#include "libs/controller.h"

#include <array>
#include <mutex>
#include <algorithm>
#include <pthread.h>
#include <sys/resource.h>
#include <cerrno>
#include <cstring>
#include <android/log.h>
#include <atomic>
#include <chrono>
#include <filesystem>
#include <memory>
#include <string>
#include <thread>

#define LOG_TAG "CapyPS5-JNI"
#define ALOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)
#define ALOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)
#define ALOGW(...) __android_log_print(ANDROID_LOG_WARN, LOG_TAG, __VA_ARGS__)

static JavaVM*            g_jvm           = nullptr;
static ANativeWindow*     g_native_window = nullptr;
// Console Five Pro fix (v0.0.8pro): the window pointer is shared between the UI thread
// (attach/detach) and the emulator thread (Vulkan surface creation); every access goes
// through this mutex now.
static std::mutex         g_window_mutex;
static std::thread        g_emulator_thread;
static std::atomic<bool>  g_is_running{false};
static std::mutex         g_lifecycle_mutex;
static std::string        g_files_dir;
static std::string        g_cache_dir;
static AndroidGpuDriver   g_gpu_driver;
#if defined(__ANDROID__) && defined(__aarch64__)
#include <malloc.h>
#endif

extern "C" {

JNIEXPORT void JNICALL Java_com_consolefivepro_NativeBridge_nativeInit(
    JNIEnv* env, jclass /*clazz*/, jstring filesDir, jstring cacheDir) {
#if defined(__ANDROID__) && defined(__aarch64__)
#if defined(M_BIONIC_SET_HEAP_TAGGING_LEVEL) && defined(M_HEAP_TAGGING_LEVEL_NONE)
	mallopt(M_BIONIC_SET_HEAP_TAGGING_LEVEL, M_HEAP_TAGGING_LEVEL_NONE);
#endif
#endif
	if (g_jvm == nullptr && env != nullptr) {
		env->GetJavaVM(&g_jvm);
	}
	if (filesDir != nullptr) {
		const char* cFilesDir = env->GetStringUTFChars(filesDir, nullptr);
		g_files_dir = cFilesDir;
		env->ReleaseStringUTFChars(filesDir, cFilesDir);
	}
	if (cacheDir != nullptr) {
		const char* cCacheDir = env->GetStringUTFChars(cacheDir, nullptr);
		g_cache_dir = cCacheDir;
		env->ReleaseStringUTFChars(cacheDir, cCacheDir);
	}

	Common::VirtualMemory::Init();
	Common::InitializeThreads();

	Log::SetFatalReportPath(g_files_dir + "/last_error.txt");
	ALOGI("nativeInit complete. FilesDir: %s, CacheDir: %s",
	      g_files_dir.c_str(), g_cache_dir.c_str());
}

static std::atomic<uint64_t> g_surface_revision {0};
static std::atomic<uint64_t> g_surface_change_ns {0};

static void NoteSurfaceChanged() {
	g_surface_revision.fetch_add(1, std::memory_order_release);
	const auto now_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
		std::chrono::steady_clock::now().time_since_epoch())
		.count();
	g_surface_change_ns.store(static_cast<uint64_t>(now_ns), std::memory_order_release);
}

uint64_t AndroidSurfaceRevision() {
	return g_surface_revision.load(std::memory_order_acquire);
}

uint64_t AndroidSurfaceAgeMs() {
	const auto now_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
		std::chrono::steady_clock::now().time_since_epoch())
		.count();
	const auto changed_ns = g_surface_change_ns.load(std::memory_order_acquire);
	if (changed_ns == 0 || now_ns <= static_cast<int64_t>(changed_ns)) {
		return 0;
	}
	return static_cast<uint64_t>(now_ns - static_cast<int64_t>(changed_ns)) / 1000000u;
}

JNIEXPORT void JNICALL Java_com_consolefivepro_NativeBridge_nativeSetSurface(
    JNIEnv* env, jclass /*clazz*/, jobject surface) {
	std::lock_guard lock(g_window_mutex);

	if (surface != nullptr) {
		auto* window = ANativeWindow_fromSurface(env, surface);
		// Repeated surfaceChanged callbacks hand us the SAME underlying window
		// (windowed resizes fire them in bursts). Swapping refs and bumping the
		// revision for each would make the presenter rebuild the Vulkan surface
		// over and over - the windowed flicker. Same window means nothing changed.
		if (window == g_native_window) {
			ANativeWindow_release(window); // drop the extra reference
			return;
		}
		if (g_native_window != nullptr) {
			ANativeWindow_release(g_native_window);
		}
		g_native_window = window;
		NoteSurfaceChanged();
		ALOGI("nativeSetSurface: Native window attached %p", g_native_window);
	} else {
		if (g_native_window != nullptr) {
			ANativeWindow_release(g_native_window);
			g_native_window = nullptr;
		}
		NoteSurfaceChanged();
		ALOGI("nativeSetSurface: Surface detached");
	}
}

JNIEXPORT void JNICALL Java_com_consolefivepro_NativeBridge_nativeDestroySurface(
    JNIEnv* /*env*/, jclass /*clazz*/) {
	std::lock_guard lock(g_window_mutex);

	if (g_native_window != nullptr) {
		ANativeWindow_release(g_native_window);
		g_native_window = nullptr;
		NoteSurfaceChanged();
		ALOGI("nativeDestroySurface: Native window released");
	}
}

JNIEXPORT jboolean JNICALL Java_com_consolefivepro_NativeBridge_nativeStartGame(
    JNIEnv* env, jclass /*clazz*/, jstring gamePath, jstring elfPath) {
	std::lock_guard lock(g_lifecycle_mutex);

	if (g_is_running.load()) {
		ALOGW("nativeStartGame: Emulator already running");
		return JNI_FALSE;
	}

	std::string game_str;
	std::string elf_str;

	if (gamePath != nullptr) {
		const char* cGame = env->GetStringUTFChars(gamePath, nullptr);
		game_str = cGame;
		env->ReleaseStringUTFChars(gamePath, cGame);
	}

	if (elfPath != nullptr) {
		const char* cElf = env->GetStringUTFChars(elfPath, nullptr);
		elf_str = cElf;
		env->ReleaseStringUTFChars(elfPath, cElf);
	}

	ALOGI("nativeStartGame: game=%s, elf=%s", game_str.c_str(), elf_str.c_str());

	Emulator::RunOptions options;

	// A .zar dump boots straight from the archive: the archive root becomes app0 and
	// eboot.bin resolves through the archive layer's path routing (same as --game <zar>).
	const auto elf_path  = Common::PathFromUtf8(Common::FixFilenameSlash(elf_str));
	const auto game_path = Common::PathFromUtf8(Common::FixFilenameSlash(game_str));
	std::filesystem::path archive {};
	if (!elf_str.empty() && Common::IsSupportedArchive(elf_path)) {
		archive = elf_path;
	} else if (!game_str.empty() && Common::IsSupportedArchive(game_path)) {
		archive = game_path;
	}
	if (!archive.empty()) {
		const auto root = Common::MakeArchivePath(archive);
		if (!Common::File::IsFileExisting(root / "eboot.bin")) {
			ALOGW("nativeStartGame: Archive does not contain eboot.bin: %s",
			      Common::PathToString(archive).c_str());
			return JNI_FALSE;
		}
		options.app0_dir = root;
		options.elf      = "/app0/eboot.bin";
	} else {
		options.app0_dir = game_str;
		options.elf      = elf_str.empty() ? (game_str + "/eboot.bin") : elf_str;

		if (!std::filesystem::exists(options.elf)) {
			ALOGW("nativeStartGame: Executable does not exist on disk: %s", options.elf.c_str());
			return JNI_FALSE;
		}
	}

	// Mobile defaults
	options.config.screen_width       = 1280;
	options.config.screen_height      = 720;
	options.config.fullscreen_enabled = true;
	// Console Five Pro (v0.0.10pro phase 2): the emulated vblank used to stay at the built-in
	// 60 Hz, and PresentThread flips at most once per vblank - every game pacing on vblank was
	// capped at 60 FPS even on 90/120 Hz displays. The frontend passes the chosen refresh
	// (display default, see GameActivity) as debug.capy.vblank_hz before nativeStartGame.
	{
		static Common::DebugFlags::CachedValue vblank_hz("debug.capy.vblank_hz", 0);
		const int64_t                          hz = vblank_hz.Get();
		if (hz >= 30 && hz <= 120) {
			options.config.vblank_frequency = static_cast<uint32_t>(hz);
		}
	}

	// Console Five Pro fix (v0.0.8pro): after a previous game finished on its own
	// (Emulator::Run returned), the thread object is finished but still joinable, and
	// assigning to a joinable std::thread calls std::terminate - so the second game
	// launch from the same process killed the app. Join the finished run first; this
	// never blocks, because g_is_running is false here.
	if (g_emulator_thread.joinable()) {
		g_emulator_thread.join();
	}

	g_is_running.store(true);
	g_emulator_thread = std::thread([options]() {
		ALOGI("Emulator thread started");
		pthread_setname_np(pthread_self(), "CapyEmulator");
		Common::InitializeThreads();
		// Console Five Pro tweak (v0.0.4pro): ask the scheduler for a higher priority on the
		// emulation thread. Best effort only: most ROMs deny nice < 0 for untrusted apps
		// (EPERM), in which case this is a no-op logged once.
		if (setpriority(PRIO_PROCESS, 0, -4) != 0) {
			ALOGI("Emulator thread: priority boost unavailable (%s)", std::strerror(errno));
		} else {
			ALOGI("Emulator thread: priority raised (nice -4)");
		}
		// Console Five Pro tweak (v0.0.6proplus): keep the guest main loop on the big
		// cores of big.LITTLE SoCs (best effort, toggle debug.capy.perf_cores).
		Common::PinToPerformanceCores("Emulator thread");
		Emulator::Run(options);
		g_is_running.store(false);
		ALOGI("Emulator thread finished");
	});

	return JNI_TRUE;
}

// Frontend settings for this session (see Common::DebugFlags::SetFrontendOption). Called before
// nativeStartGame; `name` is the property name, e.g. "debug.capy.res_scale".
JNIEXPORT void JNICALL Java_com_consolefivepro_NativeBridge_nativeSetOption(
    JNIEnv* env, jclass /*clazz*/, jstring name, jlong value) {
	if (name == nullptr) {
		return;
	}
	const char* text = env->GetStringUTFChars(name, nullptr);
	Common::DebugFlags::SetFrontendOption(text, static_cast<int64_t>(value));
	ALOGI("option %s = %lld", text, static_cast<long long>(value));
	env->ReleaseStringUTFChars(name, text);
}

// Called before nativeStartGame. An empty `library` keeps the system Vulkan driver;
// `driverDir` must be app-private storage (dlopen refuses world-writable locations).
JNIEXPORT void JNICALL Java_com_consolefivepro_NativeBridge_nativeSetGpuDriver(
    JNIEnv* env, jclass /*clazz*/, jstring nativeLibraryDir, jstring driverDir, jstring library) {
	const auto read = [env](jstring value) {
		std::string result;
		if (value != nullptr) {
			const char* text = env->GetStringUTFChars(value, nullptr);
			result = text;
			env->ReleaseStringUTFChars(value, text);
		}
		return result;
	};
	g_gpu_driver.native_library_dir = read(nativeLibraryDir);
	g_gpu_driver.driver_dir         = read(driverDir);
	g_gpu_driver.library            = read(library);
	ALOGI("GPU driver: %s", g_gpu_driver.library.empty()
	                           ? "system"
	                           : (g_gpu_driver.driver_dir + "/" + g_gpu_driver.library).c_str());
}

JNIEXPORT void JNICALL Java_com_consolefivepro_NativeBridge_nativeStopGame(
    JNIEnv* /*env*/, jclass /*clazz*/) {
	std::lock_guard lock(g_lifecycle_mutex);

	if (!g_is_running.load()) {
		return;
	}
	ALOGI("nativeStopGame requested");
	g_is_running.store(false);
	if (g_emulator_thread.joinable()) {
		g_emulator_thread.detach();
	}
}

// Last pad state from the frontend, so the on-screen touchpad click can be merged into it (a
// physical controller sends its whole state on every event and would drop the click).
static std::mutex             g_pad_mutex;
static jint                   g_last_buttons = 0;
static std::array<jfloat, 6>  g_last_axes {};
static bool                   g_touchpad_click = false;

static void SubmitPad(jint buttons, jfloat lx, jfloat ly, jfloat rx, jfloat ry, jfloat l2, jfloat r2);

JNIEXPORT void JNICALL Java_com_consolefivepro_NativeBridge_nativeSendControllerInput(
    JNIEnv* /*env*/, jclass /*clazz*/, jint buttons, jfloat lx, jfloat ly,
    jfloat rx, jfloat ry, jfloat l2, jfloat r2) {
	std::lock_guard lock(g_pad_mutex);
	g_last_buttons = buttons;
	g_last_axes    = {lx, ly, rx, ry, l2, r2};
	SubmitPad(buttons, lx, ly, rx, ry, l2, r2);
}

JNIEXPORT void JNICALL Java_com_consolefivepro_NativeBridge_nativeSetTouchpadClick(
    JNIEnv* /*env*/, jclass /*clazz*/, jboolean down) {
	std::lock_guard lock(g_pad_mutex);
	g_touchpad_click = down != JNI_FALSE;
	const auto& a = g_last_axes;
	SubmitPad(g_last_buttons, a[0], a[1], a[2], a[3], a[4], a[5]);
}

static void SubmitPad(jint buttons, jfloat lx, jfloat ly, jfloat rx, jfloat ry, jfloat l2, jfloat r2) {
	uint32_t pad_buttons = static_cast<uint32_t>(buttons) & 0xFFFE;
	if (g_touchpad_click) pad_buttons |= Libs::Controller::PAD_BUTTON_TOUCH_PAD;
	if (buttons & 1) pad_buttons |= Libs::Controller::PAD_BUTTON_TOUCH_PAD;
	if (buttons & (1 << 16)) pad_buttons |= Libs::Controller::PAD_BUTTON_TOUCH_PAD;

	int axis_lx = std::clamp(static_cast<int>((lx + 1.0f) * 127.5f), 0, 255);
	int axis_ly = std::clamp(static_cast<int>((ly + 1.0f) * 127.5f), 0, 255);
	int axis_rx = std::clamp(static_cast<int>((rx + 1.0f) * 127.5f), 0, 255);
	int axis_ry = std::clamp(static_cast<int>((ry + 1.0f) * 127.5f), 0, 255);
	int axis_l2 = std::clamp(static_cast<int>(l2 * 255.0f), 0, 255);
	int axis_r2 = std::clamp(static_cast<int>(r2 * 255.0f), 0, 255);

	// The d-pad and the left stick stay independent, as on a real DualSense: mirroring one
	// into the other made the stick press d-pad buttons in games that use both.
	Libs::Controller::SetState(Libs::Controller::HOST_INPUT_CONTROLLER_ID,
	                           pad_buttons, axis_lx, axis_ly, axis_rx, axis_ry, axis_l2, axis_r2);
}

// On-screen touchpad: finger 0 or 1, coordinates normalized to the pad (0..1).
JNIEXPORT void JNICALL Java_com_consolefivepro_NativeBridge_nativeSendTouch(
    JNIEnv* /*env*/, jclass /*clazz*/, jint finger, jboolean down, jfloat x, jfloat y) {
	Libs::Controller::SetTouchPad(Libs::Controller::HOST_INPUT_CONTROLLER_ID, finger, down != JNI_FALSE, x, y);
}

// Layout must match NativeBridge.STAT_* in Kotlin.
JNIEXPORT jlongArray JNICALL Java_com_consolefivepro_NativeBridge_nativeGetStats(
    JNIEnv* env, jclass /*clazz*/) {
	using namespace Common::Telemetry;
	const jlong values[] = {
	    static_cast<jlong>(g_game_frames.load(std::memory_order_relaxed)),
	    static_cast<jlong>(g_host_presents.load(std::memory_order_relaxed)),
	    static_cast<jlong>(g_max_frame_ns.exchange(0, std::memory_order_relaxed)),
	    static_cast<jlong>(g_wait_reg_mem_timeouts.load(std::memory_order_relaxed)),
	    static_cast<jlong>(g_wait_reg_mem_unmapped.load(std::memory_order_relaxed)),
	    static_cast<jlong>(g_null_buffer_fallbacks.load(std::memory_order_relaxed)),
	    g_is_running.load() ? 1 : 0,
	    // v0.0.10pro phase 4: the locked workload class (0/500/501/502) so the
	    // perf overlay can show WHICH turbo family is driving the engine - the
	    // on-device answer to "did the turbo even engage for this game".
	    static_cast<jlong>(Common::ContentProfile::ActiveClass()),
	    // v0.0.10pro phase 5: frames the presenter generated to fill idle display
	    // ticks (the frame generator). game frames + generated = host presents.
	    static_cast<jlong>(g_generated_frames.load(std::memory_order_relaxed)),
	};
	constexpr auto count = static_cast<jsize>(sizeof(values) / sizeof(values[0]));
	jlongArray     array = env->NewLongArray(count);
	if (array != nullptr) {
		env->SetLongArrayRegion(array, 0, count, values);
	}
	return array;
}

// Five longs per pass: pixel shader address, width << 32 | height, draws, targets, skipped.
JNIEXPORT jlongArray JNICALL Java_com_consolefivepro_NativeBridge_nativeGetPasses(
    JNIEnv* env, jclass /*clazz*/) {
	const auto         passes = Common::PassInspector::Snapshot();
	std::vector<jlong> values;
	values.reserve(passes.size() * 5);
	for (const auto& [pass, skipped]: passes) {
		values.push_back(static_cast<jlong>(pass.pixel_shader));
		values.push_back(static_cast<jlong>((uint64_t {pass.width} << 32u) | pass.height));
		values.push_back(pass.draws);
		values.push_back(pass.targets);
		values.push_back(skipped ? 1 : 0);
	}
	jlongArray array = env->NewLongArray(static_cast<jsize>(values.size()));
	if (array != nullptr && !values.empty()) {
		env->SetLongArrayRegion(array, 0, static_cast<jsize>(values.size()), values.data());
	}
	return array;
}

JNIEXPORT void JNICALL Java_com_consolefivepro_NativeBridge_nativeSetPassSkipped(
    JNIEnv* /*env*/, jclass /*clazz*/, jlong pixel_shader, jboolean skipped) {
	Common::PassInspector::SetSkipped(static_cast<uint64_t>(pixel_shader), skipped == JNI_TRUE);
}

JNIEXPORT void JNICALL Java_com_consolefivepro_NativeBridge_nativeClearSkippedPasses(
    JNIEnv* /*env*/, jclass /*clazz*/) {
	Common::PassInspector::ClearSkipped();
}

JNIEXPORT jstring JNICALL Java_com_consolefivepro_NativeBridge_nativeGetVersion(
    JNIEnv* env, jclass /*clazz*/) {
	const std::string version = "CapyPS5 v0.0.3-alpha (ARM64 Android)";
	return env->NewStringUTF(version.c_str());
}

ANativeWindow* AndroidGetNativeWindow() {
	// Console Five Pro fix (v0.0.8pro): hand out a strong reference so the UI thread can
	// detach the surface without freeing the window while the emulator thread is still
	// building its Vulkan surface from it. The caller drops the extra reference once the
	// surface is created.
	std::lock_guard lock(g_window_mutex);

	if (g_native_window == nullptr) {
		return nullptr;
	}
	ANativeWindow_acquire(g_native_window);
	return g_native_window;
}

} // extern "C"

const std::string& AndroidGetFilesDir() {
	return g_files_dir;
}

const AndroidGpuDriver& AndroidGetGpuDriver() {
	return g_gpu_driver;
}
