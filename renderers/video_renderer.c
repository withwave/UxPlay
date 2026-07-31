/**
 * RPiPlay - An open-source AirPlay mirroring server for Raspberry Pi
 * Copyright (C) 2019 Florian Draschbacher
 * Modified for:
 * UxPlay - An open-source AirPlay mirroring server
 * Copyright (C) 2021-24 F. Duncanh
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software Foundation,
 * Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301  USA
 */

#include <gst/gst.h>
#include <gst/app/gstappsrc.h>
#include "video_renderer.h"

#define SECOND_IN_NSECS 1000000000UL
#define SECOND_IN_MICROSECS 1000000
/* Keys reach us through navigation messages on every platform, not only the
   X11 build, so this cannot live behind X_DISPLAY_FIX. */
#include <gst/video/navigation.h>
#include <gst/video/videooverlay.h>
#include <stdarg.h>
#ifdef X_DISPLAY_FIX
#include "x_display_fix.h"
static bool fullscreen = false;
static bool alt_keypress = false;
static unsigned char X11_search_attempts = 0;
#endif

#ifdef __APPLE__
/* Patched videosink built into uxplay; see renderers/uxvideosink. */
GST_PLUGIN_STATIC_DECLARE(uxvideo);
#endif

static GstClockTime gst_video_pipeline_base_time = GST_CLOCK_TIME_NONE;
static logger_t *logger = NULL;
static unsigned short width, height, width_source, height_source;  /* not currently used */
static bool first_packet = false;
static bool sync = false;
static bool auto_videosink = true;
static bool window_closed = false;
static void (*key_handler)(const char *key) = NULL;
static bool hls_video = false;
/* kbit/s reported to the HLS variant chooser; see where it is applied. */
static guint hls_connection_speed_kbps = 12000;
#ifdef X_DISPLAY_FIX
static bool use_x11 = false;
#endif
static bool logger_debug = false;
static gint64 hls_requested_start_position = 0;
#define HLS_START_SEEK_MAX_ATTEMPTS 12
static gint hls_start_seek_attempts = 0;
static double hls_last_known_position = -1.0;
static float hls_commanded_rate = 1.0f;
static gboolean hls_paused_for_buffering = FALSE;
static gboolean hls_seek_resuming = FALSE;
static gint64 hls_seek_start = 0;
static gint64 hls_seek_end = 0;
static gint64 hls_duration = 0;
static gboolean hls_seek_enabled = FALSE;
static gboolean hls_playing = FALSE;
static gboolean hls_buffer_empty = FALSE;
static gboolean hls_buffer_full = FALSE;
static int type_264 = 0;
static int type_265 = 0;
static int type_hls = 0;
static int type_jpeg = 0;

typedef enum {
  //GST_PLAY_FLAG_VIDEO         = (1 << 0),
  //GST_PLAY_FLAG_AUDIO         = (1 << 1),
  //GST_PLAY_FLAG_TEXT          = (1 << 2),
  //GST_PLAY_FLAG_VIS           = (1 << 3),
  //GST_PLAY_FLAG_SOFT_VOLUME   = (1 << 4),
  //GST_PLAY_FLAG_NATIVE_AUDIO  = (1 << 5),
  //GST_PLAY_FLAG_NATIVE_VIDEO  = (1 << 6),
  GST_PLAY_FLAG_DOWNLOAD      = (1 << 7),
  GST_PLAY_FLAG_BUFFERING     = (1 << 8),
  //GST_PLAY_FLAG_DEINTERLACE   = (1 << 9),
  //GST_PLAY_FLAG_SOFT_COLORBALANCE = (1 << 10),
  //GST_PLAY_FLAG_FORCE_FILTERS = (1 << 11),
  GST_PLAY_FLAG_FORCE_SW_DECODERS = (1 << 12),
} GstPlayFlags;

#define NCODECS  3   /* renderers for h264,h265, and jpeg images */

struct video_renderer_s {
    GstElement *appsrc, *pipeline, *textsrc;
    GstBus *bus;
    const char *codec;
    bool autovideo;
    int id;
    char *uri;
    gboolean eos;
    gint64 duration;
    gint buffering_level;
#ifdef  X_DISPLAY_FIX
    bool use_x11;
    const char * server_name;
    X11_Window_t * gst_window;
#endif
};

static video_renderer_t *renderer = NULL;
/* The http threads read the renderer while the main loop can be replacing it,
   so both sides take this. Recursive: the teardown path crosses it twice. */
static GRecMutex renderer_lock;
static video_renderer_t *renderer_type[NCODECS] = {0};
static int n_renderers = NCODECS;
static char h264[] = "h264";
static char h265[] = "h265";
static char hls[]  = "hls";
static char jpeg[] = "jpeg";

static void append_videoflip (GString *launch, const videoflip_t *flip, const videoflip_t *rot) {
    /* videoflip image transform */
    switch (*flip) {
    case INVERT:
        switch (*rot)  {
        case LEFT:
	    g_string_append(launch, "videoflip video-direction=GST_VIDEO_ORIENTATION_90R ! ");
	    break;
        case RIGHT:
            g_string_append(launch, "videoflip video-direction=GST_VIDEO_ORIENTATION_90L ! ");
            break;
        default:
	    g_string_append(launch, "videoflip video-direction=GST_VIDEO_ORIENTATION_180 ! ");
	    break;
        }
        break;
    case HFLIP:
        switch (*rot) {
        case LEFT:
            g_string_append(launch, "videoflip video-direction=GST_VIDEO_ORIENTATION_UL_LR ! ");
            break;
        case RIGHT:
            g_string_append(launch, "videoflip video-direction=GST_VIDEO_ORIENTATION_UR_LL ! ");
            break;
        default:
            g_string_append(launch, "videoflip video-direction=GST_VIDEO_ORIENTATION_HORIZ ! ");
            break;
        }
        break;
    case VFLIP:
        switch (*rot) {
        case LEFT:
            g_string_append(launch, "videoflip video-direction=GST_VIDEO_ORIENTATION_UR_LL ! ");
            break;
        case RIGHT:
            g_string_append(launch, "videoflip video-direction=GST_VIDEO_ORIENTATION_UL_LR ! ");
            break;
        default:
            g_string_append(launch, "videoflip video-direction=GST_VIDEO_ORIENTATION_VERT ! ");
	  break;
	}
        break;
    default:
        switch (*rot) {
        case LEFT:
            g_string_append(launch, "videoflip video-direction=GST_VIDEO_ORIENTATION_90L ! ");
            break;
        case RIGHT:
            g_string_append(launch, "videoflip video-direction=GST_VIDEO_ORIENTATION_90R ! ");
            break;
        default:
            break;
        }
        break;
    }
}

/* apple uses colorimetry that is detected as  1:3:7:1           * //previously 1:3:5:1 was seen
 * (not recognized by v4l2 plugin in Gstreamer  < 1.20.4)        *
 * See .../gst-libs/gst/video/video-color.h in gst-plugins-base  *
 * range = 1   -> GST_VIDEO_COLOR_RANGE_0_255      ("full RGB")  * 
 * matrix = 3  -> GST_VIDEO_COLOR_MATRIX_BT709                   *
 * transfer = 7 -> GST_VIDEO_TRANSFER_SRGB                       * // previously GST_VIDEO_TRANSFER_BT709
 * primaries = 1 -> GST_VIDEO_COLOR_PRIMARIES_BT709              *
 * closest used by  GStreamer < 1.20.4 is BT709, 2:3:5:1 with    * // now use sRGB = 1:1:7:1    
 * range = 2 -> GST_VIDEO_COLOR_RANGE_16_235 ("limited RGB")     */  

static const char jpeg_caps[]="image/jpeg";
static const char h264_caps[]="video/x-h264,stream-format=(string)byte-stream,alignment=(string)au";
static const char h265_caps[]="video/x-h265,stream-format=(string)byte-stream,alignment=(string)au";

void video_renderer_size(float *f_width_source, float *f_height_source, float *f_width, float *f_height) {
    width_source = (unsigned short) *f_width_source;
    height_source = (unsigned short) *f_height_source;
    width = (unsigned short) *f_width;
    height = (unsigned short) *f_height;
    logger_log(logger, LOGGER_DEBUG, "begin video stream wxh = %dx%d; source %dx%d", width, height, width_source, height_source);
}

GstElement *make_video_sink(const char *videosink, const char *videosink_options) {
    /* used to build a videosink for playbin, using the user-specified string "videosink" */ 
    GstElement *video_sink = gst_element_factory_make(videosink, "videosink");
    if (!video_sink) {
        return NULL;
    }

    /* process the video_sink_options */
    size_t len = strlen(videosink_options);
    if (!len) {
        return video_sink;
    }

    char *options  = (char *) malloc(len + 1);
    strncpy(options, videosink_options, len + 1);

    /* remove any extension begining with "!" */
    char *end = strchr(options, '!');
    if (end) {   
      *end = '\0';
    }

    /* add any fullscreen options "property=pval" included in string videosink_options*/    
    /* OK to use strtok_r in Windows with MSYS2 (POSIX); use strtok_s for MSVC */
    char *token = NULL;
    char *text = options;

    while((token = strtok_r(text, " ", &text))) {
	char *pval = strchr(token, '=');
        if (pval) {
            *pval = '\0';
            pval++;
            const gchar *property_name = (const gchar *) token;
            const gchar *value = (const gchar *) pval;
            g_print("playbin_videosink property: \"%s\" \"%s\"\n", property_name, value);
            gst_util_set_object_arg(G_OBJECT (video_sink), property_name, value);
        }
    }
    free(options);
    return video_sink;
}

#ifdef NEED_G_STRING_REPLACE
guint
g_string_replace (GString     *string,
                  const gchar *find,
                  const gchar *replace,
                  guint        limit)
{
  if (find == NULL || find[0] == '\0')
    return 0;

  gchar **parts = g_strsplit (string->str, find, limit + 1);
  if (parts == NULL || parts[0] == NULL)
    {
      g_strfreev (parts);
      return 0;
    }

  gchar *joined = g_strjoinv (replace, parts);
  g_strfreev (parts);

  g_string_assign (string, joined);
  g_free (joined);

  return g_strv_length (parts);
}
#endif

void video_renderer_init(logger_t *render_logger, const char *server_name, videoflip_t videoflip[2], const char *parser, const char * rtp_pipeline,
                          const char *decoder, const char *converter, const char *videosink, const char *videosink_options, 
                          bool initial_fullscreen, bool video_sync, bool h265_support, bool coverart_support, guint playbin_version, const char *uri) {
    GError *error = NULL;
    GstCaps *caps = NULL;
    bool rtp = (bool) strlen(rtp_pipeline);
    hls_video = (uri != NULL);
    hls_last_known_position = -1.0;   /* per session, never carried over */
    hls_commanded_rate = 1.0f;
#ifdef __APPLE__
    static bool uxvideo_registered = false;
    if (!uxvideo_registered) {
        uxvideo_registered = true;
        if (!gst_is_initialized()) {
            gst_init(NULL, NULL);
        }
        GST_PLUGIN_STATIC_REGISTER(uxvideo);
    }
#endif
    /* videosink choices that are auto */
    auto_videosink = (strstr(videosink, "autovideosink") || strstr(videosink, "fpsdisplaysink"));

    logger = render_logger;
    logger_debug = (logger_get_level(logger) >= LOGGER_DEBUG);
    hls_seek_enabled = FALSE;
    hls_playing = FALSE;
    hls_seek_start = -1;
    hls_seek_end = -1;
    hls_duration = -1;
    hls_buffer_empty = TRUE;
    hls_buffer_full = FALSE;          /* was a second assignment to _empty, so a
                                         full buffer was inherited by the next
                                         session and reported before any data
                                         had arrived */
    type_hls = -1;
    type_264 = -1;
    type_265 = -1;
    type_jpeg = -1;

    /* this call to g_set_application_name makes server_name appear in the  X11 display window title bar, */
    /* (instead of the program name uxplay taken from (argv[0]). It is only set one time. */

    const gchar *appname = g_get_application_name();
    if (!appname || strcmp(appname,server_name))  g_set_application_name(server_name);
    appname = NULL;
    n_renderers = 1;
    /* the renderer for hls video will only be built if a HLS uri is provided in 
     * the call to video_renderer_init, in which case the h264/h265 mirror-mode and jpeg
     * audio-mode renderers will not be built.   This is because it appears that we cannot  
     * put playbin into GST_STATE_READY before knowing the uri (?), so cannot use a
     * unified renderer structure with h264, h265, jpeg and hls  */  
    if (hls_video) {
        type_hls = 0;
    } else {
        type_264 = 0;
        if (h265_support) {
            type_265 = n_renderers++;
        }
        if (coverart_support) {
            type_jpeg = n_renderers++;
        }
    }
    g_assert (n_renderers <= NCODECS);
    for (int i = 0; i < n_renderers; i++) {
        g_assert (i < 3);
        renderer_type[i] = (video_renderer_t *) calloc(1, sizeof(video_renderer_t));
        g_assert(renderer_type[i]);
        renderer_type[i]->autovideo = auto_videosink;
        renderer_type[i]->id = i;
        renderer_type[i]->bus = NULL;
        renderer_type[i]->appsrc = NULL;
        renderer_type[i]->textsrc = NULL;
        renderer_type[i]->uri = NULL;
        renderer_type[i]->eos = FALSE;
        if (hls_video) {
            renderer_type[i]->uri = (char *) calloc(strlen(uri) + 1, sizeof(char));
            memcpy(renderer_type[i]->uri, uri, strlen(uri));
            /* use playbin3 to play HLS video: replace "playbin3" by "playbin" to use playbin2 */
            switch (playbin_version)  {
            case 2:
                renderer_type[i]->pipeline = gst_element_factory_make("playbin", "hls-playbin2");
                break;
            case 3:
                renderer_type[i]->pipeline = gst_element_factory_make("playbin3", "hls-playbin3");
                break;
            default:
                logger_log(logger, LOGGER_ERR, "video_renderer_init: invalid playbin version %u", playbin_version);
                g_assert(0);
            }
            logger_log(logger, LOGGER_INFO, "Will use GStreamer playbin version %u to play HLS streamed video", playbin_version);	    
            g_assert(renderer_type[i]->pipeline);
            renderer_type[i]->codec = hls;
            /* if we are not using an autovideosink, build a videosink based on the string "videosink" */
            if (!auto_videosink) { 
                GstElement *playbin_videosink = make_video_sink(videosink, videosink_options);  
                if (!playbin_videosink) {
                    logger_log(logger, LOGGER_ERR, "video_renderer_init: failed to create playbin_videosink");
                } else {
                    logger_log(logger, LOGGER_DEBUG, "video_renderer_init: create playbin_videosink at %p", playbin_videosink);
                    g_object_set(G_OBJECT (renderer_type[i]->pipeline), "video-sink", playbin_videosink, NULL);
                }
            }
            /* The playlists are served from our own http server on localhost,
               but the media segments come from the internet. Left to measure
               the connection itself, the HLS demuxer times the localhost
               transfers, concludes the link runs at over 100 Mbit/s, and picks
               a variant the real connection cannot sustain; the segment
               fetches then fall behind and the demuxer errors out. Declaring a
               modest speed keeps the choice realistic. */
            g_object_set(renderer_type[i]->pipeline, "connection-speed",
                         (guint64) hls_connection_speed_kbps, NULL);

            gint flags = 0;
            g_object_get(renderer_type[i]->pipeline, "flags", &flags, NULL);
            flags |= GST_PLAY_FLAG_DOWNLOAD;
            flags |= GST_PLAY_FLAG_BUFFERING;    // set by default in playbin3, but not in playbin2; is it needed?
            /* VideoToolbox hands playbin IOSurface-backed GL memory that the
               sink cannot map; copying it fails and freeing it then crashes
               inside IOSurfaceDecrementUseCount. Reproducible with any HLS
               stream and a plain fakesink, so it is not particular to us.
               Only this pipeline is affected: mirroring builds its own and
               keeps hardware decoding. */
            flags |= GST_PLAY_FLAG_FORCE_SW_DECODERS;
            g_object_set(renderer_type[i]->pipeline, "flags", flags, NULL);
            //g_object_set (G_OBJECT (renderer_type[i]->pipeline), "uri", uri, NULL);
        } else {
            bool jpeg_pipeline = false;
            if (i == type_264) {
                renderer_type[i]->codec = h264;
                caps = gst_caps_from_string(h264_caps);
            } else if (i == type_265) {
                renderer_type[i]->codec = h265;
                caps = gst_caps_from_string(h265_caps);
            } else if (i == type_jpeg) {
                jpeg_pipeline = true;
                renderer_type[i]->codec = jpeg;
                caps = gst_caps_from_string(jpeg_caps);
            } else {
                g_assert(0);
            }
            GString *launch = g_string_new("appsrc name=video_source ! ");
            if (jpeg_pipeline) {
                g_string_append(launch, "jpegdec ");
            } else {
                g_string_append(launch, "queue ! ");
                g_string_append(launch, parser);
                g_string_append(launch, " ! ");
                if (!rtp) {
                    g_string_append(launch, decoder);
                } else {
                    g_string_append(launch, "rtph264pay ");
                    g_string_append(launch, rtp_pipeline);
                }
            }
            if (!rtp || jpeg_pipeline) {
                g_string_append(launch, " ! ");
                append_videoflip(launch, &videoflip[0], &videoflip[1]);
                g_string_append(launch, converter);
                g_string_append(launch, " ! ");
                g_string_append(launch, "videoscale ! ");
                if (jpeg_pipeline) {
                    g_string_append(launch, " imagefreeze allow-replace=TRUE ! textoverlay name=metadata_overlay ! ");
                }
                g_string_append(launch, videosink);
                g_string_append(launch, " name=");
                g_string_append(launch, videosink);
                g_string_append(launch, "_");
                g_string_append(launch, renderer_type[i]->codec);
                g_string_append(launch, videosink_options);
                if (video_sync && !jpeg_pipeline) {
                    g_string_append(launch, " sync=true");
                    sync = true;
                } else {
                    g_string_append(launch, " sync=false");
                    sync = false;
                }
            }
            if (!strcmp(renderer_type[i]->codec, h264)) {
                char *pos = launch->str;
                while ((pos = strstr(pos,h265))){
                    pos +=3;
                    *pos = '4';
                }
            } else if (!strcmp(renderer_type[i]->codec, h265)) {
                char *pos = launch->str;
                while ((pos = strstr(pos,h264))){
                    pos +=3;
                    *pos = '5';
                }
            }

            logger_log(logger, LOGGER_DEBUG, "GStreamer video pipeline %d:\n\"%s\"", i + 1, launch->str);
            renderer_type[i]->pipeline = gst_parse_launch(launch->str, &error);
            if (error) {
                logger_log(logger, LOGGER_ERR, "GStreamer gst_parse_launch failed to create video pipeline %d\n"
                           "*** error message from gst_parse_launch was:\n%s\n"
                           "launch string parsed was \n[%s]", i + 1, error->message, launch->str);
                if (strstr(error->message, "no element")) {
                    logger_log(logger, LOGGER_ERR, "This error usually means that a uxplay option was mistyped\n"
                               "           or some requested part of GStreamer is not installed\n");
                }
                g_clear_error (&error);
            }
            g_assert (renderer_type[i]->pipeline);
            GstClock *clock = gst_system_clock_obtain();
            g_object_set(clock, "clock-type", GST_CLOCK_TYPE_REALTIME, NULL);
            gst_pipeline_use_clock(GST_PIPELINE_CAST(renderer_type[i]->pipeline), clock);
            renderer_type[i]->appsrc = gst_bin_get_by_name (GST_BIN (renderer_type[i]->pipeline), "video_source");
            g_assert(renderer_type[i]->appsrc);
            g_object_set(renderer_type[i]->appsrc, "caps", caps, "stream-type", 0, "is-live", TRUE, "format", GST_FORMAT_TIME, NULL);
            g_string_free(launch, TRUE);
            gst_caps_unref(caps);
            gst_object_unref(clock);
            if (jpeg_pipeline) {
                 renderer_type[i]->textsrc = gst_bin_get_by_name(GST_BIN(renderer_type[i]->pipeline), "metadata_overlay");
                 g_object_set(G_OBJECT(renderer_type[i]->textsrc), "text", "", "shaded-background", TRUE, "font-desc", "Sans, 16",  NULL);
            }
        }	
#ifdef X_DISPLAY_FIX
        use_x11 = (strstr(videosink, "xvimagesink") || strstr(videosink, "ximagesink") || auto_videosink);
        fullscreen = initial_fullscreen;
        renderer_type[i]->server_name = server_name;
        renderer_type[i]->gst_window = NULL;
        renderer_type[i]->use_x11 = false;
        X11_search_attempts = 0;
        /* setting char *x11_display_name to NULL means the value is taken from $DISPLAY in the environment 
         * (a uxplay option to specify a different value is possible)  */
        char *x11_display_name = NULL;
        if (use_x11) {
            if (i == 0) {
                renderer_type[0]->gst_window = (X11_Window_t *) calloc(1, sizeof(X11_Window_t));
                g_assert(renderer_type[0]->gst_window);
                get_X11_Display(renderer_type[0]->gst_window, x11_display_name);
                if (renderer_type[0]->gst_window->display) {
                    renderer_type[0]->use_x11 = true;
                } else {
                    free(renderer_type[0]->gst_window);
                    renderer_type[0]->gst_window = NULL;
                }
            } else if (renderer_type[0]->use_x11) {
                renderer_type[i]->gst_window = (X11_Window_t *) calloc(1, sizeof(X11_Window_t));
                g_assert(renderer_type[i]->gst_window);
                memcpy(renderer_type[i]->gst_window, renderer_type[0]->gst_window, sizeof(X11_Window_t));
                renderer_type[i]->use_x11 = true;
            }
        }
#endif
        renderer_type[i]->bus = gst_element_get_bus(renderer_type[i]->pipeline);	
        gst_element_set_state (renderer_type[i]->pipeline, GST_STATE_READY);
        GstState state;
        GstStateChangeReturn ret = gst_element_get_state (renderer_type[i]->pipeline, &state, NULL, 100 * GST_MSECOND);
        if (ret == GST_STATE_CHANGE_SUCCESS) {
            logger_log(logger, LOGGER_DEBUG, "Initialized GStreamer video renderer %d", i + 1);
            if (hls_video && i == 0) {
                renderer = renderer_type[i];
            }
        } else {
            logger_log(logger, LOGGER_ERR, "Failed to initialize GStreamer video renderer %d", i + 1);
            logger_log(logger, LOGGER_INFO, "\nPerhaps your GStreamer installation is missing some required plugins,"
                       "\nor your choices of video options (-vs -vd -vc -fs etc.) are incompatible on"
                       "\nthis computer architecture.  (An example: kmssink with fullscreen option -fs"
                       "\nmay work on some systems, but fail on others)");
            exit(1);
        }
    }
}

/* The pipeline's own state is the truth here: a seek settling or a buffering
   stall can move it without the client having asked for anything. */
bool video_renderer_is_paused() {
    GstState state;

    if (!renderer || !renderer->pipeline) {
        return false;
    }
    gst_element_get_state(renderer->pipeline, &state, NULL, 0);
    return state == GST_STATE_PAUSED;
}

void video_renderer_pause() {
    if (!renderer) {
        return;
    }
    GstStateChangeReturn ret = gst_element_set_state(renderer->pipeline, GST_STATE_PAUSED);
    logger_log(logger, LOGGER_DEBUG, "video renderer pause: %s", gst_element_state_change_return_get_name(ret));
}

void video_renderer_resume() {
    if (!renderer) {
        return;
    }
    gst_element_set_state (renderer->pipeline, GST_STATE_PLAYING);
    GstState state;
    /* wait with timeout 100 msec for pipeline to change state from PAUSED to PLAYING */
    gst_element_get_state(renderer->pipeline, &state, NULL, 100 * GST_MSECOND);
    const gchar *state_name = gst_element_state_get_name(state);
    logger_log(logger, LOGGER_DEBUG, "video renderer resumed: state %s", state_name);
    if (renderer->appsrc) {
        gst_video_pipeline_base_time = gst_element_get_base_time(renderer->appsrc);
    }
}

void video_renderer_start() {
    GstState state;
    const gchar *state_name = NULL;
    if (hls_video) {
        g_object_set (G_OBJECT (renderer->pipeline), "uri", renderer->uri, NULL);
        gst_element_set_state (renderer->pipeline, GST_STATE_PAUSED);
	gst_element_get_state(renderer->pipeline, &state, NULL, 1000 * GST_MSECOND);
	state_name = gst_element_state_get_name(state);
	logger_log(logger, LOGGER_DEBUG, "video renderer_start: state %s", state_name);
        return;
    } 
    /* when not hls, start both h264 and h265 pipelines; will shut down the "wrong" one when we know the codec */
    for (int i = 0; i < n_renderers; i++) {
        gst_element_set_state (renderer_type[i]->pipeline, GST_STATE_PAUSED);
        gst_element_get_state(renderer_type[i]->pipeline, &state, NULL, 1000 * GST_MSECOND);
        state_name = gst_element_state_get_name(state);
        logger_log(logger, LOGGER_DEBUG, "video renderer_start: renderer %d %p state %s", i, renderer_type[i], state_name);
    }
    renderer = NULL;
    first_packet = true;
#ifdef X_DISPLAY_FIX
    X11_search_attempts = 0;
#endif
}

/* used to find any X11 Window used by the playbin (HLS) pipeline after it starts playing. 
*  if use_x11 is true, called every 100 ms after playbin state is READY until the x11 window is found*/
bool waiting_for_x11_window() {
    if (!hls_video) {
        return false;
    }
#ifdef X_DISPLAY_FIX
    if (use_x11 && renderer->gst_window) {
        get_x_window(renderer->gst_window, renderer->server_name);
        if (!renderer->gst_window->window) {
	    return true;    /* window still not found */
        }
    }
    if (fullscreen) {
         set_fullscreen(renderer->gst_window, &fullscreen);
    }
#endif
    return false;
}

/* use this to cycle the jpeg renderer to remove expired coverart when no new coverart has replaced it */
int video_renderer_cycle() {
    if (!renderer || !strstr(renderer->codec, jpeg)) {
        return -1;
    }
    GstState state, pending_state, target_state;
    GstStateChangeReturn ret;
    gst_element_get_state(renderer->pipeline, &state, NULL, 0);
    logger_log(logger, LOGGER_DEBUG, "renderer_cycle renderer %p: initial pipeline state is %s", renderer,
               gst_element_state_get_name(state));

    for (int i = 0 ; i < 2; i++) {
        int count = 0;
        if (i == 0 ) {
            target_state = GST_STATE_NULL;
            video_renderer_stop();
        } else {
            target_state = GST_STATE_PLAYING;
            gst_element_set_state (renderer->pipeline, target_state);
        }
        while (state != target_state) {
            ret = gst_element_get_state(renderer->pipeline, &state, &pending_state, 1000 * GST_MSECOND);
            if (ret == GST_STATE_CHANGE_SUCCESS) {
                logger_log(logger, LOGGER_DEBUG, "current pipeline state is %s", gst_element_state_get_name(state));
                if (pending_state != GST_STATE_VOID_PENDING) {
                    logger_log(logger, LOGGER_DEBUG, "pending pipeline state is %s", gst_element_state_get_name(pending_state));
                }
            } else if (ret == GST_STATE_CHANGE_FAILURE) {
                logger_log(logger, LOGGER_ERR, "pipeline %s: state change to %s failed", renderer->codec, gst_element_state_get_name(target_state));
                count++;
                if (count > 10) {
                    return -1;
                }
            } else if (ret == GST_STATE_CHANGE_ASYNC) {
                logger_log(logger, LOGGER_DEBUG, "state change to %s is asynchronous, waiting for completion ...",
                           gst_element_state_get_name(target_state));
            }
        }
    }
    return 0;
}

void video_renderer_display_jpeg(const void *data, int *data_len) {
    GstBuffer *buffer = NULL;
    if (type_jpeg == -1) {
        return;
    }
    if (renderer && !strcmp(renderer->codec, jpeg)) {
        buffer = gst_buffer_new_allocate(NULL, *data_len, NULL);
	g_assert(buffer != NULL);
        gst_buffer_fill(buffer, 0, data, *data_len);
        gst_app_src_push_buffer (GST_APP_SRC(renderer->appsrc), buffer);
    }  
}

uint64_t video_renderer_render_buffer(unsigned char* data, int *data_len, int *nal_count, uint64_t *ntp_time) {
    GstBuffer *buffer = NULL;
    GstClockTime pts = (GstClockTime) *ntp_time; /*now in nsecs */
    //GstClockTimeDiff latency = GST_CLOCK_DIFF(gst_element_get_current_clock_time (renderer->appsrc), pts);
    if (sync) {
        if (pts >= gst_video_pipeline_base_time) {
            pts -= gst_video_pipeline_base_time;
        } else {
            // adjust timestamps to be >= gst_video_pipeline_base time
            logger_log(logger, LOGGER_DEBUG, "*** invalid ntp_time < gst_video_pipeline_base_time\n%8.6f ntp_time\n%8.6f base_time",
                       ((double) *ntp_time) / SECOND_IN_NSECS, ((double) gst_video_pipeline_base_time) / SECOND_IN_NSECS);
            return  (uint64_t)  gst_video_pipeline_base_time - pts;
        }
    }
    g_assert(data_len != 0);
    /* first four bytes of valid  h264  video data are 0x00, 0x00, 0x00, 0x01.    *
     * nal_count is the number of NAL units in the data: short SPS, PPS, SEI NALs *
     * may  precede a VCL NAL. Each NAL starts with 0x00 0x00 0x00 0x01 and is    *
     * byte-aligned: the first byte of invalid data (decryption failed) is 0x01   */
    if (data[0]) {
        logger_log(logger, LOGGER_ERR, "*** ERROR decryption of video packet failed ");
    } else {
        if (first_packet) {
            logger_log(logger, LOGGER_INFO, "Begin streaming to GStreamer video pipeline");
            first_packet = false;
        }
        if (!renderer || !(renderer->appsrc)) {
            logger_log(logger, LOGGER_DEBUG, "*** no video renderer found");
            return 0;
        }
        buffer = gst_buffer_new_allocate(NULL, *data_len, NULL);
        g_assert(buffer != NULL);
        //g_print("video latency %8.6f\n", (double) latency / SECOND_IN_NSECS);
        if (sync) {
            GST_BUFFER_PTS(buffer) = pts;
        }
        gst_buffer_fill(buffer, 0, data, *data_len);
        gst_app_src_push_buffer (GST_APP_SRC(renderer->appsrc), buffer);
#ifdef X_DISPLAY_FIX
        if (renderer->gst_window && !(renderer->gst_window->window) && renderer->use_x11) {
            X11_search_attempts++;
            logger_log(logger, LOGGER_DEBUG, "Looking for X11 UxPlay Window, attempt %d", (int) X11_search_attempts);
            get_x_window(renderer->gst_window, renderer->server_name);
            if (renderer->gst_window->window) {
                logger_log(logger, LOGGER_INFO, "\n*** X11 Windows: Use key F11 or (left Alt)+Enter to toggle full-screen mode\n");
                if (fullscreen) {
                    set_fullscreen(renderer->gst_window, &fullscreen);
                }
            }
        }
#endif
    }
    return 0;
}

void video_renderer_flush() {
}

void video_renderer_hls_ready() {
    GstState state;
    GstStateChangeReturn ret;
    g_rec_mutex_lock(&renderer_lock);
    if (renderer && hls_video) {
        logger_log(logger, LOGGER_DEBUG,"video_renderer_hls_ready");
        /* Ending the video is a session ending as far as the sink is concerned:
           without this it is only told when the client disconnects, so stopping
           playback while staying connected left the window on screen. Done
           before the pipeline drops to READY, while the sink is still there. */
        video_renderer_set_stream_active(false);
        ret = gst_element_set_state (renderer->pipeline, GST_STATE_READY);
        logger_log(logger, LOGGER_DEBUG,"pipeline_state_change_return: %s",
                   gst_element_state_change_return_get_name(ret));
        gst_element_get_state(renderer->pipeline, &state, NULL, 1000 * GST_MSECOND);
        logger_log(logger, LOGGER_DEBUG,"pipeline state is %s", gst_element_state_get_name(state));
    }
    g_rec_mutex_unlock(&renderer_lock);
}

void video_renderer_set_key_handler(void (*handler)(const char *key)) {
    key_handler = handler;
}

/* Looking the sink up by GstVideoOverlay does not work inside playbin, which
   contains several elements implementing that interface -- playsink answers
   first and has none of these properties, so every setting was quietly thrown
   away and the on-screen controls stayed blank. Search for the element that
   actually carries the property instead. Returns a new reference. */
static GstElement *find_element_with_property(GstBin *bin, const char *name) {
    GstIterator *it;
    GValue item = G_VALUE_INIT;
    GstElement *found = NULL;
    gboolean done = FALSE;

    it = gst_bin_iterate_recurse(bin);
    if (!it) {
        return NULL;
    }
    while (!done) {
        switch (gst_iterator_next(it, &item)) {
        case GST_ITERATOR_OK: {
            GstElement *element = GST_ELEMENT(g_value_get_object(&item));

            if (element &&
                g_object_class_find_property(G_OBJECT_GET_CLASS(element), name)) {
                found = GST_ELEMENT(gst_object_ref(element));
                done = TRUE;
            }
            g_value_reset(&item);
            break;
        }
        case GST_ITERATOR_RESYNC:
            if (found) {
                gst_object_unref(found);
                found = NULL;
            }
            gst_iterator_resync(it);
            break;
        default:
            done = TRUE;
            break;
        }
    }
    g_value_unset(&item);
    gst_iterator_free(it);
    return found;
}

static void video_renderer_set_sink_property(const char *name, ...) {
    GstElement *sink;
    va_list args;

    g_rec_mutex_lock(&renderer_lock);
    if (!renderer || !renderer->pipeline) {
        g_rec_mutex_unlock(&renderer_lock);
        return;
    }
    /* Only the patched osxvideosink carries these; with any other sink nothing
       matches and the call does nothing. */
    sink = find_element_with_property(GST_BIN(renderer->pipeline), name);
    if (!sink) {
        g_rec_mutex_unlock(&renderer_lock);
        return;
    }
    va_start(args, name);
    g_object_set_valist(G_OBJECT(sink), name, args);
    va_end(args);
    gst_object_unref(sink);
    g_rec_mutex_unlock(&renderer_lock);
}

void video_renderer_set_commanded_rate(float rate) {
    hls_commanded_rate = rate;
    if (rate == 0.0f) {
        /* An explicit pause from the client outranks the resume a seek starts:
           claiming to play through one is what made a previous attempt at this
           worse, not better. */
        hls_seek_resuming = FALSE;
    }
}

/* Which display the video window sits on. -1 leaves it where it is. */
void video_renderer_set_display(int index) {
    video_renderer_set_sink_property("display-index", (gint) index, NULL);
}

void video_renderer_set_stream_active(bool active) {
    video_renderer_set_sink_property("stream-active", (gboolean) active, NULL);
}

void video_renderer_show_volume(double level) {
    video_renderer_set_sink_property("volume-osd", level, NULL);
}

void video_renderer_set_playback_info (double position, double duration, double rate) {
    video_renderer_set_sink_property("playback-position", position, NULL);
    video_renderer_set_sink_property("playback-duration", duration, NULL);
    video_renderer_set_sink_property("playback-rate", rate, NULL);
}

bool video_renderer_take_window_closed() {
    bool closed = window_closed;
    window_closed = false;
    return closed;
}

void video_renderer_stop() {
    g_rec_mutex_lock(&renderer_lock);
    if (renderer) {
        logger_log(logger, LOGGER_DEBUG,"video_renderer_stop");
        if (renderer->appsrc) {
            gst_app_src_end_of_stream (GST_APP_SRC(renderer->appsrc));
        }
        gst_element_set_state (renderer->pipeline, GST_STATE_NULL);
        //gst_element_set_state (renderer->playbin, GST_STATE_NULL);
     }
    g_rec_mutex_unlock(&renderer_lock);
}

void video_renderer_set_device_model(const char *model, const char *name) {
    // Device frame not supported in GStreamer renderer
    (void)model;
    (void)name;
}

void video_renderer_set_track_metadata(const char *title, const char *artist, const char *album) {
    // Track metadata display superimposed on coverart is now supported in GStreamer renderer
    GString *metadata = g_string_new("");
    if (artist) {
        g_string_append(metadata, artist);
    }
    if (artist && title) {
        g_string_append(metadata, ": ");
    }
    if (title) {
        g_string_append(metadata, "\"");
        g_string_append(metadata, title);
        g_string_append(metadata, "\"");
    }
    
    g_string_replace (metadata, "&", "&amp;", 0);   //fix pango problem with "&" in text
    if (renderer && renderer->textsrc && (artist || title)) {
        g_object_set(G_OBJECT(renderer->textsrc), "text", metadata->str, NULL);
    }
    g_string_free(metadata, TRUE);
}

static void video_renderer_destroy_instance(video_renderer_t *instance) {
    g_rec_mutex_lock(&renderer_lock);
    if (instance) {
        logger_log(logger, LOGGER_DEBUG,"destroying renderer instance %p codec=%s ", instance, instance->codec);
        GstState state;
        GstStateChangeReturn ret;
        gst_element_get_state(instance->pipeline, &state, NULL, 100 * GST_MSECOND);
        logger_log(logger, LOGGER_DEBUG,"pipeline state is %s", gst_element_state_get_name(state));
        if (state != GST_STATE_NULL) {
            if (!hls_video) {
                gst_app_src_end_of_stream (GST_APP_SRC(instance->appsrc));
            }
            ret = gst_element_set_state (instance->pipeline, GST_STATE_NULL);
            logger_log(logger, LOGGER_DEBUG,"pipeline_state_change_return: %s",
                       gst_element_state_change_return_get_name(ret));
            gst_element_get_state(instance->pipeline, &state, NULL, 1000 * GST_MSECOND);
            logger_log(logger, LOGGER_DEBUG,"pipeline state is %s", gst_element_state_get_name(state));
        }
        if (instance->appsrc) {
            gst_object_unref (instance->appsrc);
            instance->appsrc = NULL;
        }
        if (instance->textsrc) {
            gst_object_unref (instance->textsrc);
            instance->textsrc = NULL;
        }	
        gst_object_unref(instance->bus);
        gst_object_unref(instance->pipeline);
#ifdef X_DISPLAY_FIX
        if (instance->gst_window){
	  // free_X11_Display(instance->gst_window);   without this, a memory leak; with it, a coredump
            free(instance->gst_window);
            instance->gst_window = NULL;
        }
#endif
        if (instance->uri) {
            free(instance->uri);
        }
        /* The parameter used to be called "renderer" too, so clearing it here
           only cleared the parameter: the global went on pointing at the freed
           instance, and the next /playback-info poll read a pipeline pointer
           out of freed memory and crashed on it. */
        if (renderer == instance) {
            renderer = NULL;
        }
        for (int i = 0; i < n_renderers; i++) {
            if (renderer_type[i] == instance) {
                renderer_type[i] = NULL;
            }
        }
        free (instance);
        logger_log(logger, LOGGER_DEBUG,"renderer destroyed\n");	
    }
    g_rec_mutex_unlock(&renderer_lock);
}

void video_renderer_destroy() {
    for (int i = 0; i < n_renderers; i++) {
        if (renderer_type[i]) {
            video_renderer_destroy_instance(renderer_type[i]);
        }
    }
}

static void get_stream_status_name(GstStreamStatusType type, char *name, size_t len) {
    switch (type) {
    case GST_STREAM_STATUS_TYPE_CREATE:
        strncpy(name, "CREATE", len);
        return;
    case GST_STREAM_STATUS_TYPE_ENTER:
        strncpy(name, "ENTER", len);
        return;
    case GST_STREAM_STATUS_TYPE_LEAVE:
        strncpy(name, "LEAVE", len);
        return;
    case GST_STREAM_STATUS_TYPE_DESTROY:
        strncpy(name, "DESTROY", len);
        return;
    case GST_STREAM_STATUS_TYPE_START:
        strncpy(name, "START", len);
        return;
    case GST_STREAM_STATUS_TYPE_PAUSE:
        strncpy(name, "PAUSE", len);
        return;
    case GST_STREAM_STATUS_TYPE_STOP:
        strncpy(name, "STOP", len);
        return;
    default:
        strncpy(name, "", len);
        return;
    }
}

static void hls_video_seek_to_start_position(GstElement *pipeline) {
    if (!hls_requested_start_position || !hls_seek_enabled ||
        hls_requested_start_position < hls_seek_start ||
        hls_requested_start_position > hls_seek_end) {
        return;
    }
    if (hls_start_seek_attempts >= HLS_START_SEEK_MAX_ATTEMPTS) {
        logger_log(logger, LOGGER_WARNING, "giving up on the requested start position %"
                   GST_TIME_FORMAT " after %d attempts",
                   GST_TIME_ARGS(hls_requested_start_position), hls_start_seek_attempts);
        hls_requested_start_position = 0;
        return;
    }
    hls_start_seek_attempts++;
    g_print("***************** seek to hls_requested_start_position %" GST_TIME_FORMAT "\n", GST_TIME_ARGS(hls_requested_start_position));
    /* ACCURATE, not KEY_UNIT: the same undershoot that made scrubbing land five
       to twelve seconds short applies here, and here it decides where the item
       the client just handed us begins. */
    if (gst_element_seek_simple (pipeline, GST_FORMAT_TIME,
			     GST_SEEK_FLAG_FLUSH | GST_SEEK_FLAG_ACCURATE, hls_requested_start_position)) {
        hls_last_known_position = ((double) hls_requested_start_position) / GST_SECOND;
        /* Deliberately not retired here. A seek issued while the pipeline is
           still coming up returns success and is then undone by the preroll
           that follows -- measured: the seek reported success and the item
           played from zero, with a second PAUSED->PLAYING right behind it. The
           position actually arriving is what retires this, in the bus callback,
           so a seek that did not take is simply tried again. */
    } else {
        g_print("*** seek to requested_start_position failed\n");
    }
}

static gboolean gstreamer_video_pipeline_bus_callback(GstBus *bus, GstMessage *message, void *loop) {
    GstState old_state, new_state;
    const gchar no_state[] = "";
    const gchar *old_state_name = no_state, *new_state_name = no_state;
    if (GST_MESSAGE_TYPE(message) == GST_MESSAGE_STATE_CHANGED) {
        GstState old_state, new_state;
        gst_message_parse_state_changed (message, &old_state, &new_state, NULL);
        old_state_name = gst_element_state_get_name (old_state);
        new_state_name = gst_element_state_get_name (new_state);
    }

    /* identify which pipeline sent the message */ 
    int type = -1;
    for (int i = 0 ; i < n_renderers ; i ++ ) {
        if (renderer_type[i] && renderer_type[i]->bus == bus) {
            type = i;
            break;
        }
    }

    /* if the bus sending the message is not found, the renderer may already have been destroyed */
    if (type == -1) {
        if (logger_debug) {
            g_print("GStreamer(UNKNOWN, now destroyed?) bus message: %s %s %s %s\n",
                     GST_MESSAGE_SRC_NAME(message), GST_MESSAGE_TYPE_NAME(message), old_state_name, new_state_name);
        }     
        return TRUE;
    }

    video_renderer_t *renderer = renderer_type[type];

    gint64 pos = -1;
    if (hls_video) {
        gst_element_query_position (renderer->pipeline, GST_FORMAT_TIME, &pos);
        /* Retire the requested start position only once playback has actually
           arrived there, and only on a pipeline that has answered a seeking
           query of its own. Ungated, this ran on the pipeline being replaced --
           whose position is the outgoing item's, far past the start the client
           asked for for the new one -- and threw the request away before the new
           pipeline existed. Measured: the client asked for 18:31 and for 1:57,
           and both items played from zero. */
        if (hls_seek_enabled && hls_requested_start_position &&
            GST_CLOCK_TIME_IS_VALID(pos) &&
            pos + GST_SECOND >= hls_requested_start_position) {
            hls_requested_start_position = 0;
            hls_start_seek_attempts = 0;
        }
    }

    if (logger_debug) {
        gchar *name = NULL;
        GstElement *element = NULL;
        gchar type_name[8] = { 0 };
        if (GST_MESSAGE_TYPE(message) == GST_MESSAGE_STREAM_STATUS) {
            GstStreamStatusType type;
            gst_message_parse_stream_status(message, &type, &element);
            name = gst_element_get_name(element);
            get_stream_status_name(type, type_name, 8);
            old_state_name = name;
            new_state_name = type_name;
        }
        if (GST_CLOCK_TIME_IS_VALID(pos)) {
            g_print("GStreamer %s  bus message %s %s %s %s; position: %" GST_TIME_FORMAT "\n" ,renderer->codec,
                     GST_MESSAGE_SRC_NAME(message), GST_MESSAGE_TYPE_NAME(message), old_state_name, new_state_name, GST_TIME_ARGS(pos));
        } else {
            g_print("GStreamer %s bus message %s %s %s %s\n", renderer->codec,
                    GST_MESSAGE_SRC_NAME(message), GST_MESSAGE_TYPE_NAME(message), old_state_name, new_state_name);
        }
        if (name) {
            g_free(name);
        }
    }

    if (hls_video && !GST_CLOCK_TIME_IS_VALID(hls_duration)) {
        gst_element_query_duration (renderer->pipeline, GST_FORMAT_TIME, &hls_duration);
    }

    if (hls_requested_start_position && hls_seek_enabled) {
        hls_video_seek_to_start_position(renderer->pipeline);
    }

    switch (GST_MESSAGE_TYPE (message)) {
    case GST_MESSAGE_DURATION:
        hls_duration = GST_CLOCK_TIME_NONE;
        break;
    case GST_MESSAGE_BUFFERING:
        if (hls_video) {
            gint percent = -1;
            gst_message_parse_buffering(message, &percent);
            if (percent < 0) {
                break;
            }
            /* These two are what the client sees as playbackBufferEmpty and
               playbackBufferFull, and nothing else in the program writes them.
               They used to be set from inside the percent > 0 branch below, so a
               single 0% message -- which is what a seek out of the buffered
               window produces -- left "buffer empty" latched on for the rest of
               the session while the pipeline played on quite happily. A client
               that is told the buffer is empty treats the item as stalled and
               stops advancing its progress bar, however correct the position we
               report is. Derive them from the level every time instead. */
            renderer->buffering_level = percent;
            hls_buffer_empty = (percent == 0);
            hls_buffer_full = (percent == 100);
            logger_log(logger, LOGGER_DEBUG, "Buffering :%d percent done", percent);
            /* Which way the pipeline is driven is left exactly as it was: only
               a level above 0 moves it. A 0% message arrives with the buffer
               already drained and is followed by the refill, so holding the
               pipeline there buys nothing and risks stranding it in PAUSED if
               no further message follows. */
            if (percent > 0) {
                /* Holding the pipeline back until the buffer has refilled,
                   and letting it go once it has. This is what restarts
                   playback after a seek: the seek empties the buffer, the
                   client's own pause lands on top, and nothing else would ever
                   move the pipeline again. */
                if (percent < 100) {
                    hls_paused_for_buffering = TRUE;
                    gst_element_set_state (renderer->pipeline, GST_STATE_PAUSED);
                } else {
                    hls_paused_for_buffering = FALSE;
                    /* Only if the client still wants to be playing. Resuming
                       here regardless overrode a POST /rate?value=0 that
                       arrived while the buffer was refilling: the video ran on
                       while the client sat believing it had paused, its
                       progress bar frozen, and since it had never asked to
                       resume it never sent a rate of 1 to put the two back
                       together. A seek sets the commanded rate itself, so the
                       resume a seek depends on still happens. */
                    if (hls_commanded_rate > 0.0f) {
                        gst_element_set_state (renderer->pipeline, GST_STATE_PLAYING);
                    }
                }
            }
        }
	break;      
    case GST_MESSAGE_ERROR: {
        GError *err = NULL;
        gchar *debug = NULL;
        gboolean flushing = FALSE;
        gboolean closed_window = FALSE;
        gst_message_parse_error (message, &err, &debug);
        logger_log(logger, LOGGER_INFO, "GStreamer error (video): %s %s", GST_MESSAGE_SRC_NAME(message),err->message);
        if (strstr(err->message, "Output window was closed")) {
            closed_window = TRUE;
        }
        if (!hls_video && strstr(err->message,"Internal data stream error")) {
            logger_log(logger, LOGGER_INFO,
                     "*** This is a generic GStreamer error that usually means that GStreamer\n"
                     "*** was unable to construct a working video pipeline.\n\n"
                     "*** If you are letting the default autovideosink select the videosink,\n"
                     "*** GStreamer may be trying to use non-functional hardware h264 video decoding.\n"
                     "*** Try using option -avdec to force software decoding or use -vs <videosink>\n"
                     "*** to select a videosink of your choice (see \"man uxplay\").\n\n"
                     "*** Raspberry Pi models 4B and earlier using Video4Linux2 may need \"-bt709\" uxplay option");
        }
        g_error_free (err);
        g_free (debug);
        if (renderer->appsrc) {
            gst_app_src_end_of_stream (GST_APP_SRC(renderer->appsrc));
        }
        if (closed_window) {
            window_closed = true;
        }
        if (!hls_video || closed_window) {
            gst_bus_set_flushing(bus, TRUE);
            gst_element_set_state (renderer->pipeline, GST_STATE_READY);
            g_main_loop_quit( (GMainLoop *) loop);
        }
        break;
    }
    case GST_MESSAGE_EOS:
        /* end-of-stream */
        logger_log(logger, LOGGER_INFO, "GStreamer: End-Of-Stream (video)");
        /* Whatever seek was settling is over: there is no more stream for it to
           land in. Only reaching PLAYING cleared this otherwise, which the end
           of a video never does -- scrub to the very end and the flag stayed set
           for good, pinning the reported rate at 0 and freezing the client's
           display until the next seek happened to move the pipeline again. */
        hls_seek_resuming = FALSE;
        if (hls_video && renderer) {
            /* Hold at the end rather than dropping to READY, and leave the bus
               alone. Flushing it threw away every later message, buffering
               included -- and buffering is what brings the pipeline back to
               PLAYING after a seek, so scrubbing once the video had run out
               prerolled and then sat in PAUSED for good. Tearing the pipeline
               down belongs to the path that runs when the client actually
               leaves, not to reaching the end of a video. */
            gst_element_set_state (renderer->pipeline, GST_STATE_PAUSED);
            renderer->eos = TRUE;
        }
        break;
    case GST_MESSAGE_STATE_CHANGED:
        if (hls_video && strstr(GST_MESSAGE_SRC_NAME(message), "hls-playbin")) {
            GstState old_state, new_state;
            gst_message_parse_state_changed (message, &old_state, &new_state, NULL);
            if (logger_debug) {
            g_print ("****** hls_playbin: Element %s changed state from %s to %s.\n", GST_OBJECT_NAME (message->src),
                     gst_element_state_get_name (old_state),
                     gst_element_state_get_name (new_state));
            } 
            if (new_state != GST_STATE_PLAYING) {
                hls_playing = FALSE;
                break;
            }
            hls_playing = TRUE;
            /* A pipeline that has reached PLAYING is not starved, whatever the
               last buffering message claimed. Buffering levels are the only
               other thing that clears these, so a 0% with no message after it
               would otherwise keep telling the client the item is stalled for
               the rest of the session. */
            hls_buffer_empty = FALSE;
            hls_paused_for_buffering = FALSE;
            GstQuery *query = NULL;
            query = gst_query_new_seeking(GST_FORMAT_TIME);
                if (gst_element_query(renderer->pipeline, query)) {
	        gst_query_parse_seeking (query, NULL, &hls_seek_enabled, &hls_seek_start, &hls_seek_end);
                if (hls_seek_enabled) {
                    g_print ("Seeking is ENABLED from %" GST_TIME_FORMAT " to %" GST_TIME_FORMAT "\n",
			     GST_TIME_ARGS (hls_seek_start), GST_TIME_ARGS (hls_seek_end));
                } else {
                    g_print ("Seeking is DISABLED for this stream.\n");
                }
            } else {
                g_printerr ("Seeking query failed.");
            }
            gst_query_unref (query);

            if (hls_requested_start_position && hls_seek_enabled) {
                hls_video_seek_to_start_position(renderer->pipeline);
            }

        }
        if (renderer->autovideo) {
            char *sink = strstr(GST_MESSAGE_SRC_NAME(message), "-actual-sink-");
            if (sink) {
                sink += strlen("-actual-sink-");
                if (strstr(GST_MESSAGE_SRC_NAME(message), renderer->codec)) {
                    logger_log(logger, LOGGER_DEBUG, "GStreamer: automatically-selected videosink"
                               " (renderer %d: %s) is \"%ssink\"", renderer->id + 1,
                               renderer->codec, sink);
#ifdef X_DISPLAY_FIX
                    renderer->use_x11 = (strstr(sink, "ximage") || strstr(sink, "xvimage"));
#endif
                    renderer->autovideo = false;
                }
            }
        }
        break;
    case GST_MESSAGE_ELEMENT: {
        /* Not guarded by X_DISPLAY_FIX: keys from the videosink are the only
           way the video window can drive anything, and that matters on every
           platform, not just the X11 build. */
        GstNavigationMessageType message_type = gst_navigation_message_get_type (message);
        if (message_type == GST_NAVIGATION_MESSAGE_EVENT) {
            GstEvent *event = NULL;
            if (gst_navigation_message_parse_event (message, &event)) {
                GstNavigationEventType event_type = gst_navigation_event_get_type (event);
                const gchar *key = NULL;
                switch (event_type) {
                case GST_NAVIGATION_EVENT_KEY_PRESS:
                    if (gst_navigation_event_parse_key_event (event, &key)) {
#ifdef  X_DISPLAY_FIX
                        if (renderer->gst_window && renderer->gst_window->window) {
                            if ((strcmp (key, "F11") == 0) || (alt_keypress && strcmp (key, "Return") == 0)) {
                                fullscreen = !(fullscreen);
                                set_fullscreen(renderer->gst_window, &fullscreen);
                            } else if (strcmp (key, "Alt_L") == 0) {
                                alt_keypress = true;
                            }
                        }
#endif
                        if (key_handler) {
                            key_handler (key);
                        }
                    }
                    break;
                case GST_NAVIGATION_EVENT_KEY_RELEASE:
#ifdef  X_DISPLAY_FIX
                    if (renderer->gst_window && renderer->gst_window->window &&
                        gst_navigation_event_parse_key_event (event, &key)) {
                        if (strcmp (key, "Alt_L") == 0) {
                            alt_keypress = false;
                        }
                    }
#endif
                    break;
                default:
                    break;
                }
            }
            if (event) {
                gst_event_unref (event);
            }
        }
        break;
    }
    default:
      /* unhandled message */
        break;
    }
    return TRUE;
}

int video_renderer_choose_codec (bool video_is_jpeg, bool video_is_h265) {
    video_renderer_t *renderer_used = NULL;
    g_assert(!hls_video);
    if (video_is_jpeg) {
        g_assert(type_jpeg != -1);
        renderer_used = renderer_type[type_jpeg];
    } else {
        if (video_is_h265) {
            if (type_265 == -1) {
                logger_log(logger, LOGGER_ERR, "video is h265 but the -h265 option was not used");
                return -1;
            }
            renderer_used = renderer_type[type_265];
        } else {
            g_assert(type_264 != -1);
            renderer_used = renderer_type[type_264];
        }
    }
    if (renderer_used == NULL) {
        return -1;
    } else if (renderer_used == renderer) {
        return 0;
    } else if (renderer) {
        return -1;
    }
    renderer = renderer_used;
    gst_element_set_state (renderer->pipeline, GST_STATE_PLAYING);
    GstState old_state, new_state;
    if (gst_element_get_state(renderer->pipeline, &old_state, &new_state, 100 * GST_MSECOND) == GST_STATE_CHANGE_FAILURE) {
        g_error("video pipeline failed to go into playing state");
        return -1;
    }
    logger_log(logger, LOGGER_DEBUG, "video_pipeline state change from %s to %s\n",
               gst_element_state_get_name (old_state),gst_element_state_get_name (new_state));
    gst_video_pipeline_base_time = gst_element_get_base_time(renderer->appsrc);
    if (strstr(renderer->codec, h265)) {
        logger_log(logger, LOGGER_INFO, "*** video format is h265 high definition (HD/4K) video %dx%d", width, height);
    }
    /* destroy unused renderers */
    for (int i = 0; i < n_renderers; i++) {
        if (renderer_type[i] == renderer) {
            continue;
        }
	if (renderer_type[i]) {
            video_renderer_t *renderer_unused = renderer_type[i];
            renderer_type[i] = NULL;
            video_renderer_destroy_instance(renderer_unused);
        }
    }
    return 0;
}
    

static bool video_get_playback_info_locked(double *duration, double *position, double *seek_start, double *seek_duration, float *rate, bool *buffer_empty, bool *buffer_full);

bool video_get_playback_info(double *duration, double *position, double *seek_start, double *seek_duration, float *rate, bool *buffer_empty, bool *buffer_full) {
    bool ret;

    g_rec_mutex_lock(&renderer_lock);
    ret = video_get_playback_info_locked(duration, position, seek_start, seek_duration,
                                         rate, buffer_empty, buffer_full);
    g_rec_mutex_unlock(&renderer_lock);
    return ret;
}

static bool video_get_playback_info_locked(double *duration, double *position, double *seek_start, double *seek_duration, float *rate, bool *buffer_empty, bool *buffer_full) {
    gint64 pos = 0;
    GstState state;
    *duration = 0.0;
    *position = -1.0;
    *seek_start = 0.0;
    *seek_duration = 0.0;
    *rate = 0.0f;
    /* Set before the early return below: these go straight into the plist the
       client is sent, and leaving them to the caller's stack meant an empty or
       full buffer was reported at random whenever there was no renderer. */
    *buffer_empty = (bool) hls_buffer_empty;
    *buffer_full = (bool) hls_buffer_full;
    if (!renderer) {
        return true;
    }

    if (hls_seek_enabled) {
        *seek_start = ((double) hls_seek_start) / GST_SECOND;
        *seek_duration = ((double) (hls_seek_end - hls_seek_start)) / GST_SECOND;
    }

    GstState pending = GST_STATE_VOID_PENDING;
    GstStateChangeReturn state_ret =
        gst_element_get_state(renderer->pipeline, &state, &pending, 0);
    if (state == GST_STATE_PLAYING) {
        hls_seek_resuming = FALSE;
    }

    /* The rate is the client's permission to accept a jump in the position. The
       client does follow the position reported here -- seek forward on the
       receiver and its display lands on the new time -- but only forwards,
       because while it is told the stream runs at rate 1 that is the only way a
       playing stream can behave. A report that goes backwards under rate 1
       describes something impossible, so it is dropped, and the client's clock,
       now ahead of ours, drops everything after it too. A seek the client makes
       itself always works: it sends its own rate of 0 first, so it knows the
       break is coming.

       So a seek made on the receiver has to show as the break it is. While one
       is settling the pipeline's real state is reported, a rate of 0 until it
       reaches PLAYING again -- 1, then 0, then 1, the shape the client produces
       around its own seeks.

       This undoes an earlier attempt that pinned the rate at 1 right through a
       seek to stop the client latching to stopped. That masking is what denies
       the client the discontinuity. Outside a seek the smoothing below stays,
       so ordinary rebuffering still does not read as a stop. */
    *rate = 0.0f;
    if (state == GST_STATE_PLAYING) {
        *rate = 1.0f;
    } else if (!hls_seek_resuming &&
               ((state_ret == GST_STATE_CHANGE_ASYNC && pending == GST_STATE_PLAYING) ||
                (hls_paused_for_buffering && hls_commanded_rate > 0.0f))) {
        /* Buffering is not the same thing as stopped. The pipeline is held in
           PAUSED while it refills; calling that a rate of 0 tells the client
           playback ended, and its transport latches to stopped even though we
           resume moments later. The buffer_empty/full fields carry the
           buffering state instead. */
        *rate = 1.0f;
    }

    /* The buffer our own flushing seek just drained is not the client's
       business. Measured: a report carrying buffer_empty landed between the
       seek and the pipeline reaching PLAYING, and the client answered it with
       POST /rate?value=0 -- it treats an empty buffer as a stall and pauses
       itself. From there its transport and ours never line up again. Whether a
       poll falls inside that window is why the same seek sometimes worked and
       sometimes did not, and why a short skip that stays inside the buffer
       always did. It also contradicts the rate of 1 we deliberately report
       through a seek. A stall during ordinary playback is still reported. */
    if (hls_seek_resuming) {
        *buffer_empty = false;
    }

    if (!GST_CLOCK_TIME_IS_VALID(hls_duration)) {
        if (!gst_element_query_duration (renderer->pipeline, GST_FORMAT_TIME, &hls_duration)) {
            return true;
        }
    }
    *duration = ((double) hls_duration) / GST_SECOND;
    if (*duration) {
        if (gst_element_query_position (renderer->pipeline, GST_FORMAT_TIME, &pos) &&
                                        GST_CLOCK_TIME_IS_VALID(pos)) {
            *position = ((double) pos) / GST_SECOND;
            hls_last_known_position = *position;
        } else if (hls_last_known_position >= 0.0) {
            /* The query fails while a flushing seek is still settling. Sending
               the -1 this started as, next to a rate of 1, describes a stream
               that is playing from nowhere; the client's playback state never
               recovers from it, and the app stays broken until restarted.
               Repeating the last real position keeps the report coherent. */
            *position = hls_last_known_position;
        }
    }

    /* buffer_empty is what the client reads as a stall, so it belongs next to
       the position it is being sent with -- it is the field to watch when the
       app's progress bar stops following a seek. */
    logger_log(logger, LOGGER_DEBUG, "******* video_get_playback_info: position %" GST_TIME_FORMAT " duration %" GST_TIME_FORMAT " %s rate %f buffer_empty %d buffer_full %d *****",
               GST_TIME_ARGS (pos), GST_TIME_ARGS (hls_duration), gst_element_state_get_name(state), *rate,
               (int) *buffer_empty, (int) *buffer_full);

    return true;
}

void video_renderer_set_start(float position) {
    /* Same overflow as video_renderer_seek() had: an int of microseconds wraps
       at 2147 seconds. */
    hls_requested_start_position = (gint64) ((gdouble) position * GST_SECOND);
    hls_start_seek_attempts = 0;
    /* The pipeline still running belongs to the item being replaced, and its
       seekable range is that item's. Withdrawing it here keeps the outgoing
       pipeline from seeking to a position that means nothing in it -- it tried,
       and failed, eleven times in a row -- and from retiring the request before
       the new pipeline is up. The new one enables this again once it has
       answered a seeking query for itself. */
    hls_seek_enabled = FALSE;
    logger_log(logger, LOGGER_DEBUG, "register HLS video start position %f %lld", position,
               hls_requested_start_position);
}

void video_renderer_seek(float position) {
    /* Computed directly in nanoseconds: going through an int of microseconds
       overflows at 2147 seconds, so any seek past 35:47 wrapped negative and
       was then clamped to the start of the video. */
    gint64 seek_position = (gint64) ((gdouble) position * GST_SECOND);
    if (hls_duration < 2000) return;
    seek_position =  seek_position < 1000 ? 1000 : seek_position;
    /* Leave enough of the video to actually play. The end clamp was a
       microsecond, which drops the pipeline straight into EOS: it never reaches
       PLAYING, so the rate never returns to 1, and the client -- which commits a
       jump in the position on exactly that edge -- was left showing wherever it
       had been. Landing a second short lets playback resume, the client follow,
       and the video then end on its own, which is what dragging a scrubber to
       the end does anywhere else. */
    {
        gint64 tail = (hls_duration > 2 * GST_SECOND) ? GST_SECOND : 1000;

        if (seek_position > hls_duration - tail) {
            seek_position = hls_duration - tail;
        }
    }
    g_print("SCRUB: seek to %f secs =  %" GST_TIME_FORMAT ", duration = %" GST_TIME_FORMAT "\n", position,
            GST_TIME_ARGS(seek_position),  GST_TIME_ARGS(hls_duration));
    /* KEY_UNIT snaps back to the keyframe before the target, landing five to
       twelve seconds short of what was asked for. The client watches for its
       requested position to be reached before it treats the seek as done, so
       an undershoot on every seek leaves its transport waiting. A short skip
       lands close enough not to notice, which is why those always worked. */
    gboolean result = gst_element_seek_simple(renderer->pipeline, GST_FORMAT_TIME,
                                              (GstSeekFlags)(GST_SEEK_FLAG_FLUSH | GST_SEEK_FLAG_ACCURATE),
                                              seek_position);
    if (result) {
        g_print("seek succeeded\n");
        /* The client pauses just before scrubbing and does not send a play of
           its own afterwards: resuming once the seek lands is the receiver's
           job. This only works alongside leaving the buffering messages alone
           -- while both were driving the state, the re-buffering that follows
           a seek pulled the pipeline back to PAUSED a fraction of a second
           later, which is why playback used to die immediately after. */
        hls_commanded_rate = 1.0f;
        hls_seek_resuming = TRUE;
        /* The position query fails for a moment while the seek settles, and the
           fallback below would otherwise answer with where playback was before
           it -- telling the client its seek was ignored, and leaving the app's
           own idea of the position stuck there. Move the fallback to the place
           we were asked for. */
        hls_last_known_position = ((double) seek_position) / GST_SECOND;
        gst_element_set_state (renderer->pipeline, GST_STATE_PLAYING);
    } else {
        g_print("seek failed\n");
    }
}

unsigned int video_renderer_listen(void *loop, int id) {
    g_assert(id >= 0 && id < n_renderers);
    g_assert (renderer_type[id] && renderer_type[id]->bus);
    return (unsigned int) gst_bus_add_watch(renderer_type[id]->bus,(GstBusFunc)
                                            gstreamer_video_pipeline_bus_callback, (gpointer) loop);    
}

bool video_renderer_eos_watch() {
    /* Runs on a 100ms timer, including while the loop thread is replacing the
       renderer, so the pointer has to be checked. */
    if (hls_video && renderer && renderer->eos) {
        renderer->eos = FALSE;
	return true;
    }
    return false; 
}

void video_renderer_hls_set_volume(double volume) {
    if (!renderer || strcmp(renderer->codec, hls)) {
       return;
    }
    volume = (volume > 10.0) ? 10.0 : volume;
    volume = (volume < 0.0) ? 0.0 : volume;
    g_object_set(renderer->pipeline, "volume", volume, NULL);
}
