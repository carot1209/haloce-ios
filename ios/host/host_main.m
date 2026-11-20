/*
HOST_MAIN.M

The iOS port's app: SDL runs it with its main callbacks
(SDL_MAIN_USE_CALLBACKS), so that UIKit keeps the main thread, and the game
runs on a thread of its own (host_wasm.c), as on Android.

Storage: the app's Documents folder, which the Files app shows ("On My
iPhone" > the app). The player copies their own Xbox disc image of Halo:
Combat Evolved there; the first start copies its maps folder out
(host_xiso.c). The saves go to save/ there, and config.toml lives there too,
as in OpenCE's other builds.

Logging: to the system log and to halo-log.txt in Documents (the last run's),
which the player can send along with a problem.
*/

#define SDL_MAIN_USE_CALLBACKS 1
#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>

#import <UIKit/UIKit.h>
#import <os/log.h>

#include "host.h"

#include <arpa/inet.h>

#include <dirent.h>
#include <errno.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

/* ---------- logging and termination */

static FILE *log_file;
static pthread_mutex_t log_lock = PTHREAD_MUTEX_INITIALIZER;

void host_log(int priority, const char *text)
{
	os_log_with_type(OS_LOG_DEFAULT, priority >= HOST_LOG_ERROR ? OS_LOG_TYPE_ERROR : OS_LOG_TYPE_DEFAULT,
		"halo: %{public}s", text);
	pthread_mutex_lock(&log_lock);
	if (log_file)
	{
		fprintf(log_file, "%s%s\n", priority >= HOST_LOG_ERROR ? "E " : priority >= HOST_LOG_WARN ? "W " : "",
			text);
		fflush(log_file);
	}
	pthread_mutex_unlock(&log_lock);
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

static void fatal_main(void *message)
{
	SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "Halo", message, NULL);
}

void host_fatal(const char *format, ...)
{
	char message[1024];
	va_list arguments;

	va_start(arguments, format);
	vsnprintf(message, sizeof(message), format, arguments);
	va_end(arguments);
	host_log(HOST_LOG_ERROR, message);
	host_on_main_thread(fatal_main, message);
	_exit(1);
}

void host_abort(const char *reason)
{
	host_fatal("The game stopped: %s", reason);
}

void host_exit(int code)
{
	host_logf(HOST_LOG_INFO, "the game exited (%d)", code);
	_exit(code);
}

/* ---------- the main thread */

struct main_call
{
	void (*function)(void *);
	void *argument;
};

static void SDLCALL main_call_run(void *context)
{
	struct main_call *call = context;

	call->function(call->argument);
}

void host_on_main_thread(void (*function)(void *), void *argument)
{
	struct main_call call = { function, argument };

	if (SDL_IsMainThread())
		function(argument);
	else
		SDL_RunOnMainThread(main_call_run, &call, true);
}

/* ---------- events for the game */

#define EVENT_QUEUE 1024

static SDL_Event event_queue[EVENT_QUEUE];
static unsigned event_head, event_tail;
static pthread_mutex_t event_lock = PTHREAD_MUTEX_INITIALIZER;

static void event_push(const SDL_Event *event)
{
	pthread_mutex_lock(&event_lock);
	if (event_tail - event_head < EVENT_QUEUE)
		event_queue[event_tail++ % EVENT_QUEUE] = *event;
	pthread_mutex_unlock(&event_lock);
}

int host_event_pop(void *event)
{
	int popped = 0;

	pthread_mutex_lock(&event_lock);
	if (event_head != event_tail)
	{
		memcpy(event, &event_queue[event_head++ % EVENT_QUEUE], sizeof(SDL_Event));
		popped = 1;
	}
	pthread_mutex_unlock(&event_lock);
	return popped;
}

/* ---------- the background */

static int in_background;
static pthread_mutex_t background_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t background_change = PTHREAD_COND_INITIALIZER;

static void background_set(int value)
{
	pthread_mutex_lock(&background_lock);
	in_background = value;
	pthread_cond_broadcast(&background_change);
	pthread_mutex_unlock(&background_lock);
}

int host_in_background(void)
{
	return __atomic_load_n(&in_background, __ATOMIC_ACQUIRE);
}

void host_wait_foreground(void)
{
	pthread_mutex_lock(&background_lock);
	while (in_background)
		pthread_cond_wait(&background_change, &background_lock);
	pthread_mutex_unlock(&background_lock);
}

/* ---------- paths */

static char data_root[1024];
static char save_root[1100];

void host_android_path(int which, char *buffer, unsigned int size)
{
	snprintf(buffer, size, "%s", which ? save_root : data_root);
}

/* the screen's edges the home indicator and the camera housing keep, in
pixels of the current orientation: left, top, right, bottom */
static int safe_insets[4];

void host_gesture_insets(int *insets)
{
	memcpy(insets, safe_insets, sizeof(safe_insets));
}

static void safe_insets_update(void)
{
	UIWindow *window = nil;

	for (UIScene *scene in UIApplication.sharedApplication.connectedScenes)
	{
		if ([scene isKindOfClass:UIWindowScene.class])
		{
			for (UIWindow *candidate in ((UIWindowScene *)scene).windows)
			{
				if (candidate.isKeyWindow)
					window = candidate;
			}
		}
	}
	if (window)
	{
		CGFloat scale = window.screen.nativeScale;
		UIEdgeInsets insets = window.safeAreaInsets;

		safe_insets[0] = (int)(insets.left * scale);
		safe_insets[1] = (int)(insets.top * scale);
		safe_insets[2] = (int)(insets.right * scale);
		safe_insets[3] = (int)(insets.bottom * scale);
	}
}

static int directory_has_maps(const char *root)
{
	char path[1200];
	struct stat information;

	snprintf(path, sizeof(path), "%s/maps/ui.map", root);
	return stat(path, &information) == 0;
}

/* a disc image in Documents: the first .iso or .xiso */
static int find_disc_image(const char *root, char *path, size_t size)
{
	DIR *directory = opendir(root);
	struct dirent *entry;
	int found = 0;

	if (!directory)
		return 0;
	while (!found && (entry = readdir(directory)) != NULL)
	{
		const char *dot = strrchr(entry->d_name, '.');

		if (dot && (!strcasecmp(dot, ".iso") || !strcasecmp(dot, ".xiso")))
		{
			snprintf(path, size, "%s/%s", root, entry->d_name);
			found = 1;
		}
	}
	closedir(directory);
	return found;
}

/* ---------- the start-up screen (while the maps are copied) */

static UIWindow *status_window;
static UILabel *status_label;

static void status_show(NSString *text)
{
	dispatch_async(dispatch_get_main_queue(), ^{
		if (!status_window)
		{
			UIWindowScene *scene = nil;
			UIViewController *controller = [[UIViewController alloc] init];

			for (UIScene *candidate in UIApplication.sharedApplication.connectedScenes)
			{
				if ([candidate isKindOfClass:UIWindowScene.class])
					scene = (UIWindowScene *)candidate;
			}
			status_window = scene ? [[UIWindow alloc] initWithWindowScene:scene] :
				[[UIWindow alloc] initWithFrame:UIScreen.mainScreen.bounds];
			controller.view.backgroundColor = UIColor.blackColor;
			status_label = [[UILabel alloc] initWithFrame:CGRectInset(controller.view.bounds, 40, 40)];
			status_label.autoresizingMask = UIViewAutoresizingFlexibleWidth | UIViewAutoresizingFlexibleHeight;
			status_label.textColor = UIColor.whiteColor;
			status_label.numberOfLines = 0;
			status_label.textAlignment = NSTextAlignmentCenter;
			status_label.font = [UIFont systemFontOfSize:17];
			[controller.view addSubview:status_label];
			status_window.rootViewController = controller;
			[status_window makeKeyAndVisible];
		}
		status_label.text = text;
	});
}

static void status_hide(void)
{
	dispatch_sync(dispatch_get_main_queue(), ^{
		status_window.hidden = YES;
		status_window = nil;
		status_label = nil;
	});
}

static void extraction_progress(void *context, unsigned long long done, unsigned long long total)
{
	static int last_percent = -1;
	int percent = total ? (int)(done * 100 / total) : 0;

	(void)context;
	if (percent != last_percent)
	{
		last_percent = percent;
		status_show([NSString stringWithFormat:@"Đang chép thư mục maps từ đĩa game… %d%%\n\n"
			"Copying the maps folder out of the disc image…", percent]);
	}
}

/* ---------- the guest's environment */

#define ENVIRONMENT_MAXIMUM 64

struct environment
{
	char *entries[ENVIRONMENT_MAXIMUM];
	int count;
};

static void environment_set(struct environment *environment, const char *name, const char *value)
{
	size_t length = strlen(name) + strlen(value) + 2;
	char *entry = malloc(length);

	snprintf(entry, length, "%s=%s", name, value);
	if (environment->count < ENVIRONMENT_MAXIMUM)
		environment->entries[environment->count++] = entry;
	else
		free(entry);
}

static void time_zone(char *buffer, size_t size)
{
	time_t now = time(NULL);
	struct tm local;
	long offset;

	localtime_r(&now, &local);
	offset = -local.tm_gmtoff;
	snprintf(buffer, size, "<L>%s%ld:%02ld", offset < 0 ? "-" : "", labs(offset) / 3600, (labs(offset) / 60) % 60);
}

/* argv and the environment, copied into guest memory */
static uint32_t make_boot(const struct environment *environment)
{
	uint32_t size = 0x10000;
	uint32_t address = host_guest_map(0, size, 0);
	char *memory = GP(address);
	struct halo_guest_boot *boot = (struct halo_guest_boot *)memory;
	uint32_t *argv = (uint32_t *)(memory + sizeof(*boot));
	uint32_t *environ_list = argv + 2;
	char *strings = (char *)(environ_list + ENVIRONMENT_MAXIMUM + 1);
	int index;

	if (!address)
		host_fatal("cannot allocate the game's environment");
	strcpy(strings, "halo");
	argv[0] = GA(strings);
	argv[1] = 0;
	strings += strlen(strings) + 1;
	for (index = 0; index < environment->count; index++)
	{
		size_t length = strlen(environment->entries[index]) + 1;

		if (strings + length > memory + size)
			break;
		memcpy(strings, environment->entries[index], length);
		environ_list[index] = GA(strings);
		strings += length;
	}
	environ_list[index] = 0;
	boot->argc = 1;
	boot->argv = GA(argv);
	boot->environment = GA(environ_list);
	boot->page_size = 4096;
	return address;
}

/* the display, read on the main thread for the game's environment */
static int display_width_480, display_refresh;
static float display_density;

static void display_read(void)
{
	const SDL_DisplayMode *mode = SDL_GetDesktopDisplayMode(SDL_GetPrimaryDisplay());

	if (mode && mode->w > 0 && mode->h > 0)
	{
		int longer = mode->w > mode->h ? mode->w : mode->h;
		int shorter = mode->w > mode->h ? mode->h : mode->w;

		/* the game renders 480 lines at the screen's aspect ratio (d3d8_gl.c) */
		display_width_480 = (480 * longer / shorter) & ~1;
		display_refresh = mode->refresh_rate > 0.0f ? (int)(mode->refresh_rate + 0.5f) : 60;
	}
	display_density = SDL_GetDisplayContentScale(SDL_GetPrimaryDisplay());
}

/* system link's broadcasts (network.broadcast, port/linux/src/xnet.c):
since iOS 14 an app without Apple's multicast entitlement cannot
broadcast, so the game's searches and advertisements also go, as unicast,
to every other address of the local network (at most its /24), and to the
addresses the player gave in the Settings app (Settings > Halo CE) */
static void lan_targets(struct environment *environment)
{
	NSUserDefaults *defaults = NSUserDefaults.standardUserDefaults;
	NSString *peers = [defaults stringForKey:@"lan_peers"];
	BOOL scan = [defaults objectForKey:@"lan_scan"] ? [defaults boolForKey:@"lan_scan"] : YES;
	NSMutableString *list = [NSMutableString stringWithString:@"255.255.255.255"];
	uint32_t address, mask, count = 1;

	for (NSString *peer in [peers componentsSeparatedByCharactersInSet:
		[NSCharacterSet characterSetWithCharactersInString:@", ;\n"]])
	{
		struct in_addr parsed;

		if (peer.length && inet_pton(AF_INET, peer.UTF8String, &parsed) == 1 && count < 256)
		{
			[list appendFormat:@",%@", peer];
			count++;
		}
	}
	if (host_local_network(&address, &mask))
	{
		uint32_t host = ntohl(address), net_mask = ntohl(mask), first, last, other;

		/* (at most the /24 around this phone) */
		if (net_mask < 0xffffff00u)
			net_mask = 0xffffff00u;
		first = (host & net_mask) + 1;
		last = (host | ~net_mask) - 1;
		[list appendFormat:@",%u.%u.%u.%u", (host | ~net_mask) >> 24, ((host | ~net_mask) >> 16) & 255,
			((host | ~net_mask) >> 8) & 255, (host | ~net_mask) & 255];
		count++;
		for (other = first; scan && other <= last && count < 256; other++)
		{
			if (other == host)
				continue;
			[list appendFormat:@",%u.%u.%u.%u", other >> 24, (other >> 16) & 255, (other >> 8) & 255, other & 255];
			count++;
		}
		host_logf(HOST_LOG_INFO, "LAN: this phone is %u.%u.%u.%u, %u broadcast targets", host >> 24,
			(host >> 16) & 255, (host >> 8) & 255, host & 255, (unsigned)count);
	}
	else
	{
		host_logf(HOST_LOG_INFO, "LAN: no local network (Wi-Fi off?)");
	}
	environment_set(environment, "HALO_NET_BROADCAST", list.UTF8String);
}

/* the start, on a thread of its own: the maps, then the game */
static void *start_thread(void *unused)
{
	struct environment environment = { { 0 }, 0 };
	char text[64], zone[64], path[1300];

	(void)unused;
	if (!directory_has_maps(data_root))
	{
		char image[1200], error[512];

		if (!find_disc_image(data_root, image, sizeof(image)))
		{
			host_fatal("Chưa có dữ liệu game.\n\nHãy chép file đĩa Xbox (.iso) của Halo: Combat Evolved vào thư mục "
				"của app (app Tệp > Trên iPhone > Halo CE), rồi mở lại app.\n\n"
				"Copy your own Xbox disc image (.iso) of Halo: Combat Evolved into the app's folder "
				"(Files > On My iPhone > Halo CE) and open the app again.");
		}
		host_logf(HOST_LOG_INFO, "copying maps/ out of %s", image);
		extraction_progress(NULL, 0, 1);
		if (!host_xiso_extract_maps(image, data_root, extraction_progress, NULL, error, sizeof(error)))
			host_fatal("%s", error);
		host_logf(HOST_LOG_INFO, "maps copied; the disc image can be deleted now");
	}
	if (status_window)
		status_hide();

	environment_set(&environment, "HOME", save_root);
	environment_set(&environment, "HALO_DATA_ROOT", data_root);
	environment_set(&environment, "HALO_SAVE_ROOT", save_root);
	if (display_width_480)
	{
		snprintf(text, sizeof(text), "%d", display_width_480);
		environment_set(&environment, "HALO_DISPLAY_WIDTH", text);
		snprintf(text, sizeof(text), "%d", display_refresh);
		environment_set(&environment, "HALO_DISPLAY_REFRESH", text);
	}
	if (display_density > 0.0f)
	{
		snprintf(text, sizeof(text), "%g", display_density);
		environment_set(&environment, "HALO_DISPLAY_DENSITY", text);
	}
	time_zone(zone, sizeof(zone));
	environment_set(&environment, "TZ", zone);
	lan_targets(&environment);

	/* internet play's brokers list, from the app bundle, beside config.toml */
	{
		NSString *bundled = [NSBundle.mainBundle pathForResource:@"brokers" ofType:@"txt"];

		snprintf(path, sizeof(path), "%s/brokers.txt", data_root);
		if (bundled)
		{
			NSData *data = [NSData dataWithContentsOfFile:bundled];

			[data writeToFile:[NSString stringWithUTF8String:path] atomically:YES];
		}
	}

	host_memory_watch_initialize();
	host_logf(HOST_LOG_INFO, "data %s, saves %s, rendering %dx480 at %d Hz", data_root, save_root,
		display_width_480, display_refresh);
	host_start_guest_main(make_boot(&environment));
	return NULL;
}

/* ---------- SDL's callbacks */

SDL_AppResult SDL_AppInit(void **appstate, int argc, char *argv[])
{
	NSString *documents = NSSearchPathForDirectoriesInDomains(NSDocumentDirectory, NSUserDomainMask, YES).firstObject;
	pthread_attr_t attributes;
	pthread_t thread;
	char path[1200];

	(void)appstate;
	(void)argc;
	(void)argv;
	snprintf(data_root, sizeof(data_root), "%s", documents.fileSystemRepresentation);
	snprintf(save_root, sizeof(save_root), "%s/save", data_root);
	mkdir(save_root, 0755);
	snprintf(path, sizeof(path), "%s/halo-log.txt", data_root);
	log_file = fopen(path, "w");
	host_logf(HOST_LOG_INFO, "Halo CE for iOS (OpenCE) starting");

	SDL_SetHint(SDL_HINT_ORIENTATIONS, "LandscapeLeft LandscapeRight");
	SDL_SetHint(SDL_HINT_IOS_HIDE_HOME_INDICATOR, "2");
	SDL_SetHint(SDL_HINT_TOUCH_MOUSE_EVENTS, "0");
	if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_GAMEPAD | SDL_INIT_AUDIO))
		host_logf(HOST_LOG_ERROR, "SDL_Init: %s", SDL_GetError());
	display_read();

	host_wasm_initialize();

	pthread_attr_init(&attributes);
	pthread_attr_setdetachstate(&attributes, PTHREAD_CREATE_DETACHED);
	pthread_attr_setstacksize(&attributes, 1024 * 1024);
	if (pthread_create(&thread, &attributes, start_thread, NULL) != 0)
		host_fatal("cannot start");
	pthread_attr_destroy(&attributes);
	return SDL_APP_CONTINUE;
}

SDL_AppResult SDL_AppIterate(void *appstate)
{
	static int frames;

	(void)appstate;
	if (frames++ % 30 == 0)
		safe_insets_update();
	return SDL_APP_CONTINUE;
}

SDL_AppResult SDL_AppEvent(void *appstate, SDL_Event *event)
{
	(void)appstate;
	switch (event->type)
	{
	case SDL_EVENT_WILL_ENTER_BACKGROUND:
	case SDL_EVENT_DID_ENTER_BACKGROUND:
		background_set(1);
		break;
	case SDL_EVENT_WILL_ENTER_FOREGROUND:
	case SDL_EVENT_DID_ENTER_FOREGROUND:
		background_set(0);
		break;
	case SDL_EVENT_TERMINATING:
		host_log(HOST_LOG_INFO, "iOS is ending the app");
		break;
	default:
		break;
	}
	event_push(event);
	return SDL_APP_CONTINUE;
}

void SDL_AppQuit(void *appstate, SDL_AppResult result)
{
	(void)appstate;
	(void)result;
}
