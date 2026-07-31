/* GStreamer
 * OSX video sink
 * Copyright (C) 2004-6 Zaheer Abbas Merali <zaheerabbas at merali dot org>
 * Copyright (C) 2007,2008,2009 Pioneers of the Inevitable <songbird@songbirdnest.com>
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
 * The development of this code was made possible due to the involvement of
 * Pioneers of the Inevitable, the creators of the Songbird Music player.
 *
 */

/**
 * SECTION:element-osxvideosink
 *
 * The OSXVideoSink renders video frames to a MacOSX window. The video output
 * must be directed to a window embedded in an existing NSApp.
 *
 */

/* Built into UxPlay rather than as a standalone plugin, so the values
   GST_PLUGIN_DEFINE wants are supplied here instead of by a config.h. */
#define PACKAGE            "uxplay"
#define VERSION            "1.74"
#define GST_LICENSE        "LGPL"
#define GST_PACKAGE_NAME   "UxPlay"
#define GST_PACKAGE_ORIGIN "https://github.com/FDH2/UxPlay"
#include <gst/video/videooverlay.h>
#include <gst/video/navigation.h>
#include <gst/video/video.h>

#include "osxvideosink.h"
#include <unistd.h>
#include <signal.h>
#import "cocoawindow.h"

GST_DEBUG_CATEGORY (gst_debug_osx_video_sink);
#define GST_CAT_DEFAULT gst_debug_osx_video_sink

static GstStaticPadTemplate gst_osx_video_sink_sink_template_factory =
GST_STATIC_PAD_TEMPLATE ("sink",
    GST_PAD_SINK,
    GST_PAD_ALWAYS,
    GST_STATIC_CAPS ("video/x-raw, "
        "framerate = (fraction) [ 0, MAX ], "
        "width = (int) [ 1, MAX ], "
        "height = (int) [ 1, MAX ], "
#if G_BYTE_ORDER == G_BIG_ENDIAN
       "format = (string) YUY2")
#else
        "format = (string) UYVY")
#endif
    );

enum
{
  ARG_0,
  ARG_EMBED,
  ARG_FORCE_PAR,
  ARG_FILL_MODE,
  ARG_START_FULLSCREEN,
  ARG_HIDE_UNTIL_STREAM,
  ARG_STATUS_ITEM,
  ARG_VOLUME_OSD,
  ARG_STREAM_ACTIVE,
  ARG_PLAYBACK_POSITION,
  ARG_PLAYBACK_DURATION,
  ARG_PLAYBACK_RATE,
  ARG_DISPLAY_INDEX,
};

static void gst_osx_video_sink_osxwindow_destroy (GstOSXVideoSink * osxvideosink);

static GstOSXVideoSinkClass *sink_class = NULL;
static GstVideoSinkClass *parent_class = NULL;

#if MAC_OS_X_VERSION_MAX_ALLOWED < 101200
#define NSEventMaskAny                       NSAnyEventMask
#define NSWindowStyleMaskTitled              NSTitledWindowMask
#define NSWindowStyleMaskClosable            NSClosableWindowMask
#define NSWindowStyleMaskResizable           NSResizableWindowMask
#define NSWindowStyleMaskTexturedBackground  NSTexturedBackgroundWindowMask
#define NSWindowStyleMaskMiniaturizable      NSMiniaturizableWindowMask
#endif

/* Helper to trigger calls from the main thread */
static void
gst_osx_video_sink_call_from_main_thread(GstOSXVideoSink *osxvideosink,
    NSObject * object, SEL function, NSObject *data, BOOL waitUntilDone)
{
  NSThread *thread;
  NSAutoreleasePool *pool = [[NSAutoreleasePool alloc] init];

  if (sink_class->ns_app_thread == NULL){
    thread = [NSThread mainThread];
  } else {
    thread = sink_class->ns_app_thread;
  }

  [object performSelector:function onThread:thread
          withObject:data waitUntilDone:waitUntilDone];
  [pool release];
}

/* This function handles osx window creation */
static gboolean
gst_osx_video_sink_osxwindow_create (GstOSXVideoSink * osxvideosink, gint width,
    gint height)
{
  NSRect rect;
  GstOSXWindow *osxwindow = NULL;
  gboolean res = TRUE;
  NSAutoreleasePool *pool = [[NSAutoreleasePool alloc] init];

  g_return_val_if_fail (GST_IS_OSX_VIDEO_SINK (osxvideosink), FALSE);

  GST_DEBUG_OBJECT (osxvideosink, "Creating new OSX window");

  osxvideosink->osxwindow = osxwindow = g_new0 (GstOSXWindow, 1);

  osxwindow->width = width;
  osxwindow->height = height;
  osxwindow->closed = FALSE;
  osxwindow->internal = FALSE;

  /* Allocate our GstGLView for the window, and then tell the application
   * about it (hopefully it's listening...) */
  rect.origin.x = 0.0;
  rect.origin.y = 0.0;
  rect.size.width = (float) osxwindow->width;
  rect.size.height = (float) osxwindow->height;
  osxwindow->gstview =[[GstGLView alloc] initWithFrame:rect];

  if (osxvideosink->superview == NULL) {
    GST_INFO_OBJECT (osxvideosink, "emitting prepare-xwindow-id");
    gst_video_overlay_prepare_window_handle (GST_VIDEO_OVERLAY (osxvideosink));
  }

  if (osxvideosink->superview != NULL) {
    /* prepare-xwindow-id was handled, we have the superview in
     * osxvideosink->superview. We now add osxwindow->gstview to the superview
     * from the main thread
     */
    GST_INFO_OBJECT (osxvideosink, "we have a superview, adding our view to it");
    gst_osx_video_sink_call_from_main_thread (osxvideosink, osxwindow->gstview,
        @selector(addToSuperview:), osxvideosink->superview, NO);

  } else {
    gst_osx_video_sink_call_from_main_thread (osxvideosink,
      osxvideosink->osxvideosinkobject,
      @selector(createInternalWindow), nil, YES);
    GST_INFO_OBJECT (osxvideosink, "No superview, creating an internal window.");
  }

  gst_osx_video_sink_call_from_main_thread (osxvideosink, osxvideosink->osxvideosinkobject,
    @selector(setActivationPolicy), nil, YES);

  [osxwindow->gstview setNavigation: GST_NAVIGATION(osxvideosink)];
  [osxvideosink->osxwindow->gstview setKeepAspectRatio: osxvideosink->keep_par];
  [osxvideosink->osxwindow->gstview setFillMode: osxvideosink->fill_mode];

  gst_osx_video_sink_call_from_main_thread (osxvideosink,
      osxvideosink->osxvideosinkobject, @selector(createStatusItem), (id) nil,
      YES);

  [pool release];

  return res;
}

static void
gst_osx_video_sink_osxwindow_destroy (GstOSXVideoSink * osxvideosink)
{
  NSAutoreleasePool *pool;

  g_return_if_fail (GST_IS_OSX_VIDEO_SINK (osxvideosink));
  pool = [[NSAutoreleasePool alloc] init];

  gst_osx_video_sink_call_from_main_thread (osxvideosink,
      osxvideosink->osxvideosinkobject,
      @selector(removeStatusItem), (id) nil, YES);
  gst_osx_video_sink_call_from_main_thread (osxvideosink,
      osxvideosink->osxvideosinkobject,
      @selector(destroy), (id) nil, YES);
  [pool release];
}

/* This function resizes a GstXWindow */
static void
gst_osx_video_sink_osxwindow_resize (GstOSXVideoSink * osxvideosink,
    GstOSXWindow * osxwindow, guint width, guint height)
{
  GstOSXVideoSinkObject *object = osxvideosink->osxvideosinkobject;

  NSAutoreleasePool *pool = [[NSAutoreleasePool alloc] init];
  g_return_if_fail (osxwindow != NULL);
  g_return_if_fail (GST_IS_OSX_VIDEO_SINK (osxvideosink));

  osxwindow->width = width;
  osxwindow->height = height;

  GST_DEBUG_OBJECT (osxvideosink, "Resizing window to (%d,%d)", width, height);

  /* Directly resize the underlying view. Dispatched without waiting: the main
     thread is not always free -- an open menu holds it in modal event tracking
     -- and blocking the streaming thread on it stalls the whole pipeline long
     enough for the client to give up on us. Waiting used to be necessary so a
     frame could not be drawn against a stale texture, but showFrame: now bounds
     itself by the texture's real size, so arriving early is harmless. */
  GST_DEBUG_OBJECT (osxvideosink, "Calling setVideoSize on %p", osxwindow->gstview);
  gst_osx_video_sink_call_from_main_thread (osxvideosink, object,
      @selector(resize), (id)nil, NO);

  [pool release];
}

static gboolean
gst_osx_video_sink_setcaps (GstBaseSink * bsink, GstCaps * caps)
{
  GstOSXVideoSink *osxvideosink;
  GstStructure *structure;
  gboolean res, result = FALSE;
  gint video_width, video_height;

  osxvideosink = GST_OSX_VIDEO_SINK (bsink);

  GST_DEBUG_OBJECT (osxvideosink, "caps: %" GST_PTR_FORMAT, caps);

  structure = gst_caps_get_structure (caps, 0);
  res = gst_structure_get_int (structure, "width", &video_width);
  res &= gst_structure_get_int (structure, "height", &video_height);

  if (!res) {
    goto beach;
  }

  GST_DEBUG_OBJECT (osxvideosink, "our format is: %dx%d video",
      video_width, video_height);

  GST_VIDEO_SINK_WIDTH (osxvideosink) = video_width;
  GST_VIDEO_SINK_HEIGHT (osxvideosink) = video_height;

  gst_osx_video_sink_osxwindow_resize (osxvideosink, osxvideosink->osxwindow,
      video_width, video_height);

  gst_video_info_from_caps (&osxvideosink->info, caps);

  /* First caps of a stream: this, not window creation, is when a client has
     actually started sending video. */
  if (!osxvideosink->stream_started) {
    osxvideosink->stream_started = TRUE;
    gst_osx_video_sink_call_from_main_thread (osxvideosink,
        osxvideosink->osxvideosinkobject, @selector(showStream), (id) nil, NO);
  }

  result = TRUE;

beach:
  return result;

}

static GstStateChangeReturn
gst_osx_video_sink_change_state (GstElement * element,
    GstStateChange transition)
{
  GstOSXVideoSink *osxvideosink;
  GstStateChangeReturn ret;

  osxvideosink = GST_OSX_VIDEO_SINK (element);

  GST_DEBUG_OBJECT (osxvideosink, "%s => %s",
        gst_element_state_get_name(GST_STATE_TRANSITION_CURRENT (transition)),
        gst_element_state_get_name(GST_STATE_TRANSITION_NEXT (transition)));

  switch (transition) {
    case GST_STATE_CHANGE_NULL_TO_READY:
      break;
    case GST_STATE_CHANGE_READY_TO_PAUSED:
      /* Creating our window and our image */
      GST_VIDEO_SINK_WIDTH (osxvideosink) = 320;
      GST_VIDEO_SINK_HEIGHT (osxvideosink) = 240;
      if (!gst_osx_video_sink_osxwindow_create (osxvideosink,
          GST_VIDEO_SINK_WIDTH (osxvideosink),
          GST_VIDEO_SINK_HEIGHT (osxvideosink))) {
        ret = GST_STATE_CHANGE_FAILURE;
        goto done;
      }
      break;
    default:
      break;
  }

  ret = (GST_ELEMENT_CLASS (parent_class))->change_state (element, transition);

  switch (transition) {
    case GST_STATE_CHANGE_PAUSED_TO_READY:
      if (osxvideosink->stream_started) {
        osxvideosink->stream_started = FALSE;
        gst_osx_video_sink_call_from_main_thread (osxvideosink,
            osxvideosink->osxvideosinkobject, @selector(hideStream), (id) nil,
            YES);
      }
      GST_VIDEO_SINK_WIDTH (osxvideosink) = 0;
      GST_VIDEO_SINK_HEIGHT (osxvideosink) = 0;
      gst_osx_video_sink_osxwindow_destroy (osxvideosink);
      break;
    case GST_STATE_CHANGE_READY_TO_NULL:
      break;
    default:
      break;
  }

done:
  return ret;
}

static GstFlowReturn
gst_osx_video_sink_show_frame (GstBaseSink * bsink, GstBuffer * buf)
{
  GstOSXVideoSink *osxvideosink;
  GstBufferObject* bufferobject;
  NSAutoreleasePool *pool = [[NSAutoreleasePool alloc] init];

  osxvideosink = GST_OSX_VIDEO_SINK (bsink);

  GST_DEBUG ("show_frame");
  bufferobject = [[GstBufferObject alloc] initWithBuffer:buf];
  /* Dispatched asynchronously, so a frame can still be queued on the main
   * thread when the caps change underneath it; showFrame: clamps to the
   * intersection of the frame and sink geometry so that cannot overrun.
   * Waiting here would close that window too, but it stalls the streaming
   * thread on every frame and audio then drifts out of sync with the video. */
  gst_osx_video_sink_call_from_main_thread (osxvideosink,
      osxvideosink->osxvideosinkobject,
      @selector(showFrame:), bufferobject, NO);
  [pool release];
  return GST_FLOW_OK;
}

/* Buffer management */



/* =========================================== */
/*                                             */
/*              Init & Class init              */
/*                                             */
/* =========================================== */

static void
gst_osx_video_sink_set_property (GObject * object, guint prop_id,
    const GValue * value, GParamSpec * pspec)
{
  GstOSXVideoSink *osxvideosink;

  g_return_if_fail (GST_IS_OSX_VIDEO_SINK (object));

  osxvideosink = GST_OSX_VIDEO_SINK (object);

  switch (prop_id) {
    case ARG_EMBED:
      g_warning ("The \"embed\" property of osxvideosink is deprecated and "
          "has no effect anymore. Use the GstVideoOverlay "
          "instead.");
      break;
    case ARG_FORCE_PAR:
      osxvideosink->keep_par = g_value_get_boolean(value);
      if (osxvideosink->osxwindow)
        [osxvideosink->osxwindow->gstview
            setKeepAspectRatio: osxvideosink->keep_par];
      break;
    case ARG_FILL_MODE:
      osxvideosink->fill_mode = g_value_get_int(value);
      if (osxvideosink->osxwindow)
        [osxvideosink->osxwindow->gstview
            setFillMode: osxvideosink->fill_mode];
      break;
    case ARG_START_FULLSCREEN:
      osxvideosink->start_fullscreen = g_value_get_boolean(value);
      break;
    case ARG_HIDE_UNTIL_STREAM:
      osxvideosink->hide_until_stream = g_value_get_boolean(value);
      break;
    case ARG_STATUS_ITEM:
      osxvideosink->status_item = g_value_get_boolean(value);
      break;
    case ARG_STREAM_ACTIVE:
      /* The pipeline is not always torn down when a client goes away, so the
         end of a session has to be told to us rather than inferred. */
      if (!g_value_get_boolean(value)) {
        if (osxvideosink->stream_started) {
          osxvideosink->stream_started = FALSE;
          gst_osx_video_sink_call_from_main_thread (osxvideosink,
              osxvideosink->osxvideosinkobject, @selector(hideStream),
              (id) nil, NO);
        }
      }
      break;
    case ARG_PLAYBACK_POSITION:
      osxvideosink->playback_position = g_value_get_double (value);
      if (osxvideosink->osxwindow && osxvideosink->osxwindow->gstview) {
        [osxvideosink->osxwindow->gstview
            performSelectorOnMainThread: @selector(setPlaybackPosition:)
                             withObject: [NSNumber numberWithDouble:
                                 osxvideosink->playback_position]
                          waitUntilDone: NO];
      }
      break;
    case ARG_PLAYBACK_DURATION:
      osxvideosink->playback_duration = g_value_get_double (value);
      if (osxvideosink->osxwindow && osxvideosink->osxwindow->gstview) {
        [osxvideosink->osxwindow->gstview
            performSelectorOnMainThread: @selector(setPlaybackDuration:)
                             withObject: [NSNumber numberWithDouble:
                                 osxvideosink->playback_duration]
                          waitUntilDone: NO];
      }
      break;
    case ARG_DISPLAY_INDEX:
      osxvideosink->display_index = g_value_get_int (value);
      if (osxvideosink->osxwindow && osxvideosink->osxwindow->gstview) {
        [osxvideosink->osxwindow->gstview
            performSelectorOnMainThread: @selector(setDisplayIndex:)
                             withObject: [NSNumber numberWithInt:
                                 osxvideosink->display_index]
                          waitUntilDone: NO];
      }
      break;
    case ARG_PLAYBACK_RATE:
      osxvideosink->playback_rate = g_value_get_double (value);
      if (osxvideosink->osxwindow && osxvideosink->osxwindow->gstview) {
        [osxvideosink->osxwindow->gstview
            performSelectorOnMainThread: @selector(setPlaybackRate:)
                             withObject: [NSNumber numberWithDouble:
                                 osxvideosink->playback_rate]
                          waitUntilDone: NO];
      }
      break;
    case ARG_VOLUME_OSD:
      osxvideosink->volume_osd = g_value_get_double(value);
      if (osxvideosink->osxwindow && osxvideosink->osxwindow->gstview) {
        [osxvideosink->osxwindow->gstview
            performSelectorOnMainThread: @selector(showVolumeOSDNumber:)
                             withObject: [NSNumber numberWithDouble:
                                 osxvideosink->volume_osd]
                          waitUntilDone: NO];
      }
      break;
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      break;
  }
}

static void
gst_osx_video_sink_get_property (GObject * object, guint prop_id,
    GValue * value, GParamSpec * pspec)
{
  GstOSXVideoSink *osxvideosink;

  g_return_if_fail (GST_IS_OSX_VIDEO_SINK (object));

  osxvideosink = GST_OSX_VIDEO_SINK (object);

  switch (prop_id) {
    case ARG_EMBED:
      g_value_set_boolean (value, FALSE);
      break;
    case ARG_FORCE_PAR:
      g_value_set_boolean (value, osxvideosink->keep_par);
      break;
    case ARG_FILL_MODE:
      g_value_set_int (value, osxvideosink->fill_mode);
      break;
    case ARG_START_FULLSCREEN:
      g_value_set_boolean (value, osxvideosink->start_fullscreen);
      break;
    case ARG_HIDE_UNTIL_STREAM:
      g_value_set_boolean (value, osxvideosink->hide_until_stream);
      break;
    case ARG_STATUS_ITEM:
      g_value_set_boolean (value, osxvideosink->status_item);
      break;
    case ARG_VOLUME_OSD:
      g_value_set_double (value, osxvideosink->volume_osd);
      break;
    case ARG_STREAM_ACTIVE:
      g_value_set_boolean (value, osxvideosink->stream_started);
      break;
    case ARG_PLAYBACK_POSITION:
      g_value_set_double (value, osxvideosink->playback_position);
      break;
    case ARG_PLAYBACK_DURATION:
      g_value_set_double (value, osxvideosink->playback_duration);
      break;
    case ARG_DISPLAY_INDEX:
      g_value_set_int (value, osxvideosink->display_index);
      break;
    case ARG_PLAYBACK_RATE:
      g_value_set_double (value, osxvideosink->playback_rate);
      break;
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      break;
  }
}

static gboolean
gst_osx_video_sink_propose_allocation (GstBaseSink * base_sink, GstQuery * query)
{
    gst_query_add_allocation_meta (query,
        GST_VIDEO_META_API_TYPE, NULL);

    return TRUE;
}

static void
gst_osx_video_sink_init (GstOSXVideoSink * sink)
{
  if ([[NSRunLoop mainRunLoop] currentMode] == nil)
    g_warning ("An NSRunLoop needs to be running on the main thread "
        "to ensure correct behaviour on macOS. Use gst_macos_main() or call "
        "[NSApplication sharedApplication] in your code before using this element.");

  sink->osxwindow = NULL;
  sink->superview = NULL;
  sink->osxvideosinkobject = [[GstOSXVideoSinkObject alloc] initWithSink:sink];
  sink->keep_par = FALSE;
  sink->fill_mode = GST_OSX_FILL_FIT;
  sink->start_fullscreen = FALSE;
  sink->hide_until_stream = FALSE;
  sink->status_item = FALSE;
  sink->volume_osd = 1.0;
  sink->stream_started = FALSE;
  sink->hide_when_windowed = FALSE;
}

static void
gst_osx_video_sink_base_init (gpointer g_class)
{
  GstElementClass *element_class = GST_ELEMENT_CLASS (g_class);

  gst_element_class_set_static_metadata (element_class, "macOS Video sink",
      "Sink/Video", "macOS native videosink",
      "Zaheer Abbas Merali <zaheerabbas at merali dot org>");

  gst_element_class_add_static_pad_template (element_class, &gst_osx_video_sink_sink_template_factory);
}

static void
gst_osx_video_sink_finalize (GObject *object)
{
  GstOSXVideoSink *osxvideosink = GST_OSX_VIDEO_SINK (object);

  if (osxvideosink->superview)
    [osxvideosink->superview release];

  if (osxvideosink->osxvideosinkobject)
    [(GstOSXVideoSinkObject*)(osxvideosink->osxvideosinkobject) release];

  G_OBJECT_CLASS (parent_class)->finalize (object);
}

static void
gst_osx_video_sink_class_init (GstOSXVideoSinkClass * klass)
{
  GObjectClass *gobject_class;
  GstElementClass *gstelement_class;
  GstBaseSinkClass *gstbasesink_class;

  gobject_class = (GObjectClass *) klass;
  gstelement_class = (GstElementClass *) klass;
  gstbasesink_class = (GstBaseSinkClass *) klass;

  parent_class = g_type_class_ref (GST_TYPE_VIDEO_SINK);
  sink_class = klass;

  klass->run_loop_state = GST_OSX_VIDEO_SINK_RUN_LOOP_STATE_UNKNOWN;
  klass->ns_app_thread = NULL;

  gobject_class->set_property = gst_osx_video_sink_set_property;
  gobject_class->get_property = gst_osx_video_sink_get_property;
  gobject_class->finalize = gst_osx_video_sink_finalize;

  gstbasesink_class->set_caps = gst_osx_video_sink_setcaps;
  gstbasesink_class->preroll = gst_osx_video_sink_show_frame;
  gstbasesink_class->render = gst_osx_video_sink_show_frame;
  gstbasesink_class->propose_allocation = gst_osx_video_sink_propose_allocation;
  gstelement_class->change_state = gst_osx_video_sink_change_state;

  g_object_class_install_property (gobject_class, ARG_EMBED,
      g_param_spec_boolean ("embed", "embed", "For ABI compatibility only, do not use",
          FALSE, G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));

  g_object_class_install_property (gobject_class, ARG_FORCE_PAR,
      g_param_spec_boolean ("force-aspect-ratio", "force aspect ration",
          "When enabled, scaling will respect original aspect ration",
          TRUE, G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));

  g_object_class_install_property (gobject_class, ARG_FILL_MODE,
      g_param_spec_int ("fill-mode", "fill mode",
          "How the frame is mapped onto the view, preserving aspect ratio in "
          "every case: 0 = fit the whole frame inside and leave bars, "
          "1 = fill the height and crop left/right, "
          "2 = fill the width and crop top/bottom. In the video window, Enter "
          "or a double click switches between 1 and 2, Escape returns to 0",
          GST_OSX_FILL_FIT, GST_OSX_FILL_WIDTH, GST_OSX_FILL_FIT,
          G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));

  g_object_class_install_property (gobject_class, ARG_START_FULLSCREEN,
      g_param_spec_boolean ("start-fullscreen", "start fullscreen",
          "Go fullscreen with the height pinned, as if Enter had been pressed, "
          "once a stream actually starts. Escape returns it to a window",
          FALSE, G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));

  g_object_class_install_property (gobject_class, ARG_HIDE_UNTIL_STREAM,
      g_param_spec_boolean ("hide-until-stream", "hide until stream",
          "Keep the video window off screen until a stream starts, so nothing "
          "is shown while waiting for a client to connect",
          FALSE, G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));

  g_object_class_install_property (gobject_class, ARG_STATUS_ITEM,
      g_param_spec_boolean ("status-item", "status item",
          "Show a menu bar item reporting whether a client is streaming or "
          "the server is still waiting for one. Useful when the video window "
          "is hidden while idle",
          FALSE, G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));

  g_object_class_install_property (gobject_class, ARG_VOLUME_OSD,
      g_param_spec_double ("volume-osd", "volume osd",
          "Setting this flashes a volume readout over the video, the way the "
          "system does for its own volume keys. The value is the level to "
          "show, 0.0 - 1.0; writing it is what triggers the display",
          0.0, 1.0, 1.0, G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));

  g_object_class_install_property (gobject_class, ARG_PLAYBACK_POSITION,
      g_param_spec_double ("playback-position", "playback position",
          "Seconds into the stream, shown on the transport bar",
          0.0, G_MAXDOUBLE, 0.0, G_PARAM_READWRITE));

  g_object_class_install_property (gobject_class, ARG_PLAYBACK_RATE,
      g_param_spec_double ("playback-rate", "playback rate",
          "Rate the stream is playing at; 0 draws the play button",
          0.0, G_MAXDOUBLE, 0.0, G_PARAM_READWRITE));

  g_object_class_install_property (gobject_class, ARG_PLAYBACK_DURATION,
      g_param_spec_double ("playback-duration", "playback duration",
          "Length of the stream in seconds; 0 hides the transport bar",
          0.0, G_MAXDOUBLE, 0.0, G_PARAM_READWRITE));

  g_object_class_install_property (gobject_class, ARG_STREAM_ACTIVE,
      g_param_spec_boolean ("stream-active", "stream active",
          "Set FALSE when a session ends to drop out of fullscreen and hide "
          "the window. Needed because the pipeline is not always torn down "
          "when the client goes away",
          TRUE, G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));

  g_object_class_install_property (gobject_class, ARG_DISPLAY_INDEX,
      g_param_spec_int ("display-index", "display index",
          "Which screen the window sits on, as an index into the displays "
          "the system reports. -1 leaves it wherever it is",
          -1, 64, -1, G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));
}

static void
gst_osx_video_sink_navigation_send_event (GstNavigation * navigation,
    GstStructure * structure)
{
  GstOSXVideoSink *osxvideosink = GST_OSX_VIDEO_SINK (navigation);
  GstEvent *event;
  GstVideoRectangle src = { 0, };
  GstVideoRectangle dst = { 0, };
  GstVideoRectangle result;
  NSRect bounds;
  gdouble x, y, xscale = 1.0, yscale = 1.0;

  if (!osxvideosink->osxwindow)
    return;

  event = gst_event_new_navigation (structure);

  bounds = [osxvideosink->osxwindow->gstview getDrawingBounds];

  if (osxvideosink->keep_par) {
    /* We get the frame position using the calculated geometry from _setcaps
       that respect pixel aspect ratios */
    src.w = GST_VIDEO_SINK_WIDTH (osxvideosink);
    src.h = GST_VIDEO_SINK_HEIGHT (osxvideosink);
    dst.w = bounds.size.width;
    dst.h = bounds.size.height;

    gst_video_sink_center_rect (src, dst, &result, TRUE);
    result.x += bounds.origin.x;
    result.y += bounds.origin.y;
  } else {
    result.x = bounds.origin.x;
    result.y = bounds.origin.y;
    result.w = bounds.size.width;
    result.h = bounds.size.height;
  }

  /* We calculate scaling using the original video frames geometry to include
     pixel aspect ratio scaling. */
  xscale = (gdouble) osxvideosink->osxwindow->width / result.w;
  yscale = (gdouble) osxvideosink->osxwindow->height / result.h;

  /* Converting pointer coordinates to the non scaled geometry */
  if (gst_structure_get_double (structure, "pointer_x", &x)) {
    x = MIN (x, result.x + result.w);
    x = MAX (x - result.x, 0);
    gst_structure_set (structure, "pointer_x", G_TYPE_DOUBLE,
        (gdouble) x * xscale, NULL);
  }
  if (gst_structure_get_double (structure, "pointer_y", &y)) {
    y = MIN (y, result.y + result.h);
    y = MAX (y - result.y, 0);
    gst_structure_set (structure, "pointer_y", G_TYPE_DOUBLE,
        (gdouble) y * yscale, NULL);
  }

  gst_event_ref (event);
  if (!gst_pad_push_event (GST_VIDEO_SINK_PAD (osxvideosink), event)) {
    /* If the event was not handled/used upstream,
     * we post it as a message on the bus so that applications can handle it */
    gst_element_post_message (GST_ELEMENT_CAST (osxvideosink),
        gst_navigation_message_new_event (GST_OBJECT_CAST (osxvideosink), event));
  }
  gst_event_unref (event);
}

static void
gst_osx_video_sink_navigation_init (GstNavigationInterface * iface)
{
  iface->send_event = gst_osx_video_sink_navigation_send_event;
}

static void
gst_osx_video_sink_set_window_handle (GstVideoOverlay * overlay, guintptr handle_id)
{
  GstOSXVideoSink *osxvideosink = GST_OSX_VIDEO_SINK (overlay);
  NSView *view = (NSView *) handle_id;

  gst_osx_video_sink_call_from_main_thread (osxvideosink,
      osxvideosink->osxvideosinkobject,
      @selector(setView:), view, YES);
}

static void
gst_osx_video_sink_xoverlay_init (GstVideoOverlayInterface * iface)
{
  iface->set_window_handle = gst_osx_video_sink_set_window_handle;
  iface->expose = NULL;
  iface->handle_events = NULL;
}

/* ============================================================= */
/*                                                               */
/*                       Public Methods                          */
/*                                                               */
/* ============================================================= */

/* =========================================== */
/*                                             */
/*          Object typing & Creation           */
/*                                             */
/* =========================================== */

GType
gst_osx_video_sink_get_type (void)
{
  static GType osxvideosink_type = 0;

  if (!osxvideosink_type) {
    static const GTypeInfo osxvideosink_info = {
      sizeof (GstOSXVideoSinkClass),
      gst_osx_video_sink_base_init,
      NULL,
      (GClassInitFunc) gst_osx_video_sink_class_init,
      NULL,
      NULL,
      sizeof (GstOSXVideoSink),
      0,
      (GInstanceInitFunc) gst_osx_video_sink_init,
    };

    static const GInterfaceInfo overlay_info = {
      (GInterfaceInitFunc) gst_osx_video_sink_xoverlay_init,
      NULL,
      NULL,
    };

    static const GInterfaceInfo navigation_info = {
      (GInterfaceInitFunc) gst_osx_video_sink_navigation_init,
      NULL,
      NULL,
    };
    osxvideosink_type = g_type_register_static (GST_TYPE_VIDEO_SINK,
        "GstOSXVideoSink", &osxvideosink_info, 0);

    g_type_add_interface_static (osxvideosink_type, GST_TYPE_VIDEO_OVERLAY,
        &overlay_info);
    g_type_add_interface_static (osxvideosink_type, GST_TYPE_NAVIGATION,
        &navigation_info);
  }

  return osxvideosink_type;
}

@implementation GstWindowDelegate
- (id) initWithSink: (GstOSXVideoSink *) sink
{
  self = [super init];
  self->osxvideosink = sink;
  return self;
}

- (void)windowWillClose:(NSNotification *)notification {
  /* Only handle close events if the window was closed manually by the user
   * and not because of a state change state to READY */
  if (osxvideosink->osxwindow == NULL) {
    return;
  }
  if (!osxvideosink->osxwindow->closed) {
    osxvideosink->osxwindow->closed = TRUE;
    GST_ELEMENT_ERROR (osxvideosink, RESOURCE, NOT_FOUND, ("Output window was closed"), (NULL));
    gst_osx_video_sink_osxwindow_destroy (osxvideosink);
  }
}

@end

@ implementation GstOSXVideoSinkObject

-(id) initWithSink: (GstOSXVideoSink*) sink
{
  self = [super init];
  self->osxvideosink = gst_object_ref (sink);
  return self;
}

-(void) dealloc {
  gst_object_unref (osxvideosink);
  [super dealloc];
}

-(void) createInternalWindow
{
  GstOSXWindow *osxwindow = osxvideosink->osxwindow;
  NSRect rect;
  unsigned int mask;

  [NSApplication sharedApplication];

  osxwindow->internal = TRUE;

  mask =  NSWindowStyleMaskTitled             |
          NSWindowStyleMaskClosable           |
          NSWindowStyleMaskResizable          |
          NSWindowStyleMaskTexturedBackground |
          NSWindowStyleMaskMiniaturizable;

  rect.origin.x = 100.0;
  rect.origin.y = 100.0;
  rect.size.width = (float) osxwindow->width;
  rect.size.height = (float) osxwindow->height;

  osxwindow->win =[[[GstOSXVideoSinkWindow alloc]
                       initWithContentNSRect: rect
                       styleMask: mask
                       backing: NSBackingStoreBuffered
                       defer: NO
                       screen: nil] retain];
  GST_DEBUG("VideoSinkWindow created, %p", osxwindow->win);
  /* Needed for -[NSWindow toggleFullScreen:], which the view binds to Enter */
  [osxwindow->win setCollectionBehavior:
      NSWindowCollectionBehaviorFullScreenPrimary];
  if (!osxvideosink->hide_until_stream)
    [osxwindow->win orderFrontRegardless];
  osxwindow->gstview =[osxwindow->win gstView];
  [osxwindow->win setDelegate:[[GstWindowDelegate alloc]
      initWithSink:osxvideosink]];


}

- (void) setActivationPolicy
{
  [NSApp setActivationPolicy: NSApplicationActivationPolicyRegular];
}

- (void) setView: (NSView*)view
{
  NSAutoreleasePool *pool = [[NSAutoreleasePool alloc] init];

  if (osxvideosink->superview) {
    GST_INFO_OBJECT (osxvideosink, "old xwindow id %p", osxvideosink->superview);
    if (osxvideosink->osxwindow) {
      [osxvideosink->osxwindow->gstview removeFromSuperview];
    }
    [osxvideosink->superview release];
  }
  if (osxvideosink->osxwindow != NULL && view != NULL) {
    if (osxvideosink->osxwindow->internal) {
      GST_INFO_OBJECT (osxvideosink, "closing internal window");
      osxvideosink->osxwindow->closed = TRUE;
      [osxvideosink->osxwindow->win close];
      [osxvideosink->osxwindow->win release];
    }
  }

  GST_INFO_OBJECT (osxvideosink, "set xwindow id %p", view);
  osxvideosink->superview = [view retain];
  if (osxvideosink->osxwindow) {
    [osxvideosink->osxwindow->gstview addToSuperview: osxvideosink->superview];
    if (view) {
      osxvideosink->osxwindow->internal = FALSE;
    }
  }

  [pool release];
}

- (void) resize
{
  GstOSXWindow *osxwindow = osxvideosink->osxwindow;

  NSAutoreleasePool *pool = [[NSAutoreleasePool alloc] init];

  GST_INFO_OBJECT (osxvideosink, "resizing");
  NSSize size = {osxwindow->width, osxwindow->height};
  /* Resizing the window to the frame would undo it filling the screen, so when
     the client rotates while filling only the view is told the new size — it
     keeps its frame and reshape re-fits the new aspect. */
  if (osxwindow->internal && ![osxwindow->gstview isInNativeFullScreen]) {
    [osxwindow->win setContentSize:size];
  }
  if (osxwindow->gstview) {
      [osxwindow->gstview setVideoSize :(int)osxwindow->width :(int)osxwindow->height];
  }
  GST_INFO_OBJECT (osxvideosink, "done");

  [pool release];
}

/* A menu bar item is the only thing left showing the server is alive once the
   video window is hidden while idle, so it reports which of the two states we
   are in. */
- (void) createStatusItem
{
  NSAutoreleasePool *pool = [[NSAutoreleasePool alloc] init];

  if (statusItem == nil && osxvideosink->status_item) {
    NSMenu *menu = [[[NSMenu alloc] initWithTitle:@"UxPlay"] autorelease];

    statusItem = [[[NSStatusBar systemStatusBar]
        statusItemWithLength:NSSquareStatusItemLength] retain];

    statusMenuItem = [menu addItemWithTitle:@"" action:nil keyEquivalent:@""];
    [statusMenuItem setEnabled:NO];
    [menu addItem:[NSMenuItem separatorItem]];
    [[menu addItemWithTitle:@"Quit UxPlay"
                     action:@selector(statusMenuQuit:)
              keyEquivalent:@"q"] setTarget:self];
    [statusItem setMenu:menu];

    [self updateStatusItem];
  }
  [pool release];
}

- (void) updateStatusItem
{
  NSAutoreleasePool *pool = [[NSAutoreleasePool alloc] init];

  if (statusItem != nil) {
    BOOL streaming = osxvideosink->stream_started;
    NSStatusBarButton *button = [statusItem button];
    NSImage *image = nil;

    /* One glyph for both states, so only the colour changes: the menu bar's own
       colour while idle, blue once a client is streaming. Sizing it from the
       bar's thickness is what keeps it vertically centred — an unconfigured
       symbol comes out at its own metrics and sits high. */
    if ([NSImage respondsToSelector:
            @selector(imageWithSystemSymbolName:accessibilityDescription:)]) {
      image = [NSImage imageWithSystemSymbolName:@"airplayvideo"
                        accessibilityDescription:@"UxPlay"];
      if (image != nil) {
        CGFloat points = [[NSStatusBar systemStatusBar] thickness] * 0.62;
        NSImageSymbolConfiguration *config = [NSImageSymbolConfiguration
            configurationWithPointSize:points
                               weight:NSFontWeightRegular];
        NSImage *configured = [image imageWithSymbolConfiguration:config];
        if (configured != nil)
          image = configured;
      }
    }

    if (image != nil) {
      if (streaming) {
        /* A template image is drawn in the menu bar's own colour whatever the
           button's tint says, so the colour has to be burnt into a non-template
           copy instead. */
        NSImage *tinted = [[image copy] autorelease];

        [tinted setTemplate:NO];
        [tinted lockFocus];
        [[NSColor systemBlueColor] set];
        NSRectFillUsingOperation (NSMakeRect (0, 0, [tinted size].width,
                [tinted size].height), NSCompositingOperationSourceAtop);
        [tinted unlockFocus];
        image = tinted;
      } else {
        [image setTemplate:YES];
      }
      [button setImage:image];
      [button setTitle:@""];
      [button setImagePosition:NSImageOnly];
      [button setImageScaling:NSImageScaleProportionallyDown];
      [button setContentTintColor:nil];
    } else {
      [button setTitle: streaming ? @"\U0001F535" : @"\U000026AA"];
    }
    [button setToolTip: streaming ?
        @"UxPlay: client streaming" : @"UxPlay: waiting for a client"];
    [statusMenuItem setTitle: streaming ?
        @"Streaming from a client" : @"Waiting for a client"];
  }
  [pool release];
}

- (void) statusMenuQuit: (id) sender
{
  /* Same path as Ctrl-C, so UxPlay tears the server down cleanly. */
  kill (getpid (), SIGINT);
}

- (void) removeStatusItem
{
  NSAutoreleasePool *pool = [[NSAutoreleasePool alloc] init];

  if (statusItem != nil) {
    [[NSStatusBar systemStatusBar] removeStatusItem:statusItem];
    [statusItem release];
    statusItem = nil;
  }
  [pool release];
}

/* Called once a stream actually starts, so nothing is on screen while UxPlay
   sits waiting for a client to connect. */
- (void) showStream
{
  GstOSXWindow *osxwindow = osxvideosink->osxwindow;
  NSAutoreleasePool *pool = [[NSAutoreleasePool alloc] init];

  if (osxwindow && osxwindow->internal && osxwindow->win) {
    [osxwindow->win orderFrontRegardless];
  }
  if (osxwindow && osxwindow->gstview && osxvideosink->start_fullscreen) {
    [osxwindow->gstview enterFillScreen];
  }
  [self updateStatusItem];
  [pool release];
}

/* The stream ended: drop back out of fullscreen, and go off screen again if we
   were hidden to begin with. */
- (void) hideStream
{
  GstOSXWindow *osxwindow = osxvideosink->osxwindow;
  NSAutoreleasePool *pool = [[NSAutoreleasePool alloc] init];

  if (osxwindow && osxwindow->gstview) {
    [osxwindow->gstview leaveFillScreen];
  }
  if (osxwindow && osxwindow->internal && osxwindow->win) {
    /* A rotation while filling the screen left the window's own size stale. */
    [osxwindow->win setContentSize:
        NSMakeSize (osxwindow->width, osxwindow->height)];
    if (osxvideosink->hide_until_stream) {
      [osxwindow->win orderOut:nil];
    }
  }
  [self updateStatusItem];
  [pool release];
}

- (void) showFrame: (GstBufferObject *) object
{
  GstVideoFrame frame;
  guint8 *readp, *writep;
  gint i, active_width, stride;
  guint8 *texture_buffer;
  NSAutoreleasePool *pool = [[NSAutoreleasePool alloc] init];
  GstBuffer *buf = object->buf;

  GST_OBJECT_LOCK (osxvideosink);
  if (osxvideosink->osxwindow == NULL)
      goto no_window;

  texture_buffer = (guint8 *) [osxvideosink->osxwindow->gstview getTextureBuffer];
  if (G_UNLIKELY (texture_buffer == NULL))
      goto no_texture_buffer;

  if (!gst_video_frame_map (&frame, &osxvideosink->info, buf, GST_MAP_READ))
      goto no_map;

  readp = GST_VIDEO_FRAME_PLANE_DATA (&frame, 0);
  stride = GST_VIDEO_FRAME_PLANE_STRIDE (&frame, 0);
  writep = texture_buffer;
  /* Bound the source by the mapped frame and the destination by the size the
   * texture buffer was actually allocated with. Neither may be taken from the
   * sink's caps: this frame may predate a caps change, and the resize it
   * triggers may not have run on the main thread yet, so the sink can already
   * report dimensions that match neither the frame nor the buffer. */
  {
    gint tex_width = 0, tex_height = 0;
    gint copy_height, dst_stride;

    [osxvideosink->osxwindow->gstview getTextureSize:&tex_width :&tex_height];

    copy_height = MIN (GST_VIDEO_FRAME_HEIGHT (&frame), tex_height);
    dst_stride = tex_width * sizeof (short);
    active_width = MIN (GST_VIDEO_FRAME_WIDTH (&frame), tex_width) *
        sizeof (short);

    for (i = 0; i < copy_height; i++) {
        memcpy (writep, readp, active_width);
        writep += dst_stride;
        readp += stride;
    }
  }
  [osxvideosink->osxwindow->gstview displayTexture];

  gst_video_frame_unmap (&frame);

out:
  GST_OBJECT_UNLOCK (osxvideosink);
  [object release];

  [pool release];
  return;

no_map:
  GST_WARNING_OBJECT (osxvideosink, "couldn't map frame");
  goto out;

no_window:
  GST_WARNING_OBJECT (osxvideosink, "not showing frame since we have no window (!?)");
  goto out;

no_texture_buffer:
  GST_ELEMENT_ERROR (osxvideosink, RESOURCE, WRITE, (NULL),
          ("the texture buffer is NULL"));
  goto out;
}

-(void) destroy
{
  NSAutoreleasePool *pool;
  GstOSXWindow *osxwindow;

  pool = [[NSAutoreleasePool alloc] init];

  GST_OBJECT_LOCK (osxvideosink);
  osxwindow = osxvideosink->osxwindow;
  osxvideosink->osxwindow = NULL;

  if (osxwindow) {
    if (osxvideosink->superview) {
      [osxwindow->gstview removeFromSuperview];
    }
    [osxwindow->gstview release];
    if (osxwindow->internal) {
      if (!osxwindow->closed) {
        osxwindow->closed = TRUE;
        [osxwindow->win close];
        [osxwindow->win release];
      }
    }
    g_free (osxwindow);
  }
  GST_OBJECT_UNLOCK (osxvideosink);

  [pool release];
}

@end

@ implementation GstBufferObject
-(id) initWithBuffer: (GstBuffer*) buffer
{
  self = [super init];
  gst_buffer_ref(buffer);
  self->buf = buffer;
  return self;
}

-(void) dealloc{
  gst_buffer_unref(buf);
  [super dealloc];
}
@end

static gboolean
plugin_init (GstPlugin * plugin)
{
  if (!gst_element_register (plugin, "uxvideosink",
          GST_RANK_MARGINAL, GST_TYPE_OSX_VIDEO_SINK))
    return FALSE;

  GST_DEBUG_CATEGORY_INIT (gst_debug_osx_video_sink, "uxvideosink", 0,
      "uxvideosink element");

  return TRUE;
}

GST_PLUGIN_DEFINE (GST_VERSION_MAJOR,
    GST_VERSION_MINOR,
    uxvideo,
    "UxPlay macOS video output",
    plugin_init, VERSION, GST_LICENSE, GST_PACKAGE_NAME, GST_PACKAGE_ORIGIN)
