/*
SMOKE_MAIN.C

A Linux harness for the iOS port's host runtime (not part of the app): the
translated guest, ios_imports.c and ios/host's runtime (host_wasm.c,
host_syscall.c, host_misc.c, host_gl.c) run on x86-64 Linux, with this file
in place of host_main.m, host_sdl.c and host_touch.m. SDL is a stand-in:
the window is an OpenGL ES 3 context of Mesa's surfaceless EGL, so the
renderer's start-up runs for real; events never come. It shows how far the
game gets (tests/linux_smoke/run.sh).
*/

#include "host.h"

#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES3/gl3.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

/* ---------- Darwin's, for host_syscall.c on Linux */

int pthread_threadid_np(void *thread, uint64_t *id)
{
	(void)thread;
	*id = (uint64_t)gettid();
	return 0;
}

/* ---------- host_main.m's */

static int frames_wanted = 120;
static int frames;

void host_log(int priority, const char *text)
{
	fprintf(stderr, "%s%s\n", priority >= HOST_LOG_ERROR ? "E " : priority >= HOST_LOG_WARN ? "W " : "  ", text);
}

void host_logf(int priority, const char *format, ...)
{
	char text[2048];
	va_list arguments;

	va_start(arguments, format);
	vsnprintf(text, sizeof(text), format, arguments);
	va_end(arguments);
	host_log(priority, text);
}

void host_fatal(const char *format, ...)
{
	char text[2048];
	va_list arguments;

	va_start(arguments, format);
	vsnprintf(text, sizeof(text), format, arguments);
	va_end(arguments);
	fprintf(stderr, "FATAL %s\n", text);
	fflush(stderr);
	_exit(2);
}

void host_abort(const char *reason)
{
	host_fatal("abort: %s", reason);
}

void host_exit(int code)
{
	fprintf(stderr, "EXIT %d after %d frames\n", code, frames);
	fflush(stderr);
	_exit(code);
}

void host_on_main_thread(void (*function)(void *), void *argument)
{
	function(argument);
}

int host_event_pop(void *event)
{
	(void)event;
	return 0;
}

int host_in_background(void)
{
	return 0;
}

void host_wait_foreground(void)
{
}

static char data_root[512], save_root[600];

void host_android_path(int which, char *buffer, unsigned int size)
{
	snprintf(buffer, size, "%s", which ? save_root : data_root);
}

void host_gesture_insets(int *insets)
{
	memset(insets, 0, 4 * sizeof(int));
}

/* ---------- host_touch.m's */

void host_touch_read(int *state) { memset(state, 0, 7 * sizeof(int)); }
void host_touch_look_read(float *delta) { memset(delta, 0, 4 * sizeof(float)); }
void host_touch_rumble(unsigned int low, unsigned int high) { (void)low; (void)high; }
void host_touch_scene(int scene) { (void)scene; }
void host_touch_bindings(const int *controls) { (void)controls; }
int host_touch_devices(void) { return 1; }
void host_touch_install(void *window) { (void)window; }

/* ---------- host_sdl.c's, as a stand-in */

static EGLDisplay display;
static EGLContext context;

int host_sdl_init(unsigned int flags) { (void)flags; return 1; }
int host_sdl_set_hint(const char *name, const char *value) { (void)name; (void)value; return 1; }
void host_sdl_get_error(char *buffer, unsigned int size) { snprintf(buffer, size, "smoke test"); }

long long host_sdl_ticks(void)
{
	struct timespec now;

	clock_gettime(CLOCK_MONOTONIC, &now);
	return (long long)now.tv_sec * 1000 + now.tv_nsec / 1000000;
}

long long host_sdl_thread_id(void) { return (long long)gettid(); }

unsigned int host_sdl_create_window(const char *title, int width, int height, long long flags)
{
	host_logf(HOST_LOG_INFO, "window \"%s\" %dx%d flags %llx", title, width, height, flags);
	return 1;
}

void host_sdl_window_size_in_pixels(unsigned int window, int *width, int *height)
{
	(void)window;
	*width = 1280;
	*height = 720;
}

int host_sdl_set_relative_mouse(unsigned int window, int enabled) { (void)window; (void)enabled; return 1; }
int host_sdl_gl_set_attribute(int attribute, int value) { (void)attribute; (void)value; return 1; }

unsigned int host_sdl_gl_create_context(unsigned int window)
{
	PFNEGLGETPLATFORMDISPLAYEXTPROC get_display =
		(PFNEGLGETPLATFORMDISPLAYEXTPROC)eglGetProcAddress("eglGetPlatformDisplayEXT");
	static const EGLint config_attributes[] = { EGL_RENDERABLE_TYPE, EGL_OPENGL_ES3_BIT, EGL_NONE };
	static const EGLint context_attributes[] = { EGL_CONTEXT_MAJOR_VERSION, 3, EGL_CONTEXT_MINOR_VERSION, 0, EGL_NONE };
	EGLConfig config;
	EGLint count;

	(void)window;
	display = get_display ? get_display(EGL_PLATFORM_SURFACELESS_MESA, EGL_DEFAULT_DISPLAY, NULL) : EGL_NO_DISPLAY;
	if (display == EGL_NO_DISPLAY || !eglInitialize(display, NULL, NULL))
		host_fatal("no surfaceless EGL display");
	eglBindAPI(EGL_OPENGL_ES_API);
	if (!eglChooseConfig(display, config_attributes, &config, 1, &count) || !count)
		config = (EGLConfig)0; /* EGL_NO_CONFIG_KHR */
	context = eglCreateContext(display, config, EGL_NO_CONTEXT, context_attributes);
	if (!context || !eglMakeCurrent(display, EGL_NO_SURFACE, EGL_NO_SURFACE, context))
		host_fatal("no GL ES 3.0 context");
	/* a framebuffer to draw into, for the default one */
	{
		GLuint framebuffer, color, depth;

		glGenFramebuffers(1, &framebuffer);
		glBindFramebuffer(GL_FRAMEBUFFER, framebuffer);
		glGenRenderbuffers(1, &color);
		glBindRenderbuffer(GL_RENDERBUFFER, color);
		glRenderbufferStorage(GL_RENDERBUFFER, GL_RGBA8, 1280, 720);
		glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_RENDERBUFFER, color);
		glGenRenderbuffers(1, &depth);
		glBindRenderbuffer(GL_RENDERBUFFER, depth);
		glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH24_STENCIL8, 1280, 720);
		glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT, GL_RENDERBUFFER, depth);
	}
	host_logf(HOST_LOG_INFO, "GL: %s / %s", glGetString(GL_RENDERER), glGetString(GL_VERSION));
	return 1;
}

int host_sdl_gl_make_current(unsigned int window, unsigned int context_handle)
{
	(void)window;
	(void)context_handle;
	return 1;
}

int host_sdl_gl_set_swap_interval(int interval) { (void)interval; return 1; }

int host_sdl_gl_swap_window(unsigned int window)
{
	(void)window;
	glFinish();
	if (++frames % 30 == 0)
		host_logf(HOST_LOG_INFO, "frame %d", frames);
	if (frames >= frames_wanted)
		host_exit(0);
	return 1;
}

int host_sdl_poll_event(void *event) { return host_event_pop(event); }
int host_sdl_text_input(unsigned int window, int type) { (void)window; (void)type; return 0; }
int host_sdl_set_clipboard_text(const char *text) { (void)text; return 1; }
void host_sdl_get_clipboard_text(char *buffer, unsigned int size) { if (size) buffer[0] = 0; }
int host_sdl_open_url(const char *url) { (void)url; return 0; }
void host_sdl_scancode_name(int scancode, char *buffer, unsigned int size) { snprintf(buffer, size, "key%d", scancode); }
int host_sdl_scancode_from_name(const char *name) { (void)name; return 0; }

int host_sdl_show_toast(const char *message, int duration, int gravity, int x, int y)
{
	(void)duration; (void)gravity; (void)x; (void)y;
	host_logf(HOST_LOG_INFO, "toast: %s", message);
	return 0;
}

int host_sdl_show_simple_message_box(unsigned int flags, const char *title, const char *message)
{
	host_logf(HOST_LOG_WARN, "message box %x \"%s\": %s", flags, title, message);
	return 1;
}

int host_sdl_show_message_box(unsigned int flags, const char *title, const char *message, int default_index,
	const char *button0, const char *button1, const char *button2)
{
	host_logf(HOST_LOG_WARN, "question %x \"%s\": %s [%s|%s|%s] -> %d", flags, title, message,
		button0 ? button0 : "", button1 ? button1 : "", button2 ? button2 : "", default_index);
	return default_index;
}

int host_sdl_get_gamepads(unsigned int *ids, int capacity) { (void)ids; (void)capacity; return 0; }
unsigned int host_sdl_open_gamepad(unsigned int id) { (void)id; return 0; }
unsigned int host_sdl_gamepad_from_id(unsigned int id) { (void)id; return 0; }
int host_sdl_gamepad_axis(unsigned int gamepad, int axis) { (void)gamepad; (void)axis; return 0; }
int host_sdl_gamepad_button(unsigned int gamepad, int button) { (void)gamepad; (void)button; return 0; }
int host_sdl_gamepad_type(unsigned int gamepad) { (void)gamepad; return 0; }
int host_sdl_rumble_gamepad(unsigned int gamepad, unsigned int low, unsigned int high, unsigned int ms)
{ (void)gamepad; (void)low; (void)high; (void)ms; return 0; }
void host_sdl_close_gamepad(unsigned int gamepad) { (void)gamepad; }

/* audio: a stream that takes whatever it is given, with its callback called
from a thread of ours as SDL would */
static uint32_t audio_callback, audio_userdata;

static void *audio_thread(void *unused)
{
	(void)unused;
	for (;;)
	{
		struct timespec wait = { 0, 20000000 };

		nanosleep(&wait, NULL);
		host_call_guest(audio_callback, audio_userdata, 1, 4096, 4096);
	}
	return NULL;
}

unsigned int host_sdl_open_audio_stream(unsigned int device, const void *spec, unsigned int callback, unsigned int userdata)
{
	(void)device;
	host_logf(HOST_LOG_INFO, "audio stream: format %x, %d channels, %d Hz, callback %u", ((const int *)spec)[0],
		((const int *)spec)[1], ((const int *)spec)[2], callback);
	if (!callback)
		return 0;
	audio_callback = callback;
	audio_userdata = userdata;
	return 1;
}

int host_sdl_put_audio_stream_data(unsigned int stream, const void *data, int length)
{ (void)stream; (void)data; (void)length; return 1; }

int host_sdl_resume_audio_stream_device(unsigned int stream)
{
	(void)stream;
	if (audio_callback && host_native_thread_create(audio_thread, NULL, 256 * 1024) != 0)
		host_fatal("cannot start the audio thread");
	return 1;
}

int host_sdl_get_audio_stream_data(unsigned int stream, void *data, int length) { (void)stream; (void)data; (void)length; return 0; }
int host_sdl_get_audio_stream_available(unsigned int stream) { (void)stream; return 0; }
void host_sdl_destroy_audio_stream(unsigned int stream) { (void)stream; }

/* ---------- start */

static uint32_t make_boot(int count, char **entries)
{
	uint32_t address = host_guest_map(0, 0x10000, 0);
	char *memory = GP(address);
	struct halo_guest_boot *boot = (struct halo_guest_boot *)memory;
	uint32_t *argv = (uint32_t *)(memory + sizeof(*boot));
	uint32_t *environ_list = argv + 2;
	char *strings = (char *)(environ_list + 65);
	int index;

	strcpy(strings, "halo");
	argv[0] = GA(strings);
	argv[1] = 0;
	strings += strlen(strings) + 1;
	for (index = 0; index < count; index++)
	{
		strcpy(strings, entries[index]);
		environ_list[index] = GA(strings);
		strings += strlen(strings) + 1;
	}
	environ_list[count] = 0;
	boot->argc = 1;
	boot->argv = GA(argv);
	boot->environment = GA(environ_list);
	boot->page_size = 4096;
	return address;
}

int main(int argc, char **argv)
{
	char *environment[8];
	char home[700], data[700], save[700];

	if (argc < 2)
	{
		fprintf(stderr, "usage: %s data-root [frames]\n", argv[0]);
		return 1;
	}
	snprintf(data_root, sizeof(data_root), "%s", argv[1]);
	snprintf(save_root, sizeof(save_root), "%s/save", data_root);
	mkdir(save_root, 0755);
	if (argc > 2)
		frames_wanted = atoi(argv[2]);
	snprintf(home, sizeof(home), "HOME=%s", save_root);
	snprintf(data, sizeof(data), "HALO_DATA_ROOT=%s", data_root);
	snprintf(save, sizeof(save), "HALO_SAVE_ROOT=%s", save_root);
	environment[0] = home;
	environment[1] = data;
	environment[2] = save;
	environment[3] = "HALO_DISPLAY_WIDTH=852";
	environment[4] = "HALO_DISPLAY_REFRESH=60";
	environment[5] = "TZ=<L>-07:00";
	host_wasm_initialize();
	host_memory_watch_initialize();
	host_start_guest_main(make_boot(6, environment));
	for (;;)
		pause();
}

/* ---------- host_https.m's, as a stand-in (no master servers here) */
int host_https_get(const char *url, int *status, char *headers, int headers_size, char *body, int body_capacity,
	int *body_size, char *error, int error_size)
{
	(void)url; (void)headers; (void)headers_size; (void)body; (void)body_capacity;
	*status = 0;
	*body_size = 0;
	snprintf(error, (size_t)error_size, "smoke test");
	return 0;
}
