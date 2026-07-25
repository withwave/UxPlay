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

static NSString *track_text = nil;
static void (*volume_handler)(double) = NULL;
static void (*disconnect_handler)(void) = NULL;

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

@interface UxPlayStatusTarget : NSObject
- (void) quit: (id) sender;
- (void) disconnect: (id) sender;
- (void) volumeChanged: (id) sender;
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
@end

static UxPlayStatusTarget *target = nil;

/* Must run on the main thread. */
static void
refresh (void)
{
    NSString *state_text;
    NSString *symbol;
    BOOL connected = (current_state != STATUSBAR_IDLE);

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
               and sits high. */
            CGFloat points = [[NSStatusBar systemStatusBar] thickness] * 0.62;
            NSImage *sized = [image imageWithSymbolConfiguration:
                [NSImageSymbolConfiguration configurationWithPointSize: points
                                                                weight: NSFontWeightRegular]];
            if (sized != nil) {
                image = sized;
            }
        }
    }

    NSStatusBarButton *button = [status_item button];

    if (image != nil) {
        if (connected) {
            /* A template image is drawn in the menu bar's own colour whatever
               the button's tint says, so the colour has to be burnt into a
               non-template copy instead. */
            NSImage *tinted = [[image copy] autorelease];

            [tinted setTemplate: NO];
            [tinted lockFocus];
            [[NSColor systemBlueColor] set];
            NSRectFillUsingOperation (NSMakeRect (0, 0, [tinted size].width,
                    [tinted size].height), NSCompositingOperationSourceAtop);
            [tinted unlockFocus];
            image = tinted;
        } else {
            [image setTemplate: YES];
        }
        [button setImage: image];
        [button setTitle: @""];
        [button setImagePosition: NSImageOnly];
        [button setImageScaling: NSImageScaleProportionallyDown];
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

        /* A slider needs a view of its own; a plain menu item cannot hold one. */
        {
            NSView *row = [[[NSView alloc] initWithFrame:
                NSMakeRect (0, 0, 220, 32)] autorelease];
            NSTextField *label = [NSTextField labelWithString: @"Volume"];

            [label setFrame: NSMakeRect (14, 7, 56, 18)];
            [label setFont: [NSFont menuFontOfSize: 0]];
            [row addSubview: label];

            volume_slider = [[NSSlider alloc] initWithFrame:
                NSMakeRect (74, 6, 132, 20)];
            [volume_slider setMinValue: 0.0];
            [volume_slider setMaxValue: 1.0];
            [volume_slider setDoubleValue: 1.0];
            [volume_slider setTarget: target];
            [volume_slider setAction: @selector(volumeChanged:)];
            [volume_slider setContinuous: YES];
            [row addSubview: volume_slider];

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

        status_item = [[[NSStatusBar systemStatusBar]
            statusItemWithLength: NSSquareStatusItemLength] retain];
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
