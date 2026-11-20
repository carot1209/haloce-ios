/*
HOST_TOUCH.M

The on-screen touch controls of the iOS port: a UIKit view over SDL's OpenGL
view, in place of the Android port's TouchControls.java, with the same
contract towards the game (port/linux/src/touch_input.c through
port/android/guest/runtime/guest_host.h's host_touch_*):

- the game reads the overlay's controller (SDL's axes and button bits), its
  view swipe and its gyroscope turn each frame, and says when the overlay is
  to show (in a game, with no menu or cinematic up, and input.touch_controls
  not "off"; with "auto", only while no game controller is connected);
- hidden, the overlay lets every finger through to SDL's view, whose finger
  events become the menus' pointer.

The layout is a mobile shooter's: a stick that follows the left thumb, the
view turned by dragging anywhere else, a large fire button on the right (and
one on the left) that also aims while it is held, zoom by it, jump and crouch
at the bottom right, Halo's action button (reload; held: pick up, board,
flip a vehicle, press a switch) in the middle. Each button shows what the
profile's mapping has it do. The gear opens the settings (sensitivity, the
gyroscope, size, opacity, crouch held or toggled, rumble) and the layout
editor, where each control is dragged, sized or hidden; all of it is saved.
*/

#import <UIKit/UIKit.h>
#import <GameController/GameController.h>
#import <CoreMotion/CoreMotion.h>

#include "host.h"

#include <SDL3/SDL.h>
#include <math.h>
#include <pthread.h>
#include <string.h>
#include <time.h>

/* (host_touch.c's) */
#define TOUCH_TAP_NS 60000000LL
#define TOUCH_STALE_NS 150000000LL
#define TOUCH_INPUTS 17
#define TOUCH_BUTTONS 16
#define HOST_TOUCH_DEVICE_TOUCHSCREEN 1
#define HOST_TOUCH_DEVICE_CONTROLLER 2

enum
{
	_touch_scene_known = 1 << 0,
	_touch_scene_menus = 1 << 1,
	_touch_scene_on = 1 << 2,
	_touch_scene_off = 1 << 3,
};

/* SDL_GamepadButton, and the two triggers after them */
enum
{
	_input_a, _input_b, _input_x, _input_y, _input_back, _input_guide, _input_start,
	_input_left_stick, _input_right_stick, _input_left_shoulder, _input_right_shoulder,
	_input_dpad_up, _input_dpad_down, _input_dpad_left, _input_dpad_right,
	_input_left_trigger, _input_right_trigger,
};

static pthread_mutex_t touch_lock = PTHREAD_MUTEX_INITIALIZER;
static int32_t touch_state[7];
static int64_t touch_pressed_ns[TOUCH_INPUTS];
static float look_delta[2];
static int64_t look_ns;
static float gyro_delta[2];
static int64_t gyro_ns;
static int rumble_amplitude;
static int64_t rumble_ns;
static volatile int touch_scene;
static volatile int touch_devices = HOST_TOUCH_DEVICE_TOUCHSCREEN;
static int32_t touch_bindings[TOUCH_BUTTONS];
static volatile int touch_bindings_serial;

static int64_t now_ns(void)
{
	struct timespec now;

	clock_gettime(CLOCK_MONOTONIC, &now);
	return (int64_t)now.tv_sec * 1000000000LL + now.tv_nsec;
}

static uint32_t touch_inputs(const int32_t *state)
{
	return ((uint32_t)state[6] & 0x7fff) | (state[4] ? 1u << 15 : 0) | (state[5] ? 1u << 16 : 0);
}

static void state_set(const int32_t *next)
{
	uint32_t pressed;
	int64_t now = now_ns();
	int input;

	pthread_mutex_lock(&touch_lock);
	pressed = touch_inputs(next) & ~touch_inputs(touch_state);
	for (input = 0; input < TOUCH_INPUTS; input++)
	{
		if (pressed & (1u << input))
			touch_pressed_ns[input] = now;
	}
	memcpy(touch_state, next, sizeof(touch_state));
	pthread_mutex_unlock(&touch_lock);
}

/* ---------- the guest's (guest_host.h) */

void host_touch_read(int *state)
{
	int64_t now = now_ns();
	int input;

	pthread_mutex_lock(&touch_lock);
	memcpy(state, touch_state, sizeof(touch_state));
	for (input = 0; input < TOUCH_INPUTS; input++)
	{
		if (touch_pressed_ns[input] && now - touch_pressed_ns[input] < TOUCH_TAP_NS)
		{
			if (input < 15)
				state[6] |= 1 << input;
			else if (!state[4 + input - 15])
				state[4 + input - 15] = 32767;
		}
	}
	pthread_mutex_unlock(&touch_lock);
}

/* the swipe, then the gyroscope (which the profile's invert leaves alone),
in the mouse's units */
void host_touch_look_read(float *delta)
{
	int64_t now = now_ns();

	pthread_mutex_lock(&touch_lock);
	delta[0] = look_delta[0];
	delta[1] = look_delta[1];
	delta[2] = gyro_delta[0];
	delta[3] = gyro_delta[1];
	if (now - look_ns > TOUCH_STALE_NS)
		delta[0] = delta[1] = 0.0f;
	if (now - gyro_ns > TOUCH_STALE_NS)
		delta[2] = delta[3] = 0.0f;
	look_delta[0] = look_delta[1] = 0.0f;
	gyro_delta[0] = gyro_delta[1] = 0.0f;
	pthread_mutex_unlock(&touch_lock);
}

void host_touch_rumble(unsigned int low, unsigned int high)
{
	pthread_mutex_lock(&touch_lock);
	rumble_amplitude = (int)(low > high ? low : high);
	rumble_ns = now_ns();
	pthread_mutex_unlock(&touch_lock);
}

void host_touch_scene(int scene)
{
	touch_scene = scene;
}

void host_touch_bindings(const int *controls)
{
	pthread_mutex_lock(&touch_lock);
	memcpy(touch_bindings, controls, sizeof(touch_bindings));
	touch_bindings_serial++;
	pthread_mutex_unlock(&touch_lock);
}

int host_touch_devices(void)
{
	return touch_devices;
}

/* ---------- the controls */

enum
{
	/* a finger that holds it also turns the view as it drags */
	_kind_aims = 1 << 0,
	/* a tap holds it, the next lets it go (if the setting says so) */
	_kind_toggle = 1 << 1,
	/* the move stick */
	_kind_stick = 1 << 2,
};

/* the controls, their default places on a 960 x 540 grid over the safe area,
and radii in points */
static const struct kind
{
	const char *key;
	int input;
	/* the game's controller button (input.h's _gamepad_*): what the profile
	maps it to names it */
	int gamepad;
	float x, y, radius;
	int flags;
} kinds[] =
{
	{ "move", -1, -1, 150, 400, 66, _kind_stick },
	{ "fire", _input_right_trigger, 7, 862, 328, 46, _kind_aims },
	{ "fire_left", _input_right_trigger, 7, 108, 200, 34, _kind_aims },
	{ "zoom", _input_right_stick, 15, 868, 192, 30, _kind_aims },
	{ "jump", _input_a, 0, 908, 470, 31, 0 },
	{ "crouch", _input_left_stick, 14, 800, 486, 28, _kind_toggle },
	{ "melee", _input_b, 1, 752, 404, 29, 0 },
	{ "action", _input_x, 2, 636, 330, 34, 0 },
	{ "grenade", _input_left_trigger, 6, 742, 252, 29, 0 },
	{ "grenade_type", _input_right_shoulder, 4, 790, 160, 23, 0 },
	{ "weapon", _input_y, 3, 520, 482, 30, 0 },
	{ "light", _input_left_shoulder, 5, 912, 72, 22, 0 },
	{ "pause", _input_start, 12, 530, 34, 20, 0 },
	{ "score", _input_back, 13, 430, 34, 20, 0 },
};
#define KIND_COUNT ((int)(sizeof(kinds) / sizeof(kinds[0])))

/* the game's controls (input_abstraction.c's _game_control_*): a name and an
SF Symbol */
static NSString *const control_names[] =
{
	@"Nhảy", @"Đổi lựu", @"Nạp / Dùng", @"Đổi súng", @"Đánh", @"Đèn",
	@"Lựu đạn", @"Bắn", @"Dừng", @"Điểm", @"Ngồi", @"Ngắm",
};
static NSString *const control_symbols[] =
{
	@"arrow.up.to.line", @"arrow.triangle.2.circlepath", @"hand.point.up.left.fill", @"arrow.left.arrow.right",
	@"hand.raised.fill", @"flashlight.on.fill", @"circle.circle.fill", @"scope", @"pause.fill",
	@"list.bullet", @"arrow.down.to.line", @"plus.magnifyingglass",
};
/* the default profile's mapping (TouchControls.java's), until the game's */
static const int default_bindings[TOUCH_BUTTONS] = { 0, 4, 2, 3, 1, 5, 6, 7, -1, -1, -1, -1, 8, 9, 10, 11 };
static NSString *const button_names[TOUCH_BUTTONS] =
{
	@"A", @"B", @"X", @"Y", @"Đen", @"Trắng", @"LT", @"RT", @"↑", @"↓", @"←", @"→", @"Start", @"Back", @"L3", @"R3",
};

/* radians of turn for a unit of swipe (xinput_sdl.c's mouse rate) */
static const float LOOK_RADIANS_PER_UNIT = 0.0022f;
static const float STICK_DEAD_ZONE = 0.12f;
/* the stick follows a thumb that lands in this part of the screen */
static const CGFloat STICK_ZONE_WIDTH = 0.45;
static const CGFloat STICK_ZONE_TOP = 0.3;

/* the settings (NSUserDefaults) */
static float setting_sensitivity = 1.0f;
static float setting_gyro_sensitivity = 1.0f;
static float setting_opacity = 0.75f;
static float setting_size = 1.0f;
static BOOL setting_gyro = NO;
static BOOL setting_gyro_invert = NO;
static BOOL setting_crouch_toggle = YES;
static BOOL setting_rumble = YES;

#define DEFAULTS_PREFIX @"halo.touch."

enum
{
	_tag_sensitivity = 1, _tag_gyro_sensitivity, _tag_opacity, _tag_size,
	_tag_gyro = 11, _tag_gyro_invert, _tag_crouch_toggle, _tag_rumble, _tag_left_fire,
};

/* each finger's role: a control's index, or one of these */
enum
{
	_role_stick = -1,
	_role_look = -2,
	_role_none = -3,
};

@interface HaloControl : UIView
@property (nonatomic) int kind;
/* the place on the grid, the player's size for it, and whether it is off */
@property (nonatomic) CGFloat gridX, gridY, scale;
@property (nonatomic) BOOL off;
@property (nonatomic) BOOL toggled;
@property (nonatomic) BOOL down;
@property (nonatomic, strong) UIImageView *icon;
@property (nonatomic, strong) UILabel *label;
@end

@implementation HaloControl
@end

@interface HaloTouchOverlay : UIView
@end

@implementation HaloTouchOverlay
{
	NSMutableArray<HaloControl *> *controls;
	NSMapTable<UITouch *, NSNumber *> *roles;
	int held[TOUCH_INPUTS];
	int32_t state[7];
	UITouch *stickTouch;
	CGPoint stickCenter;
	UIView *stickKnob;
	UIButton *gear;
	CADisplayLink *link;
	int bindingsSerial;
	int64_t hapticNs;
	UIImpactFeedbackGenerator *haptics;
	CMMotionManager *motion;
	NSTimeInterval motionTime;
	/* the settings panel, and the layout editor's toolbar and selection */
	UIView *panel;
	UIView *toolbar;
	BOOL editing;
	HaloControl *selected;
	CGPoint dragOffset;
}

/* ---------- the settings, saved */

- (void)loadSettings
{
	NSUserDefaults *defaults = NSUserDefaults.standardUserDefaults;
	NSDictionary *layout = [defaults dictionaryForKey:DEFAULTS_PREFIX @"layout"];

	if ([defaults objectForKey:DEFAULTS_PREFIX @"sensitivity"])
	{
		setting_sensitivity = [defaults floatForKey:DEFAULTS_PREFIX @"sensitivity"];
		setting_gyro_sensitivity = [defaults floatForKey:DEFAULTS_PREFIX @"gyro_sensitivity"];
		setting_opacity = [defaults floatForKey:DEFAULTS_PREFIX @"opacity"];
		setting_size = [defaults floatForKey:DEFAULTS_PREFIX @"size"];
		setting_gyro = [defaults boolForKey:DEFAULTS_PREFIX @"gyro"];
		setting_gyro_invert = [defaults boolForKey:DEFAULTS_PREFIX @"gyro_invert"];
		setting_crouch_toggle = [defaults boolForKey:DEFAULTS_PREFIX @"crouch_toggle"];
		setting_rumble = [defaults boolForKey:DEFAULTS_PREFIX @"rumble"];
	}
	for (HaloControl *control in controls)
	{
		NSArray *saved = layout[@(kinds[control.kind].key)];

		control.gridX = kinds[control.kind].x;
		control.gridY = kinds[control.kind].y;
		control.scale = 1.0;
		control.off = NO;
		if ([saved isKindOfClass:NSArray.class] && saved.count == 4)
		{
			control.gridX = [saved[0] doubleValue];
			control.gridY = [saved[1] doubleValue];
			control.scale = [saved[2] doubleValue];
			control.off = [saved[3] boolValue] && !(kinds[control.kind].flags & _kind_stick);
		}
	}
}

- (void)saveSettings
{
	NSUserDefaults *defaults = NSUserDefaults.standardUserDefaults;
	NSMutableDictionary *layout = [NSMutableDictionary dictionary];

	for (HaloControl *control in controls)
		layout[@(kinds[control.kind].key)] = @[ @(control.gridX), @(control.gridY), @(control.scale), @(control.off) ];
	[defaults setObject:layout forKey:DEFAULTS_PREFIX @"layout"];
	[defaults setFloat:setting_sensitivity forKey:DEFAULTS_PREFIX @"sensitivity"];
	[defaults setFloat:setting_gyro_sensitivity forKey:DEFAULTS_PREFIX @"gyro_sensitivity"];
	[defaults setFloat:setting_opacity forKey:DEFAULTS_PREFIX @"opacity"];
	[defaults setFloat:setting_size forKey:DEFAULTS_PREFIX @"size"];
	[defaults setBool:setting_gyro forKey:DEFAULTS_PREFIX @"gyro"];
	[defaults setBool:setting_gyro_invert forKey:DEFAULTS_PREFIX @"gyro_invert"];
	[defaults setBool:setting_crouch_toggle forKey:DEFAULTS_PREFIX @"crouch_toggle"];
	[defaults setBool:setting_rumble forKey:DEFAULTS_PREFIX @"rumble"];
}

/* ---------- making and placing the controls */

- (HaloControl *)controlNamed:(NSString *)key
{
	for (HaloControl *control in controls)
	{
		if (!strcmp(kinds[control.kind].key, key.UTF8String))
			return control;
	}
	return nil;
}

- (CGFloat)radiusOf:(HaloControl *)control
{
	return kinds[control.kind].radius * control.scale * setting_size;
}

/* the safe area, a little inside it */
- (CGRect)area
{
	return CGRectInset(UIEdgeInsetsInsetRect(self.bounds, self.safeAreaInsets), 6, 4);
}

- (CGPoint)homeOf:(HaloControl *)control
{
	CGRect area = [self area];

	return CGPointMake(area.origin.x + control.gridX / 960.0 * area.size.width,
		area.origin.y + control.gridY / 540.0 * area.size.height);
}

- (void)relabel
{
	int bindings[TOUCH_BUTTONS];

	pthread_mutex_lock(&touch_lock);
	memcpy(bindings, touch_bindings_serial ? touch_bindings : default_bindings, sizeof(bindings));
	bindingsSerial = touch_bindings_serial;
	pthread_mutex_unlock(&touch_lock);
	for (HaloControl *control in controls)
	{
		int gamepad = kinds[control.kind].gamepad;
		int game = gamepad >= 0 ? bindings[gamepad] : -1;
		UIImage *image = nil;

		if (kinds[control.kind].flags & _kind_stick)
			continue;
		if (game >= 0 && game < (int)(sizeof(control_names) / sizeof(control_names[0])))
		{
			control.label.text = control_names[game];
			image = [UIImage systemImageNamed:control_symbols[game]];
		}
		else
		{
			control.label.text = gamepad >= 0 ? button_names[gamepad] : @"";
		}
		control.icon.image = image;
		control.icon.hidden = image == nil;
	}
}

- (void)styleControl:(HaloControl *)control
{
	BOOL stick = (kinds[control.kind].flags & _kind_stick) != 0;
	BOOL lit = control.down || control.toggled || control == selected;
	CGFloat radius = [self radiusOf:control];

	control.bounds = CGRectMake(0, 0, radius * 2, radius * 2);
	control.center = (stick && stickTouch && !editing) ? stickCenter : [self homeOf:control];
	control.layer.cornerRadius = radius;
	control.layer.borderWidth = control == selected ? 3 : 1.5;
	control.layer.borderColor = (control == selected ? [UIColor colorWithRed:0.4 green:0.85 blue:1 alpha:1] :
		[UIColor colorWithWhite:1.0 alpha:0.55]).CGColor;
	control.backgroundColor = [UIColor colorWithWhite:lit ? 0.85 : 0.0 alpha:lit ? 0.45 : (stick ? 0.12 : 0.28)];
	control.alpha = (control.off ? (editing ? 0.3 : 0.0) : setting_opacity);
	control.hidden = control.off && !editing;
	control.icon.frame = CGRectInset(control.bounds, radius * 0.48, radius * 0.48);
	control.label.frame = CGRectMake(-24, radius * 2 + 1, radius * 2 + 48, 13);
	control.label.hidden = stick || radius < 20;
}

- (void)layoutSubviews
{
	[super layoutSubviews];
	for (HaloControl *control in controls)
		[self styleControl:control];
	[self placeKnob];
	gear.center = CGPointMake(CGRectGetMinX([self area]) + 22, CGRectGetMinY([self area]) + 20);
	if (panel)
		panel.center = CGPointMake(CGRectGetMidX(self.bounds), CGRectGetMidY(self.bounds));
	if (toolbar)
		toolbar.center = CGPointMake(CGRectGetMidX(self.bounds), CGRectGetMinY([self area]) + 26);
}

- (void)placeKnob
{
	HaloControl *stick = [self controlNamed:@"move"];
	CGFloat knob = [self radiusOf:stick] * 0.42;

	stickKnob.bounds = CGRectMake(0, 0, knob * 2, knob * 2);
	stickKnob.layer.cornerRadius = knob;
	stickKnob.alpha = setting_opacity;
	if (!stickTouch || editing)
		stickKnob.center = stick.center;
	stickKnob.hidden = editing;
}

- (instancetype)initWithFrame:(CGRect)frame
{
	int index;

	self = [super initWithFrame:frame];
	if (!self)
		return nil;
	self.multipleTouchEnabled = YES;
	self.backgroundColor = UIColor.clearColor;
	self.autoresizingMask = UIViewAutoresizingFlexibleWidth | UIViewAutoresizingFlexibleHeight;
	controls = [NSMutableArray array];
	roles = [NSMapTable strongToStrongObjectsMapTable];
	for (index = 0; index < KIND_COUNT; index++)
	{
		HaloControl *control = [[HaloControl alloc] initWithFrame:CGRectZero];

		control.kind = index;
		control.userInteractionEnabled = NO;
		control.icon = [[UIImageView alloc] init];
		control.icon.contentMode = UIViewContentModeScaleAspectFit;
		control.icon.tintColor = UIColor.whiteColor;
		[control addSubview:control.icon];
		control.label = [[UILabel alloc] init];
		control.label.textAlignment = NSTextAlignmentCenter;
		control.label.textColor = UIColor.whiteColor;
		control.label.font = [UIFont boldSystemFontOfSize:10];
		control.label.shadowColor = UIColor.blackColor;
		control.label.shadowOffset = CGSizeMake(0, 1);
		[control addSubview:control.label];
		[self addSubview:control];
		[controls addObject:control];
	}
	stickKnob = [[UIView alloc] init];
	stickKnob.userInteractionEnabled = NO;
	stickKnob.backgroundColor = [UIColor colorWithWhite:1.0 alpha:0.5];
	[self addSubview:stickKnob];

	gear = [UIButton buttonWithType:UIButtonTypeSystem];
	gear.frame = CGRectMake(0, 0, 40, 36);
	[gear setImage:[UIImage systemImageNamed:@"gearshape.fill"] forState:UIControlStateNormal];
	gear.tintColor = [UIColor colorWithWhite:1.0 alpha:0.8];
	gear.backgroundColor = [UIColor colorWithWhite:0.0 alpha:0.3];
	gear.layer.cornerRadius = 10;
	[gear addTarget:self action:@selector(openSettings) forControlEvents:UIControlEventTouchUpInside];
	[self addSubview:gear];

	haptics = [[UIImpactFeedbackGenerator alloc] initWithStyle:UIImpactFeedbackStyleHeavy];
	motion = [[CMMotionManager alloc] init];
	motion.deviceMotionUpdateInterval = 1.0 / 60.0;

	[self loadSettings];
	[self relabel];

	link = [CADisplayLink displayLinkWithTarget:self selector:@selector(refresh:)];
	link.preferredFramesPerSecond = 30;
	[link addToRunLoop:NSRunLoop.mainRunLoop forMode:NSRunLoopCommonModes];
	return self;
}

/* ---------- playing */

- (void)publish
{
	int input;

	memset(state, 0, sizeof(state));
	if (!panel && !editing)
	{
		HaloControl *stick = [self controlNamed:@"move"];

		if (stickTouch)
		{
			CGFloat radius = [self radiusOf:stick];
			CGPoint point = stickKnob.center;
			CGFloat dx = (point.x - stickCenter.x) / radius, dy = (point.y - stickCenter.y) / radius;
			CGFloat length = sqrt(dx * dx + dy * dy);

			if (length > STICK_DEAD_ZONE)
			{
				/* (the dead zone taken out, so the walk starts slow) */
				CGFloat scaled = fmin(1.0, (length - STICK_DEAD_ZONE) / (1.0 - STICK_DEAD_ZONE)) / length;

				state[0] = (int32_t)(dx * scaled * 32767);
				state[1] = (int32_t)(dy * scaled * 32767);
			}
		}
		for (input = 0; input < 15; input++)
		{
			if (held[input] > 0)
				state[6] |= 1 << input;
		}
		state[4] = held[_input_left_trigger] > 0 ? 32767 : 0;
		state[5] = held[_input_right_trigger] > 0 ? 32767 : 0;
	}
	state_set(state);
}

- (void)setControl:(HaloControl *)control down:(BOOL)down
{
	int input = kinds[control.kind].input;

	control.down = down;
	if (input >= 0)
		held[input] += down ? 1 : -1;
	if (input >= 0 && held[input] < 0)
		held[input] = 0;
	[self styleControl:control];
}

/* lets go of everything the fingers hold (crouch's toggle too) */
- (void)releaseAll
{
	for (HaloControl *control in controls)
	{
		control.down = NO;
		control.toggled = NO;
		[self styleControl:control];
	}
	memset(held, 0, sizeof(held));
	[roles removeAllObjects];
	stickTouch = nil;
	[self placeKnob];
	[self publish];
}

- (HaloControl *)controlAt:(CGPoint)point all:(BOOL)all
{
	HaloControl *best = nil;
	CGFloat bestDistance = 0;

	for (HaloControl *control in controls)
	{
		CGFloat radius = [self radiusOf:control] * 1.12 + 4;
		CGFloat dx = point.x - control.center.x, dy = point.y - control.center.y;
		CGFloat distance = dx * dx + dy * dy;

		if ((control.off && !all) || ((kinds[control.kind].flags & _kind_stick) && !all))
			continue;
		if (distance <= radius * radius && (!best || distance < bestDistance))
		{
			best = control;
			bestDistance = distance;
		}
	}
	return best;
}

- (void)look:(UITouch *)touch
{
	CGPoint point = [touch locationInView:self];
	CGPoint previous = [touch previousLocationInView:self];
	/* in the units of a 540-unit-high screen, as Android's */
	float scale = 540.0f / (float)fmax(self.bounds.size.height, 1.0) * setting_sensitivity;

	pthread_mutex_lock(&touch_lock);
	look_delta[0] += (float)(point.x - previous.x) * scale;
	look_delta[1] += (float)(point.y - previous.y) * scale;
	look_ns = now_ns();
	pthread_mutex_unlock(&touch_lock);
}

/* where a thumb takes the stick: the lower left, or on the stick where it
was moved to */
- (BOOL)inStickZone:(CGPoint)point
{
	HaloControl *stick = [self controlNamed:@"move"];
	CGPoint home = [self homeOf:stick];
	CGFloat reach = [self radiusOf:stick] * 1.3;
	CGFloat dx = point.x - home.x, dy = point.y - home.y;

	if (dx * dx + dy * dy <= reach * reach)
		return YES;
	return point.x < self.bounds.size.width * STICK_ZONE_WIDTH && point.y > self.bounds.size.height * STICK_ZONE_TOP;
}

- (void)moveStick:(CGPoint)point
{
	HaloControl *stick = [self controlNamed:@"move"];
	CGFloat radius = [self radiusOf:stick];
	CGFloat dx = point.x - stickCenter.x, dy = point.y - stickCenter.y;
	CGFloat length = sqrt(dx * dx + dy * dy);

	if (length > radius)
	{
		dx *= radius / length;
		dy *= radius / length;
	}
	stickKnob.center = CGPointMake(stickCenter.x + dx, stickCenter.y + dy);
}

- (void)touchesBegan:(NSSet<UITouch *> *)touches withEvent:(UIEvent *)event
{
	if (editing)
	{
		[self editBegan:touches];
		return;
	}
	for (UITouch *touch in touches)
	{
		CGPoint point = [touch locationInView:self];
		HaloControl *control = panel ? nil : [self controlAt:point all:NO];

		if (panel)
		{
			[roles setObject:@(_role_none) forKey:touch];
		}
		else if (control && (kinds[control.kind].flags & _kind_toggle) && setting_crouch_toggle)
		{
			control.toggled = !control.toggled;
			[self setControl:control down:control.toggled];
			[roles setObject:@(_role_none) forKey:touch];
		}
		else if (control)
		{
			[self setControl:control down:YES];
			[roles setObject:@(control.kind) forKey:touch];
		}
		else if (!stickTouch && [self inStickZone:point])
		{
			HaloControl *stick = [self controlNamed:@"move"];

			stickTouch = touch;
			stickCenter = point;
			[self styleControl:stick];
			stickKnob.center = point;
			[roles setObject:@(_role_stick) forKey:touch];
		}
		else
		{
			[roles setObject:@(_role_look) forKey:touch];
		}
	}
	[self publish];
}

- (void)touchesMoved:(NSSet<UITouch *> *)touches withEvent:(UIEvent *)event
{
	if (editing)
	{
		[self editMoved:touches];
		return;
	}
	for (UITouch *touch in touches)
	{
		NSNumber *role = [roles objectForKey:touch];

		if (!role)
			continue;
		if (role.intValue == _role_stick)
			[self moveStick:[touch locationInView:self]];
		else if (role.intValue == _role_look ||
			(role.intValue >= 0 && (kinds[role.intValue].flags & _kind_aims)))
			[self look:touch];
	}
	[self publish];
}

- (void)touchesEnded:(NSSet<UITouch *> *)touches withEvent:(UIEvent *)event
{
	if (editing)
	{
		[self editEnded:touches];
		return;
	}
	for (UITouch *touch in touches)
	{
		NSNumber *role = [roles objectForKey:touch];

		if (!role)
			continue;
		if (role.intValue == _role_stick)
		{
			stickTouch = nil;
			[self styleControl:[self controlNamed:@"move"]];
			[self placeKnob];
		}
		else if (role.intValue >= 0)
		{
			HaloControl *control = controls[role.intValue];

			[self setControl:control down:NO];
		}
		[roles removeObjectForKey:touch];
	}
	[self publish];
}

- (void)touchesCancelled:(NSSet<UITouch *> *)touches withEvent:(UIEvent *)event
{
	[self touchesEnded:touches withEvent:event];
}

/* ---------- the gyroscope: the phone turned as the view, while it shows */

- (void)gyroscope:(BOOL)on
{
	if (!motion.deviceMotionAvailable)
		return;
	if (on && !motion.deviceMotionActive)
	{
		__weak HaloTouchOverlay *weakSelf = self;

		motionTime = 0;
		[motion startDeviceMotionUpdatesToQueue:NSOperationQueue.mainQueue
			withHandler:^(CMDeviceMotion *sample, NSError *error) {
				[weakSelf gyroscopeSample:sample];
			}];
	}
	else if (!on && motion.deviceMotionActive)
	{
		[motion stopDeviceMotionUpdates];
	}
}

- (void)gyroscopeSample:(CMDeviceMotion *)sample
{
	UIInterfaceOrientation orientation = self.window.windowScene.interfaceOrientation;
	NSTimeInterval elapsed = motionTime > 0 ? sample.timestamp - motionTime : 0;
	CMRotationRate rate = sample.rotationRate;
	double yaw, pitch;
	float scale;

	motionTime = sample.timestamp;
	if (elapsed <= 0 || elapsed > 0.1)
		return;
	/* the device's axes as the screen's: in landscape right (the top of the
	phone to the left) the screen's up is the device's x and its right the
	device's -y; turning the phone right turns the view right, tilting its
	top towards the player looks up */
	if (orientation == UIInterfaceOrientationLandscapeLeft)
	{
		yaw = rate.x;
		pitch = -rate.y;
	}
	else
	{
		yaw = -rate.x;
		pitch = rate.y;
	}
	scale = (float)elapsed / LOOK_RADIANS_PER_UNIT * setting_gyro_sensitivity * (setting_gyro_invert ? -1.0f : 1.0f);
	pthread_mutex_lock(&touch_lock);
	gyro_delta[0] += (float)yaw * scale;
	gyro_delta[1] += (float)pitch * scale;
	gyro_ns = now_ns();
	pthread_mutex_unlock(&touch_lock);
}

/* ---------- each display frame: showing, labels, rumble, the gyroscope */

- (void)refresh:(CADisplayLink *)sender
{
	int scene = touch_scene;
	int controller = GCController.controllers.count > 0;
	BOOL show;
	int amplitude;
	int64_t now = now_ns(), rumbled;

	touch_devices = HOST_TOUCH_DEVICE_TOUCHSCREEN | (controller ? HOST_TOUCH_DEVICE_CONTROLLER : 0);
	show = (scene & _touch_scene_known) && !(scene & _touch_scene_menus) && !(scene & _touch_scene_off) &&
		((scene & _touch_scene_on) || !controller);
	if (bindingsSerial != touch_bindings_serial)
		[self relabel];
	[self gyroscope:show && setting_gyro && !panel && !editing];
	if (show == self.hidden)
	{
		self.hidden = !show;
		if (!show)
		{
			/* nothing stays held behind a menu, and the panels close */
			[self closeSettings];
			if (editing)
				[self finishEditing];
			[self releaseAll];
		}
	}
	if (!show || !setting_rumble)
		return;
	pthread_mutex_lock(&touch_lock);
	amplitude = rumble_amplitude;
	rumbled = rumble_ns;
	pthread_mutex_unlock(&touch_lock);
	if (amplitude > 4000 && now - rumbled < TOUCH_STALE_NS && now - hapticNs > 90000000LL)
	{
		hapticNs = now;
		[haptics impactOccurredWithIntensity:fmin(1.0, amplitude / 40000.0)];
	}
}

/* ---------- the settings panel */

- (UIButton *)panelButton:(NSString *)title action:(SEL)action
{
	UIButton *button = [UIButton buttonWithType:UIButtonTypeSystem];

	[button setTitle:title forState:UIControlStateNormal];
	button.titleLabel.font = [UIFont boldSystemFontOfSize:15];
	button.tintColor = UIColor.whiteColor;
	button.backgroundColor = [UIColor colorWithWhite:1.0 alpha:0.15];
	button.layer.cornerRadius = 9;
	button.contentEdgeInsets = UIEdgeInsetsMake(7, 12, 7, 12);
	[button addTarget:self action:action forControlEvents:UIControlEventTouchUpInside];
	return button;
}

- (UIStackView *)row:(NSString *)title with:(UIView *)control
{
	UILabel *label = [[UILabel alloc] init];
	UIStackView *row = [[UIStackView alloc] initWithArrangedSubviews:@[ label, control ]];

	label.text = title;
	label.textColor = UIColor.whiteColor;
	label.font = [UIFont systemFontOfSize:15];
	[label.widthAnchor constraintEqualToConstant:170].active = YES;
	row.axis = UILayoutConstraintAxisHorizontal;
	row.spacing = 10;
	row.alignment = UIStackViewAlignmentCenter;
	return row;
}

- (UIStackView *)slider:(NSString *)title tag:(int)tag value:(float)value from:(float)minimum to:(float)maximum
{
	UISlider *slider = [[UISlider alloc] init];

	slider.minimumValue = minimum;
	slider.maximumValue = maximum;
	slider.value = value;
	slider.tag = tag;
	[slider addTarget:self action:@selector(settingChanged:) forControlEvents:UIControlEventValueChanged];
	return [self row:title with:slider];
}

- (UIStackView *)toggle:(NSString *)title tag:(int)tag value:(BOOL)value
{
	UISwitch *toggle = [[UISwitch alloc] init];
	UIView *spacer = [[UIView alloc] init];
	UIStackView *row;

	toggle.on = value;
	toggle.tag = tag;
	[toggle addTarget:self action:@selector(settingChanged:) forControlEvents:UIControlEventValueChanged];
	row = [self row:title with:toggle];
	[row addArrangedSubview:spacer];
	return row;
}

- (void)openSettings
{
	UIScrollView *scroll;
	UIStackView *stack, *buttons;
	UILabel *title, *hint;
	CGFloat width = fmin(470, self.bounds.size.width - 40), height = fmin(370, self.bounds.size.height - 24);

	if (panel || editing)
		return;
	[self releaseAll];
	panel = [[UIView alloc] initWithFrame:CGRectMake(0, 0, width, height)];
	panel.backgroundColor = [UIColor colorWithWhite:0.07 alpha:0.94];
	panel.layer.cornerRadius = 16;
	panel.center = CGPointMake(CGRectGetMidX(self.bounds), CGRectGetMidY(self.bounds));
	scroll = [[UIScrollView alloc] initWithFrame:panel.bounds];
	scroll.autoresizingMask = UIViewAutoresizingFlexibleWidth | UIViewAutoresizingFlexibleHeight;
	[panel addSubview:scroll];

	title = [[UILabel alloc] init];
	title.text = @"Điều khiển cảm ứng";
	title.textColor = UIColor.whiteColor;
	title.font = [UIFont boldSystemFontOfSize:18];
	hint = [[UILabel alloc] init];
	hint.text = @"Giữ nút Nạp / Dùng để nhặt súng, lên xe, lật xe, bấm công tắc. "
		@"Nút Bắn và Ngắm vừa giữ vừa kéo để xoay tầm nhìn.";
	hint.textColor = [UIColor colorWithWhite:1.0 alpha:0.6];
	hint.font = [UIFont systemFontOfSize:12];
	hint.numberOfLines = 0;
	buttons = [[UIStackView alloc] initWithArrangedSubviews:@[
		[self panelButton:@"Chỉnh bố cục" action:@selector(startEditing)],
		[self panelButton:@"Đóng" action:@selector(closeSettings)],
	]];
	buttons.spacing = 12;
	buttons.distribution = UIStackViewDistributionFillEqually;

	stack = [[UIStackView alloc] initWithArrangedSubviews:@[
		title,
		[self slider:@"Độ nhạy vuốt" tag:_tag_sensitivity value:setting_sensitivity from:0.25f to:4.0f],
		[self toggle:@"Ngắm bằng con quay" tag:_tag_gyro value:setting_gyro],
		[self slider:@"Độ nhạy con quay" tag:_tag_gyro_sensitivity value:setting_gyro_sensitivity from:0.25f to:4.0f],
		[self toggle:@"Đảo chiều con quay" tag:_tag_gyro_invert value:setting_gyro_invert],
		[self slider:@"Cỡ nút" tag:_tag_size value:setting_size from:0.6f to:1.6f],
		[self slider:@"Độ đậm nút" tag:_tag_opacity value:setting_opacity from:0.2f to:1.0f],
		[self toggle:@"Nút bắn bên trái" tag:_tag_left_fire value:![self controlNamed:@"fire_left"].off],
		[self toggle:@"Chạm để ngồi (bật/tắt)" tag:_tag_crouch_toggle value:setting_crouch_toggle],
		[self toggle:@"Rung" tag:_tag_rumble value:setting_rumble],
		hint,
		buttons,
	]];
	stack.axis = UILayoutConstraintAxisVertical;
	stack.spacing = 10;
	stack.translatesAutoresizingMaskIntoConstraints = NO;
	[scroll addSubview:stack];
	[NSLayoutConstraint activateConstraints:@[
		[stack.topAnchor constraintEqualToAnchor:scroll.contentLayoutGuide.topAnchor constant:16],
		[stack.bottomAnchor constraintEqualToAnchor:scroll.contentLayoutGuide.bottomAnchor constant:-16],
		[stack.leadingAnchor constraintEqualToAnchor:scroll.frameLayoutGuide.leadingAnchor constant:20],
		[stack.trailingAnchor constraintEqualToAnchor:scroll.frameLayoutGuide.trailingAnchor constant:-20],
	]];
	[self addSubview:panel];
}

- (void)settingChanged:(UIControl *)sender
{
	float value = [sender isKindOfClass:UISlider.class] ? ((UISlider *)sender).value : 0;
	BOOL on = [sender isKindOfClass:UISwitch.class] ? ((UISwitch *)sender).on : NO;

	switch (sender.tag)
	{
	case _tag_sensitivity: setting_sensitivity = value; break;
	case _tag_gyro_sensitivity: setting_gyro_sensitivity = value; break;
	case _tag_opacity: setting_opacity = value; break;
	case _tag_size: setting_size = value; break;
	case _tag_gyro: setting_gyro = on; break;
	case _tag_gyro_invert: setting_gyro_invert = on; break;
	case _tag_crouch_toggle: setting_crouch_toggle = on; break;
	case _tag_rumble: setting_rumble = on; break;
	case _tag_left_fire: [self controlNamed:@"fire_left"].off = !on; break;
	}
	[self saveSettings];
	[self setNeedsLayout];
}

- (void)closeSettings
{
	if (!panel)
		return;
	[panel removeFromSuperview];
	panel = nil;
	[self saveSettings];
	[self publish];
}

/* ---------- the layout editor */

- (void)startEditing
{
	UILabel *hint = [[UILabel alloc] init];
	UIStackView *bar;

	[self closeSettings];
	[self releaseAll];
	editing = YES;
	selected = nil;
	hint.text = @"Kéo nút để di chuyển, chạm để chọn";
	hint.textColor = UIColor.whiteColor;
	hint.font = [UIFont systemFontOfSize:12];
	bar = [[UIStackView alloc] initWithArrangedSubviews:@[
		hint,
		[self panelButton:@"−" action:@selector(shrinkSelected)],
		[self panelButton:@"+" action:@selector(growSelected)],
		[self panelButton:@"Ẩn/Hiện" action:@selector(toggleSelected)],
		[self panelButton:@"Mặc định" action:@selector(resetLayout)],
		[self panelButton:@"Xong" action:@selector(finishEditing)],
	]];
	bar.spacing = 8;
	bar.alignment = UIStackViewAlignmentCenter;
	toolbar = [[UIView alloc] init];
	toolbar.backgroundColor = [UIColor colorWithWhite:0.07 alpha:0.9];
	toolbar.layer.cornerRadius = 12;
	bar.frame = CGRectMake(10, 6, 0, 0);
	[bar sizeToFit];
	[toolbar addSubview:bar];
	{
		CGSize size = [bar systemLayoutSizeFittingSize:UILayoutFittingCompressedSize];

		bar.frame = CGRectMake(10, 6, size.width, size.height);
		toolbar.bounds = CGRectMake(0, 0, size.width + 20, size.height + 12);
	}
	[self addSubview:toolbar];
	gear.hidden = YES;
	[self setNeedsLayout];
}

- (void)finishEditing
{
	if (!editing)
		return;
	editing = NO;
	selected = nil;
	[toolbar removeFromSuperview];
	toolbar = nil;
	gear.hidden = NO;
	[self saveSettings];
	[self setNeedsLayout];
	[self publish];
}

- (void)resizeSelected:(CGFloat)step
{
	if (!selected)
		return;
	selected.scale = fmax(0.5, fmin(2.0, selected.scale + step));
	[self setNeedsLayout];
}

- (void)shrinkSelected
{
	[self resizeSelected:-0.1];
}

- (void)growSelected
{
	[self resizeSelected:0.1];
}

- (void)toggleSelected
{
	if (!selected || (kinds[selected.kind].flags & _kind_stick))
		return;
	selected.off = !selected.off;
	[self setNeedsLayout];
}

- (void)resetLayout
{
	for (HaloControl *control in controls)
	{
		control.gridX = kinds[control.kind].x;
		control.gridY = kinds[control.kind].y;
		control.scale = 1.0;
		control.off = NO;
	}
	setting_size = 1.0f;
	[self setNeedsLayout];
}

- (void)editBegan:(NSSet<UITouch *> *)touches
{
	UITouch *touch = touches.anyObject;
	CGPoint point = [touch locationInView:self];
	HaloControl *control = [self controlAt:point all:YES];

	selected = control;
	if (control)
		dragOffset = CGPointMake(control.center.x - point.x, control.center.y - point.y);
	[self setNeedsLayout];
}

- (void)editMoved:(NSSet<UITouch *> *)touches
{
	UITouch *touch = touches.anyObject;
	CGPoint point = [touch locationInView:self];
	CGRect area = [self area];
	CGFloat radius, x, y;

	if (!selected || area.size.width <= 0 || area.size.height <= 0)
		return;
	radius = [self radiusOf:selected];
	x = fmax(CGRectGetMinX(area) + radius, fmin(CGRectGetMaxX(area) - radius, point.x + dragOffset.x));
	y = fmax(CGRectGetMinY(area) + radius, fmin(CGRectGetMaxY(area) - radius, point.y + dragOffset.y));
	selected.gridX = (x - area.origin.x) / area.size.width * 960.0;
	selected.gridY = (y - area.origin.y) / area.size.height * 540.0;
	[self setNeedsLayout];
}

- (void)editEnded:(NSSet<UITouch *> *)touches
{
	[self saveSettings];
}

@end

void host_touch_install(void *sdl_window)
{
	UIWindow *window = (__bridge UIWindow *)SDL_GetPointerProperty(SDL_GetWindowProperties((SDL_Window *)sdl_window),
		SDL_PROP_WINDOW_UIKIT_WINDOW_POINTER, NULL);
	UIView *root = window.rootViewController.view ? window.rootViewController.view : window;
	HaloTouchOverlay *overlay;

	if (!root)
	{
		host_logf(HOST_LOG_WARN, "touch: no view for the controls");
		return;
	}
	overlay = [[HaloTouchOverlay alloc] initWithFrame:root.bounds];
	overlay.hidden = YES;
	[root addSubview:overlay];
	host_logf(HOST_LOG_INFO, "touch: controls over %s", NSStringFromClass(root.class).UTF8String);
}
