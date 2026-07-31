/* GStreamer
 * Copyright (C) 2004 Zaheer Abbas Merali <zaheerabbas at merali dot org>
 * Copyright (C) 2007 Pioneers of the Inevitable <songbird@songbirdnest.com>
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Library General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Library General Public License for more details.
 *
 * You should have received a copy of the GNU Library General Public
 * License along with this library; if not, write to the
 * Free Software Foundation, Inc., 51 Franklin St, Fifth Floor,
 * Boston, MA 02110-1301, USA.
 *
 * The development of this code was made possible due to the involvement of Pioneers 
 * of the Inevitable, the creators of the Songbird Music player
 * 
 */

/* inspiration gained from looking at source of osx video out of xine and vlc 
 * and is reflected in the code
 */

#import <Cocoa/Cocoa.h>
#import <glib.h>
#import <gst/video/navigation.h>

struct _GstOSXImage;

/* How the frame is mapped onto the view. FIT keeps the whole frame visible and
   leaves bars on one axis; the FILL modes pin one axis to the view and let the
   other overflow, cropping it. All three preserve the aspect ratio. */
#define GST_OSX_FILL_FIT           0
#define GST_OSX_FILL_HEIGHT        1
#define GST_OSX_FILL_WIDTH         2

@interface GstGLView : NSOpenGLView
{
    int i_effect;
    unsigned int pi_texture;
    float f_x;
    float f_y;
    int initDone;
    char* data;
    int width, height;
    int allocWidth, allocHeight;  /* what data was last sized for */
    unsigned int prevTexture;     /* kept on screen until the new one fills */
    int prevWidth, prevHeight;
    BOOL awaitingFirstFrame;
    BOOL fullscreen;
    BOOL keepAspectRatio;
    int fillMode;
    float osdVolume;            /* 0.0 - 1.0, level shown by the OSD */
    unsigned int osdIconTexture;
    int osdIconWidth, osdIconHeight;
    NSTimer *osdTimer;
    double controlsExpiry;      /* reference time the close button fades out at */
    BOOL pseudoFullScreen;      /* filling the screen without a fullscreen space */
    NSRect savedWindowFrame;
    NSUInteger savedStyleMask;
    NSOpenGLContext* fullScreenContext; 
    NSOpenGLContext* actualContext;
    NSTrackingArea *trackingArea;
    GstNavigation *navigation;
    NSRect drawingBounds;
    NSThread *mainThread;
    NSUInteger savedModifierFlags;
    double osdPosition;         /* seconds into the stream */
    double osdDuration;         /* 0 when the stream has no timeline */
    unsigned int timeTexture;   /* the two clock readings, baked as one strip */
    int timeTexWidth, timeTexHeight;
    NSString *timeTexString;    /* what that strip currently says */
    BOOL scrubbing;
    BOOL volumeDragging;
    BOOL panelDragging;
    NSPoint panelOffset;        /* where the user has moved the panel to */
    NSPoint panelDragAnchor;
    NSPoint panelOffsetAtAnchor;
    double osdRate;             /* 0 while paused */
    NSInteger selectedDisplayIndex;  /* -1: wherever the window already is */
}
- (void) drawQuad;
- (void) drawQuadWidth: (int) w height: (int) h;
- (void) drawRect: (NSRect) rect;
- (id) initWithFrame: (NSRect) frame;
- (void) initTextures;
- (void) reloadTexture;
- (void) cleanUp;
- (void) displayTexture;
- (char*) getTextureBuffer;
- (void) getTextureSize: (int *) w : (int *) h;
- (void) setFullScreen: (BOOL) flag;
- (void) setKeepAspectRatio: (BOOL) flag;
- (void) showVolumeOSD: (float) level;
- (void) showVolumeOSDNumber: (NSNumber *) level;
- (void) setPlaybackPosition: (NSNumber *) seconds;
- (void) setPlaybackDuration: (NSNumber *) seconds;
- (void) setPlaybackRate: (NSNumber *) rate;
- (void) setDisplayIndex: (NSNumber *) index;
- (void) sendControlKey: (const char *) name;
- (void) setFillMode: (int) mode;
- (int) fillMode;
- (BOOL) isInNativeFullScreen;
- (void) toggleFillScreen;
- (void) enterFillScreen;
- (void) leaveFillScreen;
- (void) reshape;
- (void) setVideoSize:(int)w : (int)h;
- (NSRect) getDrawingBounds;
- (BOOL) haveSuperview;
- (void) haveSuperviewReal: (NSMutableArray *)closure;
- (void) addToSuperview: (NSView *)superview;
- (void) removeFromSuperview: (id)unused;
- (void) setNavigation: (GstNavigation *) nav;

@end

@interface GstOSXVideoSinkWindow: NSWindow {
   int width, height;
   GstGLView *gstview;
}

- (void) setContentSize: (NSSize) size;
- (GstGLView *) gstView;
- (id)initWithContentNSRect:(NSRect)contentRect styleMask:(unsigned int)styleMask backing:(NSBackingStoreType)bufferingType defer:(BOOL)flag screen:(NSScreen *)aScreen;
@end
