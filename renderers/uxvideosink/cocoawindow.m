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


#include <Cocoa/Cocoa.h>
#include <gst/gst.h>
#import "cocoawindow.h"
#import "osxvideosink.h"

#include <OpenGL/OpenGL.h>
#include <OpenGL/gl.h>
#include <OpenGL/glext.h>

#include <Carbon/Carbon.h>

/* Debugging category */
#include <gst/gstinfo.h>

#if MAC_OS_X_VERSION_MAX_ALLOWED < 101200
#define NSEventTypeMouseMoved                NSMouseMoved
#define NSEventTypeLeftMouseDown             NSLeftMouseDown
#define NSEventTypeLeftMouseUp               NSLeftMouseUp
#define NSEventTypeRightMouseDown            NSRightMouseDown
#define NSEventTypeRightMouseUp              NSRightMouseUp
#endif

static
const gchar* gst_keycode_to_keyname(gint16 keycode)
{
    switch (keycode)
    {
      case kVK_ANSI_A:
        return "a";
      case kVK_ANSI_S:
        return "s";
      case kVK_ANSI_D:
        return "d";
      case kVK_ANSI_F:
        return "f";
      case kVK_ANSI_H:
        return "h";
      case kVK_ANSI_G:
        return "g";
      case kVK_ANSI_Z:
        return "z";
      case kVK_ANSI_X:
        return "x";
      case kVK_ANSI_C:
        return "c";
      case kVK_ANSI_V:
        return "v";
      case kVK_ANSI_B:
        return "b";
      case kVK_ANSI_Q:
        return "q";
      case kVK_ANSI_W:
        return "w";
      case kVK_ANSI_E:
        return "e";
      case kVK_ANSI_R:
        return "r";
      case kVK_ANSI_Y:
        return "y";
      case kVK_ANSI_T:
        return "t";
      case kVK_ANSI_1:
        return "1";
      case kVK_ANSI_2:
        return "2";
      case kVK_ANSI_3:
        return "3";
      case kVK_ANSI_4:
        return "4";
      case kVK_ANSI_6:
        return "6";
      case kVK_ANSI_5:
        return "5";
      case kVK_ANSI_Equal:
        return "equal";
      case kVK_ANSI_9:
        return "9";
      case kVK_ANSI_7:
        return "7";
      case kVK_ANSI_Minus:
        return "minus";
      case kVK_ANSI_8:
        return "8";
      case kVK_ANSI_0:
        return "0";
      case kVK_ANSI_RightBracket:
        return "bracketright";
      case kVK_ANSI_O:
        return "0";
      case kVK_ANSI_U:
        return "u";
      case kVK_ANSI_LeftBracket:
        return "bracketleft";
      case kVK_ANSI_I:
        return "i";
      case kVK_ANSI_P:
        return "p";
      case kVK_ANSI_L:
        return "l";
      case kVK_ANSI_J:
        return "j";
      case kVK_ANSI_Quote:
        return "apostrophe";
      case kVK_ANSI_K:
        return "k";
      case kVK_ANSI_Semicolon:
        return "semicolon";
      case kVK_ANSI_Backslash:
        return "backslash";
      case kVK_ANSI_Comma:
        return "comma";
      case kVK_ANSI_Slash:
        return "slash";
      case kVK_ANSI_N:
        return "n";
      case kVK_ANSI_M:
        return "m";
      case kVK_ANSI_Period:
        return "period";
      case kVK_ANSI_Grave:
        return "grave";
      case kVK_ANSI_KeypadDecimal:
        return "KP_Delete";
      case kVK_ANSI_KeypadMultiply:
        return "KP_Multiply";
      case kVK_ANSI_KeypadPlus:
        return "KP_Add";
      case kVK_ANSI_KeypadClear:
        return "KP_Clear";
      case kVK_ANSI_KeypadDivide:
        return "KP_Divide";
      case kVK_ANSI_KeypadEnter:
        return "KP_Enter";
      case kVK_ANSI_KeypadMinus:
        return "KP_Subtract";
      case kVK_ANSI_KeypadEquals:
        return "KP_Equals";
      case kVK_ANSI_Keypad0:
        return "KP_Insert";
      case kVK_ANSI_Keypad1:
        return "KP_End";
      case kVK_ANSI_Keypad2:
        return "KP_Down";
      case kVK_ANSI_Keypad3:
        return "KP_Next";
      case kVK_ANSI_Keypad4:
        return "KP_Left";
      case kVK_ANSI_Keypad5:
        return "KP_Begin";
      case kVK_ANSI_Keypad6:
        return "KP_Right";
      case kVK_ANSI_Keypad7:
        return "KP_Home";
      case kVK_ANSI_Keypad8:
        return "KP_Up";
      case kVK_ANSI_Keypad9:
        return "KP_Prior";

    /* keycodes for keys that are independent of keyboard layout*/

      case kVK_Return:
        return "Return";
      case kVK_Tab:
        return "Tab";
      case kVK_Space:
        return "space";
      case kVK_Delete:
        return "Backspace";
      case kVK_Escape:
        return "Escape";
      case kVK_Command:
        return "Command";
      case kVK_Shift:
        return "Shift_L";
      case kVK_CapsLock:
        return "Caps_Lock";
      case kVK_Option:
        return "Option_L";
      case kVK_Control:
        return "Control_L";
      case kVK_RightShift:
        return "Shift_R";
      case kVK_RightOption:
        return "Option_R";
      case kVK_RightControl:
        return "Control_R";
      case kVK_Function:
        return "Function";
      case kVK_F17:
        return "F17";
      case kVK_VolumeUp:
        return "VolumeUp";
      case kVK_VolumeDown:
        return "VolumeDown";
      case kVK_Mute:
        return "Mute";
      case kVK_F18:
        return "F18";
      case kVK_F19:
        return "F19";
      case kVK_F20:
        return "F20";
      case kVK_F5:
        return "F5";
      case kVK_F6:
        return "F6";
      case kVK_F7:
        return "F7";
      case kVK_F3:
        return "F3";
      case kVK_F8:
        return "F8";
      case kVK_F9:
        return "F9";
      case kVK_F11:
        return "F11";
      case kVK_F13:
        return "F13";
      case kVK_F16:
        return "F16";
      case kVK_F14:
        return "F14";
      case kVK_F10:
        return "F10";
      case kVK_F12:
        return "F12";
      case kVK_F15:
        return "F15";
      case kVK_Help:
        return "Help";
      case kVK_Home:
        return "Home";
      case kVK_PageUp:
        return "Prior";
      case kVK_ForwardDelete:
        return "Delete";
      case kVK_F4:
        return "F4";
      case kVK_End:
        return "End";
      case kVK_F2:
        return "F2";
      case kVK_PageDown:
        return "Next";
      case kVK_F1:
        return "F1";
      case kVK_LeftArrow:
        return "Left";
      case kVK_RightArrow:
        return "Right";
      case kVK_DownArrow:
        return "Down";
      case kVK_UpArrow:
        return "Up";
    default:
        return "";
  };
}

@ implementation GstOSXVideoSinkWindow

/* A borderless window is not allowed to become key by default, which would
   cost us every keystroke while filling the screen. */
- (BOOL) canBecomeKeyWindow { return YES; }
- (BOOL) canBecomeMainWindow { return YES; }


/* The object has to be released */
- (id) initWithContentNSRect: (NSRect) rect
		 styleMask: (unsigned int) styleMask
		   backing: (NSBackingStoreType) bufferingType
		     defer: (BOOL) flag
		    screen:(NSScreen *) aScreen
{
  self = [super initWithContentRect: rect
		styleMask: styleMask
		backing: bufferingType
		defer: flag
		screen:aScreen];

  GST_DEBUG ("Initializing GstOSXvideoSinkWindow");

  gstview = [[GstGLView alloc] initWithFrame:rect];

  if (gstview)
    [self setContentView:gstview];
  [self setTitle:@"GStreamer Video Output"];

  return self;
}

- (void) setContentSize:(NSSize) size {
  width = size.width;
  height = size.height;

  [super setContentSize:size];
}

- (GstGLView *) gstView {
  return gstview;
}

- (void) awakeFromNib {
  [self setAcceptsMouseMovedEvents:YES];
}

@end


//
// OpenGL implementation
//

@ implementation GstGLView

- (id) initWithFrame:(NSRect) frame {
  NSOpenGLPixelFormat *fmt;
  NSOpenGLPixelFormatAttribute attribs[] = {
    NSOpenGLPFANoRecovery,
    NSOpenGLPFADoubleBuffer,
    NSOpenGLPFAColorSize, 24,
    NSOpenGLPFAAlphaSize, 8,
    NSOpenGLPFADepthSize, 24,
#if MAC_OS_X_VERSION_MAX_ALLOWED < 1090
    NSOpenGLPFAWindow,
#endif
    0
  };

  fmt = [[NSOpenGLPixelFormat alloc]
	  initWithAttributes:attribs];

  if (!fmt) {
    GST_WARNING ("Cannot create NSOpenGLPixelFormat");
    return nil;
  }

  self = [super initWithFrame: frame pixelFormat:fmt];
  [fmt release];

   actualContext = [self openGLContext];
   [actualContext makeCurrentContext];
   [actualContext update];

  /* Black background */
  glClearColor (0.0, 0.0, 0.0, 0.0);

  pi_texture = 0;
  /* Until the first report arrives; starting at zero drew an empty slider over
     a stream that was playing at full volume. */
  osdVolume = 1.0f;
  data = nil;
  width = frame.size.width * [[NSScreen mainScreen] backingScaleFactor];
  height = frame.size.height * [[NSScreen mainScreen] backingScaleFactor];
  drawingBounds = NSMakeRect(0, 0, width, height);

  GST_LOG ("Width: %d Height: %d", width, height);

  trackingArea = [[NSTrackingArea alloc] initWithRect:[self bounds]
      options: (NSTrackingMouseEnteredAndExited | NSTrackingMouseMoved | NSTrackingActiveInKeyWindow)
      owner:self
      userInfo:nil];

  [self addTrackingArea:trackingArea];
  mainThread = [NSThread mainThread];

  [self initTextures];
  return self;
}

- (NSRect) getDrawingBounds {
  return drawingBounds;
}

- (void) reshape {
  NSRect bounds;
  gdouble frame_par, view_par;
  gint view_height, view_width, c_height, c_width, c_x, c_y, scale_factor;
  gboolean pin_height;

  [super reshape];

  GST_LOG ("reshaping");

  if (!initDone) {
    return;
  }

  [actualContext makeCurrentContext];

  bounds = [self bounds];
  view_width = bounds.size.width;
  view_height = bounds.size.height;
  scale_factor = [[NSScreen mainScreen] backingScaleFactor];

  frame_par = (gdouble) width / height;
  view_par = (gdouble) view_width / view_height;
  if (!keepAspectRatio)
    view_par = frame_par;

  /* Pin one axis to the view and let the frame's aspect decide the other. FIT
     picks whichever axis keeps the whole frame inside, which is what leaves the
     bars; the FILL modes name the axis outright, so the other one overflows and
     is cropped. Overflow makes c_x or c_y negative, which glViewport accepts —
     GL clips whatever falls outside the framebuffer, so cropping costs nothing. */
  pin_height = (fillMode == GST_OSX_FILL_HEIGHT) ||
      (fillMode == GST_OSX_FILL_FIT && frame_par < view_par);

  if (frame_par == view_par) {
    c_height = view_height;
    c_width = view_width;
    c_x = 0;
    c_y = 0;
  } else if (pin_height) {
    c_height = view_height;
    c_width = c_height * frame_par;
    c_x = (view_width - c_width) / 2;
    c_y = 0;
  } else {
    c_width = view_width;
    c_height = c_width / frame_par;
    c_x = 0;
    c_y = (view_height - c_height) / 2;
  }

  drawingBounds = NSMakeRect(c_x * scale_factor, c_y * scale_factor,
    c_width * scale_factor, c_height * scale_factor);
}

- (void) initTextures {

  [actualContext makeCurrentContext];

  /* Hold on to the outgoing texture: it still holds the last frame shown, and
     re-initialising happens on a seek, where the picture should stay put until
     the new frame decodes rather than blink. */
  if (prevTexture) {
    glDeleteTextures (1, (GLuint *) &prevTexture);
    prevTexture = 0;
  }
  if (pi_texture) {
    prevTexture = pi_texture;
    prevWidth = allocWidth;
    prevHeight = allocHeight;
    pi_texture = 0;
    awaitingFirstFrame = YES;
  }

  /* Nothing here is cleared. The buffer is about to be overwritten by the
     next frame, and until that frame arrives the previous texture stays on
     screen (see prevTexture below), so there is no moment where the contents
     of this buffer are shown. Clearing it only ever produced a flash -- green,
     since zero in this chroma format is Y=0 with both components at 0. */
  if (data == NULL || width != allocWidth || height != allocHeight) {
    g_free (data);
    data = g_malloc ((gsize) width * (gsize) height * sizeof (short));
    allocWidth = width;
    allocHeight = height;
  }
  /* Create textures */
  glGenTextures (1, (GLuint *)&pi_texture);

  glEnable (GL_TEXTURE_RECTANGLE_EXT);
  glEnable (GL_UNPACK_CLIENT_STORAGE_APPLE);

  glPixelStorei (GL_UNPACK_ALIGNMENT, 1);
  glPixelStorei (GL_UNPACK_ROW_LENGTH, width);

  glBindTexture (GL_TEXTURE_RECTANGLE_EXT, pi_texture);

  /* Use VRAM texturing */
  glTexParameteri (GL_TEXTURE_RECTANGLE_EXT,
		   GL_TEXTURE_STORAGE_HINT_APPLE, GL_STORAGE_CACHED_APPLE);

  /* Tell the driver not to make a copy of the texture but to use
     our buffer */
  glPixelStorei (GL_UNPACK_CLIENT_STORAGE_APPLE, GL_TRUE);

  /* Linear interpolation */
  glTexParameteri (GL_TEXTURE_RECTANGLE_EXT, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
  glTexParameteri (GL_TEXTURE_RECTANGLE_EXT, GL_TEXTURE_MAG_FILTER, GL_LINEAR);

  /* I have no idea what this exactly does, but it seems to be
     necessary for scaling */
  glTexParameteri (GL_TEXTURE_RECTANGLE_EXT,
		   GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
  glTexParameteri (GL_TEXTURE_RECTANGLE_EXT,
		   GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
  // glPixelStorei (GL_UNPACK_ROW_LENGTH, 0); WHY ??

  glTexImage2D (GL_TEXTURE_RECTANGLE_EXT, 0, GL_RGBA,
		width, height, 0,
		GL_YCBCR_422_APPLE, GL_UNSIGNED_SHORT_8_8_APPLE, data);


  initDone = 1;
}

- (void) reloadTexture {
  if (!initDone) {
    return;
  }

  GST_LOG ("Reloading Texture");

  [actualContext makeCurrentContext];

  glBindTexture (GL_TEXTURE_RECTANGLE_EXT, pi_texture);
  glPixelStorei (GL_UNPACK_ROW_LENGTH, width);
  if (awaitingFirstFrame) {
    awaitingFirstFrame = NO;
    if (prevTexture) {
      glDeleteTextures (1, (GLuint *) &prevTexture);
      prevTexture = 0;
    }
  }

  /* glTexSubImage2D is faster than glTexImage2D
     http://developer.apple.com/samplecode/Sample_Code/Graphics_3D/
     TextureRange/MainOpenGLView.m.htm */
  glTexSubImage2D (GL_TEXTURE_RECTANGLE_EXT, 0, 0, 0,
		   width, height,
		   GL_YCBCR_422_APPLE, GL_UNSIGNED_SHORT_8_8_APPLE, data);    //FIXME
}

- (void) cleanUp {
  initDone = 0;
}


/* Bakes an SF Symbol into a texture once, so the OSD can show the same speaker
   glyph the system uses rather than something hand-drawn from polygons. */
- (void) ensureOSDIcon
{
  GLint saved_alignment = 4, saved_row_length = 0;
  GLboolean saved_client_storage = GL_FALSE;
  NSImage *symbol;
  NSBitmapImageRep *rep;
  NSSize size = NSMakeSize (64, 64);

  if (osdIconTexture != 0) {
    return;
  }
  if (![NSImage respondsToSelector:
          @selector(imageWithSystemSymbolName:accessibilityDescription:)]) {
    return;
  }
  symbol = [NSImage imageWithSystemSymbolName: @"speaker.wave.2.fill"
                     accessibilityDescription: @"Volume"];
  if (symbol == nil) {
    return;
  }
  symbol = [symbol imageWithSymbolConfiguration:
      [NSImageSymbolConfiguration configurationWithPointSize: 44
                                                      weight: NSFontWeightRegular]];
  if (symbol == nil) {
    return;
  }

  rep = [[[NSBitmapImageRep alloc]
      initWithBitmapDataPlanes: NULL
                    pixelsWide: (NSInteger) size.width
                    pixelsHigh: (NSInteger) size.height
                 bitsPerSample: 8
               samplesPerPixel: 4
                      hasAlpha: YES
                      isPlanar: NO
                colorSpaceName: NSCalibratedRGBColorSpace
                   bytesPerRow: (NSInteger) size.width * 4
                  bitsPerPixel: 32] autorelease];
  if (rep == nil) {
    return;
  }

  [NSGraphicsContext saveGraphicsState];
  [NSGraphicsContext setCurrentContext:
      [NSGraphicsContext graphicsContextWithBitmapImageRep: rep]];
  [[NSColor clearColor] set];
  NSRectFill (NSMakeRect (0, 0, size.width, size.height));
  {
    NSSize drawn = [symbol size];
    NSRect where = NSMakeRect ((size.width - drawn.width) / 2.0,
        (size.height - drawn.height) / 2.0, drawn.width, drawn.height);
    [[NSColor whiteColor] set];
    [symbol drawInRect: where fromRect: NSZeroRect
             operation: NSCompositingOperationSourceOver fraction: 1.0];
    NSRectFillUsingOperation (NSMakeRect (0, 0, size.width, size.height),
        NSCompositingOperationSourceAtop);
  }
  [NSGraphicsContext restoreGraphicsState];

  glGenTextures (1, (GLuint *) &osdIconTexture);
  glBindTexture (GL_TEXTURE_2D, osdIconTexture);
  /* The video texture is uploaded with Apple client storage, where the driver
     keeps the caller's pointer instead of copying. That must be off here: the
     bitmap backing this icon is autoreleased, and the driver would be left
     reading freed memory on every later draw. */
  glGetIntegerv (GL_UNPACK_ALIGNMENT, &saved_alignment);
  glGetIntegerv (GL_UNPACK_ROW_LENGTH, &saved_row_length);
  glGetBooleanv (GL_UNPACK_CLIENT_STORAGE_APPLE, &saved_client_storage);
  glPixelStorei (GL_UNPACK_CLIENT_STORAGE_APPLE, GL_FALSE);
  glPixelStorei (GL_UNPACK_ALIGNMENT, 1);
  glPixelStorei (GL_UNPACK_ROW_LENGTH, 0);
  glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
  glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
  glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
  glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
  glTexImage2D (GL_TEXTURE_2D, 0, GL_RGBA, (GLsizei) size.width,
      (GLsizei) size.height, 0, GL_RGBA, GL_UNSIGNED_BYTE,
      [rep bitmapData]);
  osdIconWidth = (int) size.width;
  osdIconHeight = (int) size.height;

  /* Restore what initTextures set up for the video texture. */
  /* Put back exactly what was there. Guessing at the values instead is what
     let these bakes disturb the video texture's own upload. */
  glPixelStorei (GL_UNPACK_CLIENT_STORAGE_APPLE, saved_client_storage);
  glPixelStorei (GL_UNPACK_ALIGNMENT, saved_alignment);
  glPixelStorei (GL_UNPACK_ROW_LENGTH, saved_row_length);
}

static void
osd_fill_round_rect (float x, float y, float w, float h, float r)
{
  const int steps = 6;
  int corner, i;

  glBegin (GL_TRIANGLE_FAN);
  glVertex2f (x + w / 2.0f, y + h / 2.0f);
  for (corner = 0; corner < 4; corner++) {
    float cx = (corner == 0 || corner == 3) ? x + r : x + w - r;
    float cy = (corner < 2) ? y + r : y + h - r;
    float base = (corner == 0) ? 180.0f : (corner == 1) ? 270.0f :
        (corner == 2) ? 0.0f : 90.0f;

    for (i = 0; i <= steps; i++) {
      float a = (base + 90.0f * i / steps) * (float) M_PI / 180.0f;
      glVertex2f (cx + r * cosf (a), cy + r * sinf (a));
    }
  }
  /* Close the fan on the first arc vertex. Anything else adds a stray
     triangle from the centre out to it, and since the panel is translucent
     that triangle blends over the panel a second time and reads as a black
     wedge lying across the video. */
  glVertex2f (x, y + r);
  glEnd ();
}


#define CONTROLS_VISIBLE_SECS  2.5
#define CONTROLS_FADE_SECS     0.5
#define CLOSE_BUTTON_MARGIN    22.0f
#define CLOSE_BUTTON_SIZE      34.0f
#define PLAYBACK_BAR_MARGIN    36.0f
#define PLAYBACK_BAR_HEIGHT    44.0f     /* one row of the transport panel */
#define PLAYBACK_BAR_MAX_WIDTH 520.0f
#define PLAYBACK_BAR_MIN_WIDTH 360.0f
#define PLAYBACK_TRACK_HEIGHT  6.0f
#define PLAYBACK_CLOCK_WIDTH   52.0f
#define PLAYBACK_TEXT_INSET    16.0f
#define TRANSPORT_ICON         22.0f
#define TRANSPORT_HIT          34.0f
#define TRANSPORT_VOLUME_WIDTH 104.0f

/* Where the close button sits, in view coordinates with the origin at the
   bottom left, matching the OSD's projection. */
- (NSRect) closeButtonRect
{
  NSRect b = [self bounds];

  return NSMakeRect (CLOSE_BUTTON_MARGIN,
      b.size.height - CLOSE_BUTTON_MARGIN - CLOSE_BUTTON_SIZE,
      CLOSE_BUTTON_SIZE, CLOSE_BUTTON_SIZE);
}

/* Any sign of the user being present brings the button back. */
- (void) noteUserActivity
{
  controlsExpiry = [NSDate timeIntervalSinceReferenceDate] + CONTROLS_VISIBLE_SECS;
  if (osdTimer == nil) {
    osdTimer = [NSTimer scheduledTimerWithTimeInterval: 1.0 / 30.0
                                                target: self
                                              selector: @selector(osdTick:)
                                              userInfo: nil
                                               repeats: YES];
  }
  [self setNeedsDisplay: YES];
}

- (void) setPlaybackPosition: (NSNumber *) seconds
{
  osdPosition = [seconds doubleValue];
  if (osdDuration > 0.0) {
    [self setNeedsDisplay: YES];
  }
}

- (void) setPlaybackDuration: (NSNumber *) seconds
{
  osdDuration = [seconds doubleValue];
  [self setNeedsDisplay: YES];
}

- (void) setPlaybackRate: (NSNumber *) rate
{
  osdRate = [rate doubleValue];
  [self setNeedsDisplay: YES];
}

/* Layout of the transport panel, shaped after the controls macOS puts on
   screen while it is receiving AirPlay video: one rounded panel with volume
   and the transport buttons on the top row, and the scrubber between the two
   clock readings on the bottom. Mirroring has no timeline, so it gets the top
   row alone. */
- (BOOL) transportPanel: (NSRect *) panel
{
  NSRect b = [self bounds];
  CGFloat w = b.size.width - 2.0 * PLAYBACK_BAR_MARGIN;
  CGFloat h, x, y;

  if (w > PLAYBACK_BAR_MAX_WIDTH) {
    w = PLAYBACK_BAR_MAX_WIDTH;
  }
  if (w < PLAYBACK_BAR_MIN_WIDTH) {
    return NO;
  }
  h = (osdDuration > 0.0) ? 2.0 * PLAYBACK_BAR_HEIGHT : PLAYBACK_BAR_HEIGHT;
  x = round ((b.size.width - w) / 2.0) + panelOffset.x;
  y = PLAYBACK_BAR_MARGIN + panelOffset.y;

  /* Keep it on screen however far it has been dragged. */
  if (x < 8.0) {
    x = 8.0;
  } else if (x > b.size.width - w - 8.0) {
    x = b.size.width - w - 8.0;
  }
  if (y < 8.0) {
    y = 8.0;
  } else if (y > b.size.height - h - 8.0) {
    y = b.size.height - h - 8.0;
  }
  *panel = NSMakeRect (round (x), round (y), w, h);
  return YES;
}

/* Top row: volume on the left, transport in the middle, fullscreen on the
   right. It is the whole panel when there is no timeline. */
- (NSRect) transportTopRow: (NSRect) panel
{
  if (osdDuration > 0.0) {
    return NSMakeRect (NSMinX (panel), NSMaxY (panel) - PLAYBACK_BAR_HEIGHT,
        NSWidth (panel), PLAYBACK_BAR_HEIGHT);
  }
  return panel;
}

- (NSRect) transportVolumeTrack: (NSRect) panel
{
  NSRect row = [self transportTopRow: panel];
  CGFloat x = NSMinX (row) + PLAYBACK_TEXT_INSET + TRANSPORT_ICON + 12.0;

  return NSMakeRect (x, NSMidY (row) - PLAYBACK_TRACK_HEIGHT / 2.0,
      TRANSPORT_VOLUME_WIDTH, PLAYBACK_TRACK_HEIGHT);
}

- (NSRect) transportButton: (NSRect) panel index: (int) which
{
  NSRect row = [self transportTopRow: panel];
  CGFloat cx = NSMidX (row) + which * 52.0;

  return NSMakeRect (cx - TRANSPORT_HIT / 2.0, NSMidY (row) - TRANSPORT_HIT / 2.0,
      TRANSPORT_HIT, TRANSPORT_HIT);
}

- (NSRect) transportFullScreenButton: (NSRect) panel
{
  NSRect row = [self transportTopRow: panel];

  return NSMakeRect (NSMaxX (row) - PLAYBACK_TEXT_INSET - TRANSPORT_HIT,
      NSMidY (row) - TRANSPORT_HIT / 2.0, TRANSPORT_HIT, TRANSPORT_HIT);
}

/* Bottom row, and the scrubber within it. NO when there is no timeline. */
- (BOOL) transportProgressTrack: (NSRect) panel into: (NSRect *) track
{
  CGFloat x, w;

  if (osdDuration <= 0.0) {
    return NO;
  }
  x = NSMinX (panel) + PLAYBACK_TEXT_INSET + PLAYBACK_CLOCK_WIDTH + 12.0;
  w = NSWidth (panel) - 2.0 * (PLAYBACK_TEXT_INSET + PLAYBACK_CLOCK_WIDTH + 12.0);
  if (w < 40.0) {
    return NO;
  }
  *track = NSMakeRect (x,
      NSMinY (panel) + PLAYBACK_BAR_HEIGHT / 2.0 - PLAYBACK_TRACK_HEIGHT / 2.0,
      w, PLAYBACK_TRACK_HEIGHT);
  return YES;
}

/* mm:ss, or h:mm:ss once the stream is over an hour. */
static NSString *
osd_clock_string (double seconds)
{
  long total = (long) (seconds < 0.0 ? 0.0 : seconds);

  if (total >= 3600) {
    return [NSString stringWithFormat: @"%ld:%02ld:%02ld",
        total / 3600, (total % 3600) / 60, total % 60];
  }
  return [NSString stringWithFormat: @"%ld:%02ld", total / 60, total % 60];
}

/* Both readings live in one texture the size of the panel, baked at the
   screen's own scale and drawn one-to-one. An earlier version baked a small
   strip and stretched it to the panel width, which spread the glyphs sideways
   and blurred them. */
- (void) ensureTimeTextureForWidth: (CGFloat) w scale: (CGFloat) scale
{
  GLint saved_alignment = 4, saved_row_length = 0;
  GLboolean saved_client_storage = GL_FALSE;
  NSString *elapsed = osd_clock_string (osdPosition);
  NSString *total = osd_clock_string (osdDuration);
  NSString *wanted = [NSString stringWithFormat: @"%@|%@|%.0f|%.1f",
      elapsed, total, w, scale];
  NSBitmapImageRep *rep;
  NSDictionary *attrs;
  NSInteger px = (NSInteger) round (w * scale);
  NSInteger py = (NSInteger) round (PLAYBACK_BAR_HEIGHT * scale);

  if (timeTexString != nil && [timeTexString isEqualToString: wanted]) {
    return;
  }
  if (px <= 0 || py <= 0) {
    return;
  }

  rep = [[[NSBitmapImageRep alloc]
      initWithBitmapDataPlanes: NULL
                    pixelsWide: px
                    pixelsHigh: py
                 bitsPerSample: 8
               samplesPerPixel: 4
                      hasAlpha: YES
                      isPlanar: NO
                colorSpaceName: NSCalibratedRGBColorSpace
                   bytesPerRow: px * 4
                  bitsPerPixel: 32] autorelease];
  if (rep == nil) {
    return;
  }
  /* Tell the rep its size in points, so drawing happens at the screen's scale
     rather than one glyph pixel per texture pixel. */
  [rep setSize: NSMakeSize (w, PLAYBACK_BAR_HEIGHT)];

  attrs = [NSDictionary dictionaryWithObjectsAndKeys:
      [NSFont monospacedDigitSystemFontOfSize: 15
                                       weight: NSFontWeightMedium], NSFontAttributeName,
      [NSColor whiteColor], NSForegroundColorAttributeName, nil];

  [NSGraphicsContext saveGraphicsState];
  [NSGraphicsContext setCurrentContext:
      [NSGraphicsContext graphicsContextWithBitmapImageRep: rep]];
  [[NSColor clearColor] set];
  NSRectFill (NSMakeRect (0, 0, w, PLAYBACK_BAR_HEIGHT));
  {
    NSSize es = [elapsed sizeWithAttributes: attrs];
    NSSize ts = [total sizeWithAttributes: attrs];

    [elapsed drawAtPoint: NSMakePoint (PLAYBACK_TEXT_INSET,
        (PLAYBACK_BAR_HEIGHT - es.height) / 2.0) withAttributes: attrs];
    [total drawAtPoint: NSMakePoint (w - PLAYBACK_TEXT_INSET - ts.width,
        (PLAYBACK_BAR_HEIGHT - ts.height) / 2.0) withAttributes: attrs];
  }
  [NSGraphicsContext restoreGraphicsState];

  if (timeTexture == 0) {
    glGenTextures (1, (GLuint *) &timeTexture);
  }
  glBindTexture (GL_TEXTURE_2D, timeTexture);
  /* Client storage would leave the driver holding this autoreleased bitmap;
     see ensureOSDIcon. */
  glGetIntegerv (GL_UNPACK_ALIGNMENT, &saved_alignment);
  glGetIntegerv (GL_UNPACK_ROW_LENGTH, &saved_row_length);
  glGetBooleanv (GL_UNPACK_CLIENT_STORAGE_APPLE, &saved_client_storage);
  glPixelStorei (GL_UNPACK_CLIENT_STORAGE_APPLE, GL_FALSE);
  glPixelStorei (GL_UNPACK_ALIGNMENT, 1);
  glPixelStorei (GL_UNPACK_ROW_LENGTH, 0);
  glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
  glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
  glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
  glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
  glTexImage2D (GL_TEXTURE_2D, 0, GL_RGBA, (GLsizei) px, (GLsizei) py, 0,
      GL_RGBA, GL_UNSIGNED_BYTE, [rep bitmapData]);
  timeTexWidth = (int) px;
  timeTexHeight = (int) py;

  /* Put back exactly what was there. Guessing at the values instead is what
     let these bakes disturb the video texture's own upload. */
  glPixelStorei (GL_UNPACK_CLIENT_STORAGE_APPLE, saved_client_storage);
  glPixelStorei (GL_UNPACK_ALIGNMENT, saved_alignment);
  glPixelStorei (GL_UNPACK_ROW_LENGTH, saved_row_length);

  [timeTexString release];
  timeTexString = [wanted retain];
}

/* Simple filled shapes for the transport glyphs; drawing them as geometry
   keeps them crisp at any size and avoids another texture to manage. */
static void
osd_fill_quad (float x, float y, float w, float h)
{
  glBegin (GL_QUADS);
  glVertex2f (x, y);
  glVertex2f (x + w, y);
  glVertex2f (x + w, y + h);
  glVertex2f (x, y + h);
  glEnd ();
}

static void
osd_fill_triangle (float x0, float y0, float x1, float y1, float x2, float y2)
{
  glBegin (GL_TRIANGLES);
  glVertex2f (x0, y0);
  glVertex2f (x1, y1);
  glVertex2f (x2, y2);
  glEnd ();
}

static void
osd_fill_disc (float cx, float cy, float r)
{
  int i;

  glBegin (GL_TRIANGLE_FAN);
  glVertex2f (cx, cy);
  for (i = 0; i <= 24; i++) {
    float a = 2.0f * (float) M_PI * i / 24.0f;
    glVertex2f (cx + r * cosf (a), cy + r * sinf (a));
  }
  glEnd ();
}

/* A track with the played part filled in, plus the knob. */
static void
osd_draw_slider (NSRect track, float fraction, float alpha, BOOL knob)
{
  float h = (float) NSHeight (track);
  float filled = (float) NSWidth (track) * fraction;

  glColor4f (1.0f, 1.0f, 1.0f, 0.28f * alpha);
  osd_fill_round_rect ((float) NSMinX (track), (float) NSMinY (track),
      (float) NSWidth (track), h, h / 2.0f);
  if (filled > 0.0f) {
    if (filled < h) {
      filled = h;
    }
    glColor4f (1.0f, 1.0f, 1.0f, 0.95f * alpha);
    osd_fill_round_rect ((float) NSMinX (track), (float) NSMinY (track), filled,
        h, h / 2.0f);
  }
  if (knob) {
    glColor4f (1.0f, 1.0f, 1.0f, 0.98f * alpha);
    osd_fill_disc ((float) NSMinX (track) + (float) NSWidth (track) * fraction,
        (float) NSMidY (track), 8.0f);
  }
}

- (void) drawTransportGlyphs: (NSRect) panel alpha: (float) alpha
{
  NSRect r;
  float cx, cy, s;

  glColor4f (1.0f, 1.0f, 1.0f, 0.95f * alpha);

  /* Skip back ten seconds. */
  r = [self transportButton: panel index: -1];
  cx = (float) NSMidX (r);
  cy = (float) NSMidY (r);
  s = 5.0f;
  osd_fill_triangle (cx - 1.0f, cy + s, cx - 1.0f, cy - s, cx - 1.0f - s * 1.2f, cy);
  osd_fill_triangle (cx + s * 1.2f + 1.0f, cy + s, cx + s * 1.2f + 1.0f, cy - s,
      cx + 1.0f, cy);

  /* Play or pause, whichever the stream is not doing now. */
  r = [self transportButton: panel index: 0];
  cx = (float) NSMidX (r);
  cy = (float) NSMidY (r);
  if (osdRate > 0.0) {
    osd_fill_quad (cx - 6.0f, cy - 8.0f, 4.0f, 16.0f);
    osd_fill_quad (cx + 2.0f, cy - 8.0f, 4.0f, 16.0f);
  } else {
    osd_fill_triangle (cx - 5.0f, cy + 8.0f, cx - 5.0f, cy - 8.0f, cx + 8.0f, cy);
  }

  /* Skip forward ten seconds. */
  r = [self transportButton: panel index: 1];
  cx = (float) NSMidX (r);
  cy = (float) NSMidY (r);
  s = 5.0f;
  osd_fill_triangle (cx - s * 1.2f - 1.0f, cy + s, cx - s * 1.2f - 1.0f, cy - s,
      cx - 1.0f, cy);
  osd_fill_triangle (cx + 1.0f, cy + s, cx + 1.0f, cy - s, cx + 1.0f + s * 1.2f, cy);

  /* Fullscreen: a screen outline with a stand, as macOS draws it. */
  r = [self transportFullScreenButton: panel];
  cx = (float) NSMidX (r);
  cy = (float) NSMidY (r) + 1.0f;
  glLineWidth (1.6f);
  glBegin (GL_LINE_LOOP);
  glVertex2f (cx - 9.0f, cy - 6.0f);
  glVertex2f (cx + 9.0f, cy - 6.0f);
  glVertex2f (cx + 9.0f, cy + 6.0f);
  glVertex2f (cx - 9.0f, cy + 6.0f);
  glEnd ();
  osd_fill_quad (cx - 4.0f, cy - 9.0f, 8.0f, 2.0f);
}

- (void) drawTransportPanel: (float) alpha
{
  NSRect panel, vol, track;
  float fraction;

  if (![self transportPanel: &panel]) {
    return;
  }

  glColor4f (0.0f, 0.0f, 0.0f, 0.55f * alpha);
  osd_fill_round_rect ((float) NSMinX (panel), (float) NSMinY (panel),
      (float) NSWidth (panel), (float) NSHeight (panel), 16.0f);

  /* Speaker. Built here rather than only when the volume changes, so the panel
     is never drawn with a hole where it belongs. */
  [self ensureOSDIcon];
  if (osdIconTexture != 0) {
    NSRect row = [self transportTopRow: panel];
    float ix = (float) (NSMinX (row) + PLAYBACK_TEXT_INSET);
    float iy = (float) (NSMidY (row) - TRANSPORT_ICON / 2.0);

    glEnable (GL_TEXTURE_2D);
    glBindTexture (GL_TEXTURE_2D, osdIconTexture);
    glColor4f (1.0f, 1.0f, 1.0f, 0.95f * alpha);
    glBegin (GL_QUADS);
    glTexCoord2f (0.0f, 1.0f); glVertex2f (ix, iy);
    glTexCoord2f (1.0f, 1.0f); glVertex2f (ix + TRANSPORT_ICON, iy);
    glTexCoord2f (1.0f, 0.0f); glVertex2f (ix + TRANSPORT_ICON, iy + TRANSPORT_ICON);
    glTexCoord2f (0.0f, 0.0f); glVertex2f (ix, iy + TRANSPORT_ICON);
    glEnd ();
    glDisable (GL_TEXTURE_2D);
  }

  vol = [self transportVolumeTrack: panel];
  osd_draw_slider (vol, osdVolume, alpha, YES);

  if (osdDuration <= 0.0) {
    return;                     /* mirroring: no transport, no scrubber */
  }

  [self drawTransportGlyphs: panel alpha: alpha];

  {
    CGFloat scale = [[self window] backingScaleFactor];

    if (scale <= 0.0) {
      scale = 1.0;
    }
    [self ensureTimeTextureForWidth: NSWidth (panel) scale: scale];
  }
  if (timeTexture != 0) {
    float tx = (float) NSMinX (panel);
    float ty = (float) NSMinY (panel);
    float tw = (float) NSWidth (panel);
    float th = PLAYBACK_BAR_HEIGHT;

    glEnable (GL_TEXTURE_2D);
    glBindTexture (GL_TEXTURE_2D, timeTexture);
    glColor4f (1.0f, 1.0f, 1.0f, 0.9f * alpha);
    glBegin (GL_QUADS);
    glTexCoord2f (0.0f, 1.0f); glVertex2f (tx, ty);
    glTexCoord2f (1.0f, 1.0f); glVertex2f (tx + tw, ty);
    glTexCoord2f (1.0f, 0.0f); glVertex2f (tx + tw, ty + th);
    glTexCoord2f (0.0f, 0.0f); glVertex2f (tx, ty + th);
    glEnd ();
    glDisable (GL_TEXTURE_2D);
  }

  if ([self transportProgressTrack: panel into: &track]) {
    fraction = (float) (osdPosition / osdDuration);
    if (fraction < 0.0f) {
      fraction = 0.0f;
    } else if (fraction > 1.0f) {
      fraction = 1.0f;
    }
    osd_draw_slider (track, fraction, alpha, YES);
  }
}

- (void) drawCloseButton
{
  double now = [NSDate timeIntervalSinceReferenceDate];
  double left = controlsExpiry - now;
  float alpha, cx, cy, r, arm;
  NSRect rect;
  int i;

  if (left <= 0.0) {
    return;
  }
  alpha = (left < CONTROLS_FADE_SECS) ? (float) (left / CONTROLS_FADE_SECS) : 1.0f;

  rect = [self closeButtonRect];
  cx = (float) NSMidX (rect);
  cy = (float) NSMidY (rect);
  r = (float) rect.size.width / 2.0f;

  glColor4f (0.0f, 0.0f, 0.0f, 0.55f * alpha);
  glBegin (GL_TRIANGLE_FAN);
  glVertex2f (cx, cy);
  for (i = 0; i <= 24; i++) {
    float a = 2.0f * (float) M_PI * i / 24.0f;
    glVertex2f (cx + r * cosf (a), cy + r * sinf (a));
  }
  glEnd ();

  arm = r * 0.42f;
  glColor4f (1.0f, 1.0f, 1.0f, 0.95f * alpha);
  glLineWidth (2.0f);
  glBegin (GL_LINES);
  glVertex2f (cx - arm, cy - arm); glVertex2f (cx + arm, cy + arm);
  glVertex2f (cx - arm, cy + arm); glVertex2f (cx + arm, cy - arm);
  glEnd ();
}

- (void) drawVolumeOSD
{
  double now = [NSDate timeIntervalSinceReferenceDate];
  double left = controlsExpiry - now;
  float alpha;
  NSRect b;
  CGFloat scale;

  b = [self bounds];
  scale = [[self window] backingScaleFactor];
  if (scale <= 0.0) {
    scale = 1.0;
  }

  /* The video pass leaves a viewport covering only the picture; the OSD is
     placed against the view instead, so it gets its own pixel-space one. */
  glViewport (0, 0, (GLint) (b.size.width * scale),
      (GLint) (b.size.height * scale));
  glMatrixMode (GL_PROJECTION);
  glPushMatrix ();
  glLoadIdentity ();
  glOrtho (0, b.size.width, 0, b.size.height, -1, 1);
  glMatrixMode (GL_MODELVIEW);
  glPushMatrix ();
  glLoadIdentity ();

  glDisable (GL_TEXTURE_RECTANGLE_EXT);
  glEnable (GL_BLEND);
  glBlendFunc (GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

  if (left > 0.0) {
    alpha = (left < CONTROLS_FADE_SECS) ?
        (float) (left / CONTROLS_FADE_SECS) : 1.0f;
  } else {
    alpha = 0.0f;
  }

  [self drawCloseButton];

  if (alpha <= 0.0f) {
    goto restore;
  }

  [self drawTransportPanel: alpha];

restore:
  glColor4f (1.0f, 1.0f, 1.0f, 1.0f);
  glDisable (GL_BLEND);
  glEnable (GL_TEXTURE_RECTANGLE_EXT);

  glMatrixMode (GL_PROJECTION);
  glPopMatrix ();
  glMatrixMode (GL_MODELVIEW);
  glPopMatrix ();
}

- (void) osdTick: (NSTimer *) timer
{
  if ([NSDate timeIntervalSinceReferenceDate] >= controlsExpiry) {
    [osdTimer invalidate];
    osdTimer = nil;
  }
  [self setNeedsDisplay: YES];
}

- (void) showVolumeOSDNumber: (NSNumber *) level
{
  [self showVolumeOSD: [level floatValue]];
}

- (void) showVolumeOSD: (float) level
{
  osdVolume = (level < 0.0f) ? 0.0f : ((level > 1.0f) ? 1.0f : level);
  [self ensureOSDIcon];
  /* The overlay is one thing, as it is on macOS: changing the volume brings up
     the same controls that moving the mouse does, on the same timer. */
  [self noteUserActivity];
}

- (void) drawQuadWidth: (int) w height: (int) h {
  f_x = 1.0;
  f_y = 1.0;

  glBegin (GL_QUADS);
  /* Top left */
  glTexCoord2f (0.0, 0.0);
  glVertex2f (-f_x, f_y);
  /* Bottom left */
  glTexCoord2f (0.0, (float) h);
  glVertex2f (-f_x, -f_y);
  /* Bottom right */
  glTexCoord2f ((float) w, (float) h);
  glVertex2f (f_x, -f_y);
  /* Top right */
  glTexCoord2f ((float) w, 0.0);
  glVertex2f (f_x, f_y);
  glEnd ();
}

- (void) drawQuad {
  [self drawQuadWidth: width height: height];
}

- (void) drawRect:(NSRect) rect {
  GLint params[] = { 1 };

  [actualContext makeCurrentContext];

  CGLSetParameter (CGLGetCurrentContext (), kCGLCPSwapInterval, params);

  /* Black background */
  glClear (GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

  glViewport ((GLint) drawingBounds.origin.x, (GLint) drawingBounds.origin.y,
    (GLint) drawingBounds.size.width, (GLint) drawingBounds.size.height);

  if (!initDone) {
    [actualContext flushBuffer];
    return;
  }

  /* Draw */
  if (awaitingFirstFrame && prevTexture) {
    glBindTexture (GL_TEXTURE_RECTANGLE_EXT, prevTexture);
    [self drawQuadWidth: prevWidth height: prevHeight];
  } else {
    glBindTexture (GL_TEXTURE_RECTANGLE_EXT, pi_texture); // FIXME
    [self drawQuad];
  }
  [self drawVolumeOSD];
  /* Draw */
  [actualContext flushBuffer];
}

- (void) displayTexture {
  if ([self lockFocusIfCanDraw]) {

    [self drawRect:[self bounds]];
    [self reloadTexture];

    [self unlockFocus];

  }

}

- (char *) getTextureBuffer {
  return data;
}

/* The dimensions the texture buffer was last allocated with. A caller filling
   the buffer must bound itself by these and not by the sink's caps: a frame
   queued before a resize reaches the main thread still has to land in the
   buffer that is actually there. */
- (void) getTextureSize: (int *) w : (int *) h {
  *w = width;
  *h = height;
}

- (void) setFullScreen:(BOOL) flag {
  if (!fullscreen && flag) {
    // go to full screen
    /* Create the new pixel format */
    NSOpenGLPixelFormat *fmt;
    NSOpenGLPixelFormatAttribute attribs[] = {
      NSOpenGLPFAAccelerated,
      NSOpenGLPFANoRecovery,
      NSOpenGLPFADoubleBuffer,
      NSOpenGLPFAColorSize, 24,
      NSOpenGLPFAAlphaSize, 8,
      NSOpenGLPFADepthSize, 24,
#if MAC_OS_X_VERSION_MAX_ALLOWED < 1060
      NSOpenGLPFAFullScreen,
#endif
      NSOpenGLPFAScreenMask,
      CGDisplayIDToOpenGLDisplayMask (kCGDirectMainDisplay),
      0
    };

    fmt = [[NSOpenGLPixelFormat alloc]
	    initWithAttributes:attribs];

    if (!fmt) {
      GST_WARNING ("Cannot create NSOpenGLPixelFormat");
      return;
    }

    /* Create the new OpenGL context */
    fullScreenContext = [[NSOpenGLContext alloc]
			  initWithFormat: fmt shareContext:nil];
    if (!fullScreenContext) {
      GST_WARNING ("Failed to create new NSOpenGLContext");
      return;
    }

    actualContext = fullScreenContext;

    /* Capture display, switch to fullscreen */
    if (CGCaptureAllDisplays () != CGDisplayNoErr) {
      GST_WARNING ("CGCaptureAllDisplays() failed");
      return;
    }
#if MAC_OS_X_VERSION_MAX_ALLOWED < 1070
    [fullScreenContext setFullScreen];
#endif
    [fullScreenContext makeCurrentContext];

    fullscreen = YES;

    [self initTextures];
    [self setNeedsDisplay:YES];

  } else if (fullscreen && !flag) {
    // fullscreen now and needs to go back to normal
    initDone = NO;

    actualContext = [self openGLContext];

    [NSOpenGLContext clearCurrentContext];
    [fullScreenContext clearDrawable];
    [fullScreenContext release];
    fullScreenContext = nil;

    CGReleaseAllDisplays ();

    [self reshape];
    [self initTextures];

    [self setNeedsDisplay:YES];

    fullscreen = NO;
    initDone = YES;
  }
}

- (void) setVideoSize: (int)w : (int)h {
  GST_LOG ("width:%d, height:%d", w, h);

  width = w;
  height = h;

  [self initTextures];
  [self reshape];
}

- (void) setFillMode: (int) mode {
  fillMode = mode;
  [self reshape];
  [self setNeedsDisplay:YES];
}

- (int) fillMode {
  return fillMode;
}

- (BOOL) isInNativeFullScreen {
  return pseudoFullScreen;
}

/* Resizing the window to cover the screen, rather than asking AppKit for a
   fullscreen space. A space is animated and lives on after the window that
   owns it is gone, which does not survive a window torn down per session:
   exiting could not finish before the teardown, and an empty space was left
   behind. Doing it by hand is synchronous, so the caller can hide or destroy
   the window on the very next line. */
- (void) enterPseudoFullScreen {
  NSWindow *win = [self window];
  NSScreen *screen = [win screen] ? [win screen] : [NSScreen mainScreen];

  if (win == nil || pseudoFullScreen) {
    return;
  }
  savedWindowFrame = [win frame];
  savedStyleMask = [win styleMask];
  pseudoFullScreen = YES;

  [win setStyleMask: NSWindowStyleMaskBorderless];
  [win setFrame: [screen frame] display: YES];
  [win makeKeyAndOrderFront: nil];
  [NSApp setPresentationOptions: NSApplicationPresentationHideMenuBar |
      NSApplicationPresentationHideDock];
}

- (void) leavePseudoFullScreen {
  NSWindow *win = [self window];

  if (win == nil || !pseudoFullScreen) {
    return;
  }
  pseudoFullScreen = NO;
  [NSApp setPresentationOptions: NSApplicationPresentationDefault];
  [win setStyleMask: savedStyleMask];
  [win setFrame: savedWindowFrame display: YES];
}

/* Enter (and a double click) goes fullscreen and pins the height so the bars an
   ordinary fit leaves above and below the frame disappear; pressing it again
   once fullscreen switches which axis is pinned, since which one crops less
   depends on whether the client is landscape or portrait. Escape leaves
   fullscreen and goes back to showing the whole frame. */
/* Double click is a plain in/out toggle, unlike Enter which also cycles which
   axis is pinned once filling. */
- (void) toggleFillScreen {
  if (pseudoFullScreen) {
    [self leaveFillScreen];
  } else {
    [self enterFillScreen];
  }
}

- (void) enterFillScreen {
  if (!pseudoFullScreen) {
    [self setFillMode: GST_OSX_FILL_HEIGHT];
    [self enterPseudoFullScreen];
  } else {
    [self setFillMode: (fillMode == GST_OSX_FILL_HEIGHT) ?
        GST_OSX_FILL_WIDTH : GST_OSX_FILL_HEIGHT];
  }
}

- (void) leaveFillScreen {
  [self leavePseudoFullScreen];
  [self setFillMode: GST_OSX_FILL_FIT];
}

- (void) setKeepAspectRatio: (BOOL) flag {
  keepAspectRatio = flag;
  [self reshape];
}

- (void) haveSuperviewReal:(NSMutableArray *)closure {
	BOOL haveSuperview = [self superview] != nil;
	[closure addObject:[NSNumber numberWithBool:haveSuperview]];
}

- (BOOL) haveSuperview {
	NSMutableArray *closure = [NSMutableArray arrayWithCapacity:1];
	[self performSelector:@selector(haveSuperviewReal:)
		onThread:mainThread
		withObject:(id)closure waitUntilDone:YES];

	return [[closure objectAtIndex:0] boolValue];
}

- (void) addToSuperviewReal:(NSView *)superview {
	NSRect bounds;
	[superview addSubview:self];
	bounds = [superview bounds];
	[self setFrame:bounds];
	[self setAutoresizingMask:NSViewWidthSizable|NSViewHeightSizable];
}

- (void) addToSuperview: (NSView *)superview {
	[self performSelector:@selector(addToSuperviewReal:)
		onThread:mainThread
		withObject:superview waitUntilDone:YES];
}

- (void) removeFromSuperview: (id)unused
{
	[self removeFromSuperview];
}

- (void) dealloc {
  GST_LOG ("dealloc called");
  if (data) g_free(data);

  if (fullScreenContext) {
    [NSOpenGLContext clearCurrentContext];
    [fullScreenContext clearDrawable];
    [fullScreenContext release];
    if (actualContext == fullScreenContext) actualContext = nil;
    fullScreenContext = nil;
  }

  [super dealloc];
}

- (void)updateTrackingAreas {
  [self removeTrackingArea:trackingArea];
  [trackingArea release];
  trackingArea = [[NSTrackingArea alloc] initWithRect: [self bounds]
      options: (NSTrackingMouseEnteredAndExited | NSTrackingMouseMoved | NSTrackingActiveInKeyWindow)
      owner:self userInfo:nil];
  [self addTrackingArea:trackingArea];
}

- (BOOL)acceptsFirstResponder {
    return YES;
}

- (void) setNavigation:(GstNavigation *)nav
{
  navigation = nav;
}

- (void)sendMouseEvent:(NSEvent *)event : (const char *)event_name
{
  NSPoint location;
  gint button;
  gdouble x, y;

  if (!navigation)
    return;

  switch ([event type]) {
    case NSEventTypeMouseMoved:
      button = 0;
      break;
    case NSEventTypeLeftMouseDown:
    case NSEventTypeLeftMouseUp:
      button = 1;
      break;
    case NSEventTypeRightMouseDown:
    case NSEventTypeRightMouseUp:
      button = 2;
      break;
    default:
      button = 3;
      break;
  }

  location = [self convertPoint:[event locationInWindow] fromView:nil];

  x = location.x;
  y = location.y;
  /* invert Y */

  y = (1 - ((gdouble) y) / [self bounds].size.height) * [self bounds].size.height;

  gst_navigation_send_mouse_event (navigation, event_name, button, x, y);
}

- (void)sendKeyEvent:(NSEvent *)event : (const char *)event_name
{
  if (!navigation)
    return;

  gst_navigation_send_key_event(navigation, event_name, gst_keycode_to_keyname([event keyCode]));
}

- (void)sendModifierKeyEvent:(NSEvent *)event
{
  NSUInteger flags = [event modifierFlags];
  const gchar* event_name = flags > savedModifierFlags ? "key-press" : "key-release";
  savedModifierFlags = flags;
  [self sendKeyEvent: event: event_name];
}

- (void)keyDown:(NSEvent *) event;
{
  NSString *chars = [event charactersIgnoringModifiers];
  unichar c = [chars length] ? [chars characterAtIndex:0] : 0;

  [self sendKeyEvent: event: "key-press"];
  [self noteUserActivity];

  if (c == NSCarriageReturnCharacter || c == NSEnterCharacter ||
      c == NSNewlineCharacter) {
    [self enterFillScreen];
    return;
  }
  if (c == 0x1B) {                      /* Escape */
    [self leaveFillScreen];
    return;
  }
  /* The arrow keys were already handed to GstNavigation above, where UxPlay
     picks them up as volume changes. Passing them on as well would walk the
     responder chain, find no taker, and beep. */
  if (c == NSUpArrowFunctionKey || c == NSDownArrowFunctionKey ||
      c == NSLeftArrowFunctionKey || c == NSRightArrowFunctionKey) {
    return;
  }
  [super keyDown: event];
}

- (void)keyUp:(NSEvent *) event;
{
  [self sendKeyEvent: event: "key-release"];
  [super keyUp: event];
}

- (void)flagsChanged:(NSEvent *) event;
{
  [self sendModifierKeyEvent: event];
  [super flagsChanged: event];
}

- (void)mouseDown:(NSEvent *) event;
{
  NSPoint where = [self convertPoint: [event locationInWindow] fromView: nil];

  [self noteUserActivity];

  /* Only while the button is actually showing, so a click on the picture is
     not swallowed by an invisible target. */
  if (controlsExpiry > [NSDate timeIntervalSinceReferenceDate] &&
      NSPointInRect (where, [self closeButtonRect])) {
    if (navigation) {
      /* Ending the session is UxPlay's to do; the key path is already carrying
         window input to it, so the button reports itself the same way. */
      gst_navigation_send_key_event (navigation, "key-press", "uxplay-disconnect");
    }
    return;
  }

  if (controlsExpiry > [NSDate timeIntervalSinceReferenceDate] &&
      [self controlsAtPoint: where begin: YES]) {
    return;
  }

  if ([event clickCount] == 2)
    [self toggleFillScreen];
  [self sendMouseEvent:event: "mouse-button-press"];
  [super mouseDown: event];
}

/* Routes a click or drag on the panel to the control under it. Returns NO when
   the point is not on the panel, so the caller can treat it as ordinary input.
   Actions the sink cannot carry out itself are reported through the same key
   channel the close button uses. */
- (BOOL) controlsAtPoint: (NSPoint) where begin: (BOOL) begin
{
  NSRect panel, vol, track;
  float fraction;
  char name[64];

  if (![self transportPanel: &panel]) {
    return NO;
  }

  vol = [self transportVolumeTrack: panel];
  /* The whole top row's left third counts as the volume control, so the
     slider is not a hairline target. */
  vol = NSMakeRect (NSMinX (vol) - 8.0, NSMinY (vol) - 16.0,
      NSWidth (vol) + 16.0, NSHeight (vol) + 32.0);

  if (begin) {
    if (!NSPointInRect (where, panel)) {
      return NO;
    }
    if (NSPointInRect (where, vol)) {
      volumeDragging = YES;
    } else if (osdDuration > 0.0 &&
        [self transportProgressTrack: panel into: &track] &&
        NSPointInRect (where, NSMakeRect (NSMinX (track) - 8.0,
            NSMinY (panel), NSWidth (track) + 16.0, PLAYBACK_BAR_HEIGHT))) {
      /* Only the track itself, not the whole row: the clock readings sit at
         either end of it, and treating those as part of the scrubber meant a
         click on the total time seeked to the end of the video, which ends the
         stream. */
      scrubbing = YES;
    } else if (osdDuration > 0.0) {
      if (NSPointInRect (where, [self transportButton: panel index: -1])) {
        [self sendControlKey: "uxplay-skip:-10"];
        return YES;
      }
      if (NSPointInRect (where, [self transportButton: panel index: 0])) {
        [self sendControlKey: "uxplay-playpause"];
        return YES;
      }
      if (NSPointInRect (where, [self transportButton: panel index: 1])) {
        [self sendControlKey: "uxplay-skip:10"];
        return YES;
      }
      if (NSPointInRect (where, [self transportFullScreenButton: panel])) {
        [self toggleFillScreen];
        return YES;
      }
    } else if (NSPointInRect (where, [self transportFullScreenButton: panel])) {
      [self toggleFillScreen];
      return YES;
    }

    if (!volumeDragging && !scrubbing) {
      /* Anywhere on the panel that is not a control is a handle: the panel can
         sit over something the user wants to see. */
      panelDragging = YES;
      panelDragAnchor = where;
      panelOffsetAtAnchor = panelOffset;
    }
    return YES;
  }

  if (panelDragging) {
    NSRect placed;

    panelOffset.x = panelOffsetAtAnchor.x + (where.x - panelDragAnchor.x);
    panelOffset.y = panelOffsetAtAnchor.y + (where.y - panelDragAnchor.y);
    /* Fold the on-screen clamp back into the offset, so dragging past an edge
       does not build up slack the user has to wind off again. */
    if ([self transportPanel: &placed]) {
      NSRect b = [self bounds];

      panelOffset.x = NSMinX (placed) - round ((b.size.width - NSWidth (placed)) / 2.0);
      panelOffset.y = NSMinY (placed) - PLAYBACK_BAR_MARGIN;
    }
    [self noteUserActivity];
    return YES;
  }

  if (volumeDragging) {
    vol = [self transportVolumeTrack: panel];
    fraction = (float) ((where.x - NSMinX (vol)) / NSWidth (vol));
    fraction = (fraction < 0.0f) ? 0.0f : ((fraction > 1.0f) ? 1.0f : fraction);
    osdVolume = fraction;
    [self noteUserActivity];
    g_snprintf (name, sizeof (name), "uxplay-volume:%.4f", fraction);
    [self sendControlKey: name];
    return YES;
  }

  if (scrubbing && [self transportProgressTrack: panel into: &track]) {
    double target;

    fraction = (float) ((where.x - NSMinX (track)) / NSWidth (track));
    fraction = (fraction < 0.0f) ? 0.0f : ((fraction > 1.0f) ? 1.0f : fraction);
    target = fraction * osdDuration;
    /* Move the knob straight away; position reports only catch up a second
       later, and a scrubber that lags the pointer feels broken. */
    osdPosition = target;
    [self noteUserActivity];
    g_snprintf (name, sizeof (name), "uxplay-seek:%.3f", osdPosition);
    [self sendControlKey: name];
    return YES;
  }
  return NO;
}

- (void) sendControlKey: (const char *) name
{
  if (navigation) {
    gst_navigation_send_key_event (navigation, "key-press", name);
  }
}

- (void)mouseDragged:(NSEvent *) event;
{
  NSPoint where = [self convertPoint: [event locationInWindow] fromView: nil];

  [self noteUserActivity];
  if ([self controlsAtPoint: where begin: NO]) {
    return;
  }
  [self sendMouseEvent:event: "mouse-move"];
  [super mouseDragged: event];
}

- (void)mouseUp:(NSEvent *) event;
{
  if (scrubbing || volumeDragging || panelDragging) {
    scrubbing = NO;
    volumeDragging = NO;
    panelDragging = NO;
    [self setNeedsDisplay: YES];
    return;
  }
  [self sendMouseEvent:event: "mouse-button-release"];
  [super mouseUp: event];
}

- (void)mouseMoved:(NSEvent *)event;
{
  [self noteUserActivity];
  [self sendMouseEvent:event: "mouse-move"];
  [super mouseMoved: event];
}

- (void)mouseEntered:(NSEvent *)event;
{
  [super mouseEntered: event];
}

- (void)mouseExited:(NSEvent *)event;
{
  [super mouseExited: event];
}

@end
