
/*
 * Author: Symphony teleca corporation,Bangalore.
 * Date: Nov- 10 - 2013
 *
 * File Name: demux_audio_video.c
 *
 * 
 * This application contstructs pipeline using the gstreamer to demux audio and video data from the input file
 * and sends the demuxed over the pipes. 
 * 
 *
 * Copyright (c) <2013>, Intel Corporation.
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms and conditions of the GNU Lesser General Public License,
 * version 2.1, as published by the Free Software Foundation.
 *
 * This program is distributed in the hope it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU Lesser General Public License for
 * more details.
 *
 * You should have received a copy of the GNU Lesser General Public License along with
 * this program; if not, write to the Free Software Foundation, Inc.,
 * 51 Franklin St - Fifth Floor, Boston, MA 02110-1301 USA.
 * 
 */


#include <gst/gst.h>
#include <gst/app/gstappsrc.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
//#define DEBUG

/*IPC pipe descriptors to send the audio and video data*/
extern int audio_fd[2];
extern int video_fd[2];


/*Gstreamer pipeline structure*/

struct _App
{
  GstElement *pipeline;
  GstElement *file_source;
  GMainLoop *loop;
  GstElement *appsink;
  GstElement *video_sink; 
  GTimer *timer;

};
typedef struct _App App;
App s_app;
App *app;



#ifdef DEBUG


extern int count ;
static gboolean callBack (GstClock * clock, GstClockTime time,GstClockID id, gpointer user_data) 
{

   printf("count = %d\n",count);
   
   count = 0;
   return 0;
}
#endif






static gboolean bus_message (GstBus * bus, GstMessage * message, App * app)
{
  GST_DEBUG ("got message %s",
      gst_message_type_get_name (GST_MESSAGE_TYPE (message)));

  switch (GST_MESSAGE_TYPE (message)) {
    case GST_MESSAGE_ERROR: {
        GError *err = NULL;
        gchar *dbg_info = NULL;

        gst_message_parse_error (message, &err, &dbg_info);
        g_printerr ("ERROR from element %s: %s\n",
            GST_OBJECT_NAME (message->src), err->message);
        g_printerr ("Debugging info: %s\n", (dbg_info) ? dbg_info : "none");
        g_error_free (err);
        g_free (dbg_info);
        g_main_loop_quit (app->loop);
        break;
    }
    case GST_MESSAGE_EOS:
      g_main_loop_quit (app->loop);
      break;
    default:
      break;
  }
  return TRUE;
}


int gst_main (char* fileData)
{
  
  app = &s_app;
  GstBus *bus;
 #ifdef DEBUG
  GstClockID timer;
  GstClock *clock;
 #endif    
  gst_init (NULL, NULL);
  
    
  app->loop = g_main_loop_new (NULL, TRUE);
  #ifdef DEBUG
  app->timer = g_timer_new();
  #endif

 app->pipeline = gst_parse_launch("filesrc name=file ! qtdemux name=demux demux.audio_0 ! queue ! aacparse ! mpegtsmux ! fdsink name=    audiosink  demux.video_0 ! queue ! h264parse ! flvmux ! fdsink name=videosink ",NULL); 
  g_assert (app->pipeline);

  bus = gst_pipeline_get_bus (GST_PIPELINE (app->pipeline));
  g_assert(bus);

  /* add watch for messages */
  gst_bus_add_watch (bus, (GstBusFunc) bus_message, app);

  /* get the appsrc */
    app->appsink = gst_bin_get_by_name (GST_BIN(app->pipeline), "audiosink");
    app->file_source = gst_bin_get_by_name (GST_BIN(app->pipeline), "file");
    printf("filedata = %s\n",fileData);
    g_object_set ( app->file_source, "location", fileData, NULL);

    app->video_sink = gst_bin_get_by_name (GST_BIN(app->pipeline), "videosink"); 
    g_object_set (app->appsink, "fd", audio_fd[1], NULL);
    g_object_set (app->video_sink, "fd", video_fd[1], NULL);
    gst_element_set_state (app->pipeline, GST_STATE_PLAYING);
   
  #ifdef DEBUG

    clock = gst_pipeline_get_clock(app->pipeline);
    timer = gst_clock_new_periodic_id  ( clock,
                                  gst_clock_get_time(clock),
                                1000000000);
    gst_clock_id_wait_async(timer,callBack,NULL,NULL);
  #endif
  /* this mainloop is stopped when we receive an error or EOS */
  g_main_loop_run (app->loop);

  GST_DEBUG ("stopping");

  gst_element_set_state (app->pipeline, GST_STATE_NULL);

  gst_object_unref (bus);
  g_main_loop_unref (app->loop);

  return 0;
}



