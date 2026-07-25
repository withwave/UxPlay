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
@end

@implementation UxPlayStatusTarget
- (void) quit: (id) sender
{
    /* Same path as Ctrl-C, so the server is torn down cleanly. */
    kill (getpid (), SIGINT);
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

        [menu addItem: [NSMenuItem separatorItem]];
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
        [target release];
        target = nil;
        [client_name release];
        client_name = nil;
        [client_model release];
        client_model = nil;
    });
}
