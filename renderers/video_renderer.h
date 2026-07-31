/**
 * RPiPlay - An open-source AirPlay mirroring server for Raspberry Pi
 * Copyright (C) 2019 Florian Draschbacher
 * Modified for:
 * UxPlay - An open-source AirPlay mirroring server
 * Copyright (C) 2021-23 F. Duncanh
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

/* 
 * H264 renderer using gstreamer
*/

#ifndef VIDEO_RENDERER_H
#define VIDEO_RENDERER_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>
#include "../lib/logger.h"

typedef enum videoflip_e {
    NONE,
    LEFT,
    RIGHT,
    INVERT,
    VFLIP,
    HFLIP,
} videoflip_t;

typedef struct video_renderer_s video_renderer_t;

void video_renderer_init (logger_t *logger, const char *server_name, videoflip_t videoflip[2], const char *parser, const char *rtp_pipeline,
                          const char *decoder, const char *converter, const char *videosink, const char *videosink_options,
                          bool initial_fullscreen, bool video_sync, bool h265_support, bool coverart_support,
                          guint playbin_version,  const char *uri);
void video_renderer_start ();
void video_renderer_stop ();

/* TRUE once if the user closed the video window; clears itself when read, so
   the caller can treat it as the end of a session rather than a shutdown. */
bool video_renderer_take_window_closed ();

/* Called with the key name for each key pressed in the video window, as
   reported by the videosink through GstNavigation. */
void video_renderer_set_key_handler (void (*handler)(const char *key));

/* Flash a volume readout over the video, if the videosink can show one.
   Level is 0.0 - 1.0. Ignored by sinks without the feature. */
void video_renderer_show_volume (double level);

/* Feed the videosink the stream's position and length, so a sink that draws a
   transport bar has something to draw. A duration of 0 means no timeline. */
void video_renderer_set_playback_info (double position, double duration, double rate);

/* Tell the videosink a session has ended. The pipeline is not always taken
   down when a client leaves (see the -nc workaround), so a sink that hides its
   window while idle cannot work this out for itself. */
void video_renderer_set_stream_active (bool active);

/* The playback rate the client last commanded (0 or 1). Reported back to it
   as-is: a receiver follows the client's intent rather than second-guessing it
   from the pipeline, which lags behind while a seek settles. */
void video_renderer_set_commanded_rate (float rate);
void video_renderer_set_device_model(const char *model, const char *name);
void video_renderer_set_track_metadata(const char *title, const char *artist, const char *album);
void video_renderer_pause ();
void video_renderer_hls_ready ();
void video_renderer_seek(float position);
void video_renderer_set_display(int index);
void video_renderer_set_start(float position);
void video_renderer_resume ();
int video_renderer_cycle ();
bool video_renderer_is_paused();
bool video_renderer_eos_watch();
uint64_t  video_renderer_render_buffer (unsigned char* data, int *data_len, int *nal_count, uint64_t *ntp_time);
void video_renderer_display_jpeg(const void *data, int *data_len);
void video_renderer_flush ();
unsigned int video_renderer_listen(void *loop, int id);
void video_renderer_destroy ();
void video_renderer_size(float *width_source, float *height_source, float *width, float *height);
bool waiting_for_x11_window();
bool video_get_playback_info(double *duration, double *position, double *seek_start, double *seek_duration, float *rate, bool *buffer_empty, bool *buffer_full);
int video_renderer_choose_codec (bool video_is_jpeg, bool video_is_h265);
unsigned int video_renderer_listen(void *loop, int id);
bool video_renderer_eos_watch();
void video_renderer_hls_set_volume(double volume);
#ifdef __cplusplus
}
#endif

#endif //VIDEO_RENDERER_H

