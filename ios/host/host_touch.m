/*
HOST_TOUCH.M

The on-screen touch controls of the iOS port: a UIKit view over SDL's,
in place of the Android port's TouchControls.java, with the same contract
towards the game (port/linux/src/touch_input.c through
port/android/guest/runtime/guest_host.h's host_touch_*):

- the game reads the overlay's controller (SDL's axes and button bits) and
  its view swipe each frame, and says when the overlay is to show (in a
  game, with no menu or cinematic up, and input.touch_controls not "off";
  with "auto", only while no game controller is connected);
- hidden, the overlay lets every finger through to SDL's view, whose finger
  events become the menus' pointer.

Fingers: one that lands on a button holds it; one in the left 40% of the
screen is a stick that appears under it and follows it while held; any
other one turns the view as it drags.
*/

#import <UIKit/UIKit.h>
#import <GameController/GameController.h>

#include "host.h"

#include <SDL3/SDL.h>
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
static volatile int touch_scene;
static volatile int touch_devices = HOST_TOUCH_DEVICE_TOUCHSCREEN;
static int32_t touch_bindings[TOUCH_BUTTONS];

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

void host_touch_look_read(float *delta)
{
	int64_t now = now_ns();

	pthread_mutex_lock(&touch_lock);
	delta[0] = look_delta[0];
	delta[1] = look_delta[1];
	delta[2] = delta[3] = 0.0f;
	if (now - look_ns > TOUCH_STALE_NS)
		delta[0] = delta[1] = 0.0f;
	look_delta[0] = look_delta[1] = 0.0f;
	pthread_mutex_unlock(&touch_lock);
}

void host_touch_rumble(unsigned int low, unsigned int high)
{
	(void)low;
	(void)high;
}

void host_touch_scene(int scene)
{
	touch_scene = scene;
}

void host_touch_bindings(const int *controls)
{
	pthread_mutex_lock(&touch_lock);
	memcpy(touch_bindings, controls, sizeof(touch_bindings));
	pthread_mutex_unlock(&touch_lock);
}

int host_touch_devices(void)
{
	return touch_devices;
}

/* ---------- the overlay */

@interface HaloTouchButton : UIView
@property (nonatomic) int input;
@end

@implementation HaloTouchButton
@end

@interface HaloTouchOverlay : UIView
@end

@implementation HaloTouchOverlay
{
	NSMutableArray<HaloTouchButton *> *buttons;
	/* each finger's role: a button's input, or -1 the stick, -2 the view */
	NSMapTable<UITouch *, NSNumber *> *roles;
	UITouch *stickTouch;
	CGPoint stickCenter;
	UIView *stickBase, *stickKnob;
	int32_t state[7];
	CADisplayLink *link;
}

static const CGFloat STICK_RADIUS = 60.0;
/* the view's turn for a point of drag */
static const float LOOK_SCALE = 1.0f;

- (HaloTouchButton *)button:(NSString *)label input:(int)input size:(CGFloat)size
{
	HaloTouchButton *button = [[HaloTouchButton alloc] initWithFrame:CGRectMake(0, 0, size, size)];
	UILabel *text = [[UILabel alloc] initWithFrame:button.bounds];

	button.input = input;
	button.backgroundColor = [UIColor colorWithWhite:1.0 alpha:0.18];
	button.layer.cornerRadius = size / 2;
	button.layer.borderWidth = 1.5;
	button.layer.borderColor = [UIColor colorWithWhite:1.0 alpha:0.45].CGColor;
	button.userInteractionEnabled = NO;
	text.text = label;
	text.textAlignment = NSTextAlignmentCenter;
	text.textColor = [UIColor colorWithWhite:1.0 alpha:0.8];
	text.font = [UIFont boldSystemFontOfSize:size * 0.32];
	text.autoresizingMask = UIViewAutoresizingFlexibleWidth | UIViewAutoresizingFlexibleHeight;
	[button addSubview:text];
	[self addSubview:button];
	[buttons addObject:button];
	return button;
}

- (instancetype)initWithFrame:(CGRect)frame
{
	self = [super initWithFrame:frame];
	if (!self)
		return nil;
	self.multipleTouchEnabled = YES;
	self.backgroundColor = UIColor.clearColor;
	self.autoresizingMask = UIViewAutoresizingFlexibleWidth | UIViewAutoresizingFlexibleHeight;
	buttons = [NSMutableArray array];
	roles = [NSMapTable strongToStrongObjectsMapTable];

	[self button:@"A" input:_input_a size:64];
	[self button:@"B" input:_input_b size:56];
	[self button:@"X" input:_input_x size:56];
	[self button:@"Y" input:_input_y size:56];
	[self button:@"RT" input:_input_right_trigger size:76];
	[self button:@"LT" input:_input_left_trigger size:56];
	[self button:@"RB" input:_input_right_shoulder size:48];
	[self button:@"LB" input:_input_left_shoulder size:48];
	[self button:@"R3" input:_input_right_stick size:44];
	[self button:@"L3" input:_input_left_stick size:44];
	[self button:@"☰" input:_input_start size:40];
	[self button:@"◫" input:_input_back size:40];

	stickBase = [[UIView alloc] initWithFrame:CGRectMake(0, 0, STICK_RADIUS * 2, STICK_RADIUS * 2)];
	stickBase.layer.cornerRadius = STICK_RADIUS;
	stickBase.layer.borderWidth = 2;
	stickBase.layer.borderColor = [UIColor colorWithWhite:1.0 alpha:0.35].CGColor;
	stickBase.backgroundColor = [UIColor colorWithWhite:1.0 alpha:0.08];
	stickBase.userInteractionEnabled = NO;
	stickBase.hidden = YES;
	stickKnob = [[UIView alloc] initWithFrame:CGRectMake(0, 0, 50, 50)];
	stickKnob.layer.cornerRadius = 25;
	stickKnob.backgroundColor = [UIColor colorWithWhite:1.0 alpha:0.35];
	stickKnob.userInteractionEnabled = NO;
	stickKnob.hidden = YES;
	[self addSubview:stickBase];
	[self addSubview:stickKnob];

	link = [CADisplayLink displayLinkWithTarget:self selector:@selector(refresh:)];
	link.preferredFramesPerSecond = 15;
	[link addToRunLoop:NSRunLoop.mainRunLoop forMode:NSRunLoopCommonModes];
	return self;
}

- (void)layoutSubviews
{
	CGRect bounds = self.bounds;
	UIEdgeInsets safe = self.safeAreaInsets;
	CGFloat right = bounds.size.width - safe.right - 16;
	CGFloat bottom = bounds.size.height - safe.bottom - 16;
	CGFloat left = safe.left + 16;
	CGFloat top = safe.top + 12;

	[super layoutSubviews];
	for (HaloTouchButton *button in buttons)
	{
		CGFloat size = button.bounds.size.width;
		CGPoint center = CGPointZero;

		switch (button.input)
		{
		case _input_a: center = CGPointMake(right - 40, bottom - 40); break;
		case _input_b: center = CGPointMake(right - 40 - 0, bottom - 40 - 78); break;
		case _input_x: center = CGPointMake(right - 40 - 78, bottom - 40); break;
		case _input_y: center = CGPointMake(right - 40 - 78, bottom - 40 - 78); break;
		case _input_right_trigger: center = CGPointMake(right - 50, top + 96); break;
		case _input_right_shoulder: center = CGPointMake(right - 50 - 82, top + 60); break;
		case _input_left_trigger: center = CGPointMake(left + 40, top + 90); break;
		case _input_left_shoulder: center = CGPointMake(left + 40 + 70, top + 60); break;
		case _input_right_stick: center = CGPointMake(right - 40 - 160, bottom - 30); break;
		case _input_left_stick: center = CGPointMake(left + 30, bottom - 150); break;
		case _input_start: center = CGPointMake(bounds.size.width / 2 + 34, top + 22); break;
		case _input_back: center = CGPointMake(bounds.size.width / 2 - 34, top + 22); break;
		}
		button.center = center;
		(void)size;
	}
}

- (void)publish
{
	state_set(state);
}

- (void)setInput:(int)input down:(BOOL)down
{
	if (input == _input_left_trigger || input == _input_right_trigger)
		state[4 + input - _input_left_trigger] = down ? 32767 : 0;
	else if (down)
		state[6] |= 1 << input;
	else
		state[6] &= ~(1 << input);
	for (HaloTouchButton *button in buttons)
	{
		if (button.input == input)
			button.backgroundColor = [UIColor colorWithWhite:1.0 alpha:down ? 0.45 : 0.18];
	}
}

- (HaloTouchButton *)buttonAt:(CGPoint)point
{
	for (HaloTouchButton *button in buttons)
	{
		CGFloat radius = button.bounds.size.width / 2 + 8;
		CGFloat dx = point.x - button.center.x, dy = point.y - button.center.y;

		if (dx * dx + dy * dy <= radius * radius)
			return button;
	}
	return nil;
}

- (void)moveStick:(CGPoint)point
{
	CGFloat dx = point.x - stickCenter.x, dy = point.y - stickCenter.y;
	CGFloat length = sqrt(dx * dx + dy * dy);

	if (length > STICK_RADIUS)
	{
		dx *= STICK_RADIUS / length;
		dy *= STICK_RADIUS / length;
	}
	stickKnob.center = CGPointMake(stickCenter.x + dx, stickCenter.y + dy);
	state[0] = (int32_t)(dx / STICK_RADIUS * 32767);
	state[1] = (int32_t)(dy / STICK_RADIUS * 32767);
}

- (void)touchesBegan:(NSSet<UITouch *> *)touches withEvent:(UIEvent *)event
{
	for (UITouch *touch in touches)
	{
		CGPoint point = [touch locationInView:self];
		HaloTouchButton *button = [self buttonAt:point];

		if (button)
		{
			[roles setObject:@(button.input) forKey:touch];
			[self setInput:button.input down:YES];
		}
		else if (!stickTouch && point.x < self.bounds.size.width * 0.4)
		{
			stickTouch = touch;
			stickCenter = point;
			stickBase.center = point;
			stickKnob.center = point;
			stickBase.hidden = stickKnob.hidden = NO;
			[roles setObject:@(-1) forKey:touch];
		}
		else
		{
			[roles setObject:@(-2) forKey:touch];
		}
	}
	[self publish];
}

- (void)touchesMoved:(NSSet<UITouch *> *)touches withEvent:(UIEvent *)event
{
	for (UITouch *touch in touches)
	{
		NSNumber *role = [roles objectForKey:touch];
		CGPoint point = [touch locationInView:self];
		CGPoint previous = [touch previousLocationInView:self];

		if (!role)
			continue;
		if (role.intValue == -1)
		{
			[self moveStick:point];
		}
		else if (role.intValue == -2)
		{
			pthread_mutex_lock(&touch_lock);
			look_delta[0] += (float)(point.x - previous.x) * LOOK_SCALE;
			look_delta[1] += (float)(point.y - previous.y) * LOOK_SCALE;
			look_ns = now_ns();
			pthread_mutex_unlock(&touch_lock);
		}
	}
	[self publish];
}

- (void)touchesEnded:(NSSet<UITouch *> *)touches withEvent:(UIEvent *)event
{
	for (UITouch *touch in touches)
	{
		NSNumber *role = [roles objectForKey:touch];

		if (!role)
			continue;
		if (role.intValue == -1)
		{
			stickTouch = nil;
			state[0] = state[1] = 0;
			stickBase.hidden = stickKnob.hidden = YES;
		}
		else if (role.intValue >= 0)
		{
			[self setInput:role.intValue down:NO];
		}
		[roles removeObjectForKey:touch];
	}
	[self publish];
}

- (void)touchesCancelled:(NSSet<UITouch *> *)touches withEvent:(UIEvent *)event
{
	[self touchesEnded:touches withEvent:event];
}

/* whether the overlay shows (touch_input.c's touch_input_screen_buttons
with the scene the game last sent) */
- (void)refresh:(CADisplayLink *)sender
{
	int scene = touch_scene;
	int controller = GCController.controllers.count > 0;
	BOOL show;

	touch_devices = HOST_TOUCH_DEVICE_TOUCHSCREEN | (controller ? HOST_TOUCH_DEVICE_CONTROLLER : 0);
	show = (scene & _touch_scene_known) && !(scene & _touch_scene_menus) && !(scene & _touch_scene_off) &&
		((scene & _touch_scene_on) || !controller);
	if (show == !self.hidden)
		return;
	self.hidden = !show;
	if (!show)
	{
		/* nothing stays held behind a menu */
		memset(state, 0, sizeof(state));
		[roles removeAllObjects];
		stickTouch = nil;
		stickBase.hidden = stickKnob.hidden = YES;
		for (HaloTouchButton *button in buttons)
			button.backgroundColor = [UIColor colorWithWhite:1.0 alpha:0.18];
		[self publish];
	}
}

@end

void host_touch_install(void *sdl_window)
{
	UIWindow *window = (__bridge UIWindow *)SDL_GetPointerProperty(SDL_GetWindowProperties((SDL_Window *)sdl_window),
		SDL_PROP_WINDOW_UIKIT_WINDOW_POINTER, NULL);
	UIView *root = window.rootViewController.view ? window.rootViewController.view : window;
	HaloTouchOverlay *overlay;

	if (!root)
		return;
	overlay = [[HaloTouchOverlay alloc] initWithFrame:root.bounds];
	overlay.hidden = YES;
	[root addSubview:overlay];
}
