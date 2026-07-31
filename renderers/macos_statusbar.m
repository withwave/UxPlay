/*
 * macOS menu bar status item for UxPlay. See macos_statusbar.h.
 */

#import <Cocoa/Cocoa.h>
#include <signal.h>
#include <unistd.h>

#include "macos_statusbar.h"

static NSStatusItem *status_item = nil;
static NSMenuItem *client_item = nil;
static NSMenuItem *state_item = nil;
static NSMenuItem *track_item = nil;
static NSMenuItem *disconnect_item = nil;
static NSSlider *volume_slider = nil;
static NSMenuItem *progress_item = nil;
static NSSlider *progress_slider = nil;
static NSTextField *progress_label = nil;
static double progress_duration = 0.0;
static BOOL progress_dragging = NO;

static NSString *track_text = nil;
static void (*volume_handler)(double) = NULL;
static void (*disconnect_handler)(void) = NULL;
static void (*seek_handler)(double) = NULL;

static statusbar_state_t current_state = STATUSBAR_IDLE;
static NSString *client_name = nil;
static NSString *client_model = nil;

/* UxPlay's main() runs on a worker thread under the gst_macos_main wrapper, so
   nothing here may touch AppKit directly. */
static void
run_on_main (dispatch_block_t block)
{
    if ([NSThread isMainThread]) {
        block ();
    } else {
        dispatch_async (dispatch_get_main_queue (), block);
    }
}

/* A plain NSView handed to a menu item gets a slice of the menu's backing
   store that nothing initialises and no menu material composited underneath,
   so whatever happened to be there shows through -- a black wedge across the
   menu's rounded background, with only the controls that draw themselves
   (the sliders) coming out right. Vibrancy is what makes AppKit put the menu's
   own material behind the row. */
@interface UxPlayMenuRow : NSView
@end

@implementation UxPlayMenuRow
- (BOOL) allowsVibrancy
{
    return YES;
}
@end

@interface UxPlayStatusTarget : NSObject
- (void) quit: (id) sender;
- (void) disconnect: (id) sender;
- (void) volumeChanged: (id) sender;
- (void) progressChanged: (id) sender;
@end

@implementation UxPlayStatusTarget
- (void) quit: (id) sender
{
    /* Same path as Ctrl-C, so the server is torn down cleanly. */
    kill (getpid (), SIGINT);
}

- (void) disconnect: (id) sender
{
    if (disconnect_handler != NULL) {
        disconnect_handler ();
    }
}

- (void) volumeChanged: (id) sender
{
    if (volume_handler != NULL) {
        volume_handler ([(NSSlider *) sender doubleValue]);
    }
}

- (void) progressChanged: (id) sender
{
    NSSlider *slider = (NSSlider *) sender;

    /* While the knob is held, keep the position updates that arrive every
       second from yanking it back out from under the pointer. */
    progress_dragging = ([[NSApp currentEvent] type] == NSEventTypeLeftMouseDown ||
                         [[NSApp currentEvent] type] == NSEventTypeLeftMouseDragged);
    if (!progress_dragging && seek_handler != NULL) {
        seek_handler ([slider doubleValue]);
    }
}
@end

static UxPlayStatusTarget *target = nil;
static dispatch_source_t open_menu_signal = nil;

/* A slider squeezed into a frame shorter than the control's own metrics has
   its rounded track and knob clipped, which is what left the gauge looking
   broken. Let the control size itself vertically, keep the width we want, and
   centre it in the row. */
static void
place_slider (NSSlider *slider, NSView *row, CGFloat x, CGFloat width,
    CGFloat centre_y)
{
    NSRect f;

    [slider sizeToFit];
    f = [slider frame];
    f.origin.x = x;
    f.size.width = width;
    f.origin.y = round (centre_y - f.size.height / 2.0);
    [slider setFrame: f];
    [slider setAutoresizingMask: NSViewWidthSizable];
    [row addSubview: slider];
}

/* Must run on the main thread. */
static void
refresh (void)
{
    NSString *state_text;
    NSString *symbol;
    BOOL connected = (current_state != STATUSBAR_IDLE);
    BOOL coloured = NO;

    if (status_item == nil) {
        return;
    }

    switch (current_state) {
    case STATUSBAR_MIRROR:
        state_text = @"Mirroring";
        symbol = @"airplayvideo";
        break;
    case STATUSBAR_AUDIO:
        state_text = @"Streaming audio";
        symbol = @"airplayaudio";
        break;
    case STATUSBAR_VIDEO:
        state_text = @"Playing video";
        symbol = @"airplayvideo";
        break;
    case STATUSBAR_IDLE:
    default:
        state_text = @"Waiting for a client";
        symbol = @"airplayvideo";
        break;
    }

    NSImage *image = nil;
    if ([NSImage respondsToSelector:
            @selector(imageWithSystemSymbolName:accessibilityDescription:)]) {
        image = [NSImage imageWithSystemSymbolName: symbol
                          accessibilityDescription: @"UxPlay"];
        if (image != nil) {
            /* Sizing the symbol from the bar's thickness is what keeps it
               vertically centred; left unconfigured it uses its own metrics
               and sits high. Round it: this menu bar draws one pixel per
               point, so a fractional size puts the symbol's one-pixel strokes
               on half pixels and the rounded rectangle comes back with its
               corners chewed off and gaps along the edges. */
            CGFloat points = floor ([[NSStatusBar systemStatusBar] thickness] * 0.62);
            NSImageSymbolConfiguration *config = [NSImageSymbolConfiguration
                configurationWithPointSize: points weight: NSFontWeightRegular];

            /* Colouring the symbol through its own configuration leaves the
               vector artwork intact. Drawing it into a bitmap to tint it
               rasterised it once at 1x, so on a Retina menu bar the rounded
               rectangle came out ragged. */
            if (connected && [NSImageSymbolConfiguration respondsToSelector:
                    @selector(configurationWithHierarchicalColor:)]) {
                NSImageSymbolConfiguration *colour = [NSImageSymbolConfiguration
                    configurationWithHierarchicalColor: [NSColor systemBlueColor]];
                NSImageSymbolConfiguration *both =
                    [config configurationByApplyingConfiguration: colour];

                if (both != nil) {
                    config = both;
                    coloured = YES;
                }
            }

            NSImage *sized = [image imageWithSymbolConfiguration: config];
            if (sized != nil) {
                NSSize s = [sized size];

                /* Same reason: the button centres the image in its bounds, so
                   a fractional size lands the artwork on a half pixel. */
                [sized setSize: NSMakeSize (round (s.width), round (s.height))];
                image = sized;
            } else {
                coloured = NO;
            }
        }
    }

    NSStatusBarButton *button = [status_item button];

    if (image != nil) {
        if (connected && !coloured) {
            /* Older systems have no colour configuration to apply. A template
               image is drawn in the menu bar's own colour whatever the
               button's tint says, so tint it by hand -- but through a drawing
               handler, which redraws at whatever scale the screen asks for
               instead of freezing the symbol into a 1x bitmap. */
            NSImage *plain = image;
            NSSize size = [plain size];

            image = [NSImage imageWithSize: size flipped: NO
                             drawingHandler: ^BOOL (NSRect rect) {
                [plain drawInRect: rect];
                [[NSColor systemBlueColor] set];
                NSRectFillUsingOperation (rect, NSCompositingOperationSourceAtop);
                return YES;
            }];
        }
        [image setTemplate: !connected];
        [button setImage: image];
        [button setTitle: @""];
        [button setImagePosition: NSImageOnly];
        /* Scaling here would resample what the symbol configuration already
           sized correctly, which is what broke the outline. */
        [button setImageScaling: NSImageScaleNone];
    } else {
        [button setTitle: connected ? @"\U0001F535" : @"\U000026AA"];
    }

    NSString *who = client_name.length ? client_name : @"UxPlay";
    [button setToolTip: [NSString stringWithFormat: @"%@ — %@", who, state_text]];

    [client_item setTitle: client_name.length ? client_name : @"No client"];
    [client_item setHidden: !connected];
    [state_item setTitle: state_text];

    [track_item setHidden: !(connected && track_text.length)];
    if (connected && track_text.length) {
        [track_item setTitle: track_text];
    }
    [disconnect_item setEnabled: connected];
}

void
statusbar_init (void)
{
    run_on_main (^{
        if (status_item != nil) {
            return;
        }

        target = [[UxPlayStatusTarget alloc] init];

        NSMenu *menu = [[[NSMenu alloc] initWithTitle: @"UxPlay"] autorelease];

        client_item = [menu addItemWithTitle: @"No client"
                                      action: nil keyEquivalent: @""];
        [client_item setEnabled: NO];
        [client_item setHidden: YES];

        state_item = [menu addItemWithTitle: @"Waiting for a client"
                                     action: nil keyEquivalent: @""];
        [state_item setEnabled: NO];

        track_item = [menu addItemWithTitle: @"" action: nil keyEquivalent: @""];
        [track_item setEnabled: NO];
        [track_item setHidden: YES];

        [menu addItem: [NSMenuItem separatorItem]];

        {
            NSView *row = [[[UxPlayMenuRow alloc] initWithFrame:
                NSMakeRect (0, 0, 220, 46)] autorelease];

            [row setAutoresizesSubviews: YES];
            [row setWantsLayer: YES];

            progress_label = [NSTextField labelWithString: @"0:00 / 0:00"];
            [progress_label setFrame: NSMakeRect (14, 26, 192, 16)];
            [progress_label setFont: [NSFont menuFontOfSize: 0]];
            [progress_label setAlignment: NSTextAlignmentCenter];
            [row addSubview: progress_label];

            progress_slider = [[NSSlider alloc] initWithFrame:
                NSMakeRect (14, 4, 192, 21)];
            [progress_slider setMinValue: 0.0];
            [progress_slider setMaxValue: 1.0];
            [progress_slider setDoubleValue: 0.0];
            [progress_slider setTarget: target];
            [progress_slider setAction: @selector(progressChanged:)];
            [progress_slider setContinuous: YES];
            place_slider (progress_slider, row, 14, 192, 14);

            progress_item = [menu addItemWithTitle: @"" action: nil keyEquivalent: @""];
            [progress_item setView: row];
            [progress_item setHidden: YES];
        }

        [menu addItem: [NSMenuItem separatorItem]];

        /* A slider needs a view of its own; a plain menu item cannot hold one. */
        {
            NSView *row = [[[UxPlayMenuRow alloc] initWithFrame:
                NSMakeRect (0, 0, 220, 32)] autorelease];

            [row setAutoresizesSubviews: YES];
            [row setWantsLayer: YES];
            NSTextField *label = [NSTextField labelWithString: @"Volume"];

            [label setFrame: NSMakeRect (14, 7, 56, 18)];
            [label setFont: [NSFont menuFontOfSize: 0]];
            [row addSubview: label];

            volume_slider = [[NSSlider alloc] initWithFrame:
                NSMakeRect (74, 6, 132, 21)];
            [volume_slider setMinValue: 0.0];
            [volume_slider setMaxValue: 1.0];
            [volume_slider setDoubleValue: 1.0];
            [volume_slider setTarget: target];
            [volume_slider setAction: @selector(volumeChanged:)];
            [volume_slider setContinuous: YES];
            place_slider (volume_slider, row, 74, 132, 16);

            NSMenuItem *volume_item = [menu addItemWithTitle: @""
                                                      action: nil
                                               keyEquivalent: @""];
            [volume_item setView: row];
        }

        [menu addItem: [NSMenuItem separatorItem]];

        disconnect_item = [menu addItemWithTitle: @"Disconnect"
                                          action: @selector(disconnect:)
                                   keyEquivalent: @""];
        [disconnect_item setTarget: target];
        [disconnect_item setEnabled: NO];
        [[menu addItemWithTitle: @"Quit UxPlay"
                         action: @selector(quit:)
                  keyEquivalent: @"q"] setTarget: target];

        /* The AirPlay symbols are wider than they are tall, so a square item
           would squeeze them -- and squeezing means resampling. */
        /* SIGUSR1 opens the menu, so the UI can be inspected without a
           hand on the mouse. */
        signal (SIGUSR1, SIG_IGN);
        open_menu_signal = dispatch_source_create (DISPATCH_SOURCE_TYPE_SIGNAL,
            SIGUSR1, 0, dispatch_get_main_queue ());
        dispatch_source_set_event_handler (open_menu_signal, ^{
            [[status_item button] performClick: nil];
        });
        dispatch_resume (open_menu_signal);

        status_item = [[[NSStatusBar systemStatusBar]
            statusItemWithLength: NSVariableStatusItemLength] retain];
        [status_item setMenu: menu];

        refresh ();
    });
}

void
statusbar_set_client (const char *name, const char *model)
{
    NSString *n = name ? [NSString stringWithUTF8String: name] : nil;
    NSString *m = model ? [NSString stringWithUTF8String: model] : nil;

    run_on_main (^{
        [client_name release];
        [client_model release];
        client_name = [n retain];
        client_model = [m retain];
        refresh ();
    });
}

void
statusbar_set_state (statusbar_state_t state)
{
    run_on_main (^{
        current_state = state;
        refresh ();
    });
}

/* Menu items get unreadable long before they get wide enough to hold a full
   track name, so clip with an ellipsis. */
static NSString *
elide (NSString *text, NSUInteger limit)
{
    if ([text length] <= limit) {
        return text;
    }
    return [[text substringToIndex: limit] stringByAppendingString: @"\U00002026"];
}

void
statusbar_set_metadata (const char *title, const char *artist)
{
    NSString *t = title ? [NSString stringWithUTF8String: title] : nil;
    NSString *a = artist ? [NSString stringWithUTF8String: artist] : nil;

    run_on_main (^{
        NSString *combined = nil;

        if (t.length && a.length) {
            combined = [NSString stringWithFormat: @"%@ \U00002014 %@", a, t];
        } else if (t.length) {
            combined = t;
        } else if (a.length) {
            combined = a;
        }

        [track_text release];
        track_text = combined.length ?
            [[NSString stringWithFormat: @"\U0000266A %@",
                elide (combined, 44)] retain] : nil;
        refresh ();
    });
}

/* m:ss, or h:mm:ss once past an hour. */
static NSString *
clock_string (double seconds)
{
    long total = (long) (seconds < 0.0 ? 0.0 : seconds);

    if (total >= 3600) {
        return [NSString stringWithFormat: @"%ld:%02ld:%02ld",
            total / 3600, (total % 3600) / 60, total % 60];
    }
    return [NSString stringWithFormat: @"%ld:%02ld", total / 60, total % 60];
}

void
statusbar_set_progress (double position, double duration)
{
    run_on_main (^{
        progress_duration = duration;
        if (duration <= 0.0) {
            [progress_item setHidden: YES];
            return;
        }
        [progress_item setHidden: NO];
        [progress_slider setMaxValue: duration];
        if (!progress_dragging) {
            [progress_slider setDoubleValue: position];
        }
        [progress_label setStringValue:
            [NSString stringWithFormat: @"%@ / %@",
                clock_string (progress_dragging ?
                    [progress_slider doubleValue] : position),
                clock_string (duration)]];
    });
}

void
statusbar_set_seek_handler (void (*handler)(double position))
{
    seek_handler = handler;
}

void
statusbar_set_volume (double fraction)
{
    run_on_main (^{
        if (volume_slider != nil) {
            [volume_slider setDoubleValue: fraction];
        }
    });
}

void
statusbar_set_volume_handler (void (*handler)(double fraction))
{
    volume_handler = handler;
}

void
statusbar_set_disconnect_handler (void (*handler)(void))
{
    disconnect_handler = handler;
}

void
statusbar_destroy (void)
{
    run_on_main (^{
        if (status_item != nil) {
            [[NSStatusBar systemStatusBar] removeStatusItem: status_item];
            [status_item release];
            status_item = nil;
        }
        client_item = nil;
        state_item = nil;
        track_item = nil;
        disconnect_item = nil;
        [volume_slider release];
        volume_slider = nil;
        [progress_slider release];
        progress_slider = nil;
        progress_item = nil;
        progress_label = nil;
        [track_text release];
        track_text = nil;
        [target release];
        target = nil;
        [client_name release];
        client_name = nil;
        [client_model release];
        client_model = nil;
    });
}
