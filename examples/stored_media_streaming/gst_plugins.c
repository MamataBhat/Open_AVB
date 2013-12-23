/*
 * Author: Symphony teleca corporation,Bangalore.
 * Date: Nov- 10 - 2013
 *
 * File Name: gst_plugins.c
 *
 * 
 * This application contstructs pipeline using the gstreamer to decode the audio and video data received from the IGB
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

#include <stdio.h>
#include <gst/gst.h>
#include <gst/app/gstappsrc.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <pthread.h>

#define BUFF_SIZE 	1024

extern int read_data_from_queue( void* ptr);
extern int read_data_from_video_queue( void* ptr);
extern pthread_mutex_t pipe_data_lock;


typedef struct {
	GstPipeline *pipeline;
	GstAppSrc *src;
        GstAppSrc *videosrc;
	GMainLoop *loop;
	guint sourceid;
        guint video_sourceid; 
	FILE *file;
} gst_app_t;


static gst_app_t gst_app;



extern int audio_sync_fd[2];

extern int sync_data; 
int audio_input_data[1400];
char pipe_buff[1024];
char stop_buffer[1024];  
char video_start_buf[1024];
char video_stop_buf[1024]; 



extern int video_count;
extern int audio_count;
static gboolean callBack (GstClock * clock, GstClockTime time,GstClockID id, gpointer user_data) 
{

   
   printf("audio = %d\n",audio_count);
   printf("video = %d\n",video_count);
   video_count = 0; 
   audio_count = 0;
  
   
}


static gboolean read_data(gst_app_t *app)
{
	GstBuffer *buffer;
	guint8 *ptr;
	gint size;
	GstFlowReturn ret;
        size = read_data_from_queue(audio_input_data);
        if(size > 0 ) {
        
		ptr = g_malloc(size);
		g_assert(ptr);

	        memcpy(ptr,audio_input_data,size);
		
		g_debug("size=%d",size);

		if(size == 0) {
			ret = gst_app_src_end_of_stream(app->src);
			g_debug("eos returned %d at %d\n", ret, __LINE__);
			return FALSE;
		}
	 
		buffer = gst_buffer_new();
		buffer = gst_buffer_new_wrapped(ptr, size);
	
			 
		ret = gst_app_src_push_buffer(app->src, buffer);
	 
		if(ret !=  GST_FLOW_OK){
			g_debug("push buffer returned %d for %d bytes \n", ret, size);
			return FALSE;
		}
	 
		if(size != BUFF_SIZE){
			ret = gst_app_src_end_of_stream(app->src);
			g_debug("eos returned %d at %d\n", ret, __LINE__);
			return FALSE;
		}
        }
 	return TRUE;
}


int video_data[1400];
static gboolean read_video_data(gst_app_t *app)
{
	GstBuffer *buffer;
	guint8 *ptr;
	gint size;
	GstFlowReturn ret;

	
	size = read_data_from_video_queue(video_data);
        if(size > 0) {
		ptr = g_malloc(size);
		g_assert(ptr);
                memcpy( ptr,video_data,size);
		g_debug("size=%d",size);
		
		if(size == 0) {
			ret = gst_app_src_end_of_stream(app->videosrc);
			g_debug("eos returned %d at %d\n", ret, __LINE__);
			return FALSE;
		}
	 
		buffer = gst_buffer_new();
		buffer = gst_buffer_new_wrapped(ptr, size);
			 
		ret = gst_app_src_push_buffer(app->videosrc, buffer);
	 
		if(ret !=  GST_FLOW_OK){
			g_debug("push buffer returned %d for %d bytes \n", ret, size);
			return FALSE;
		}
	 
		if(size != BUFF_SIZE){
			ret = gst_app_src_end_of_stream(app->videosrc);
			g_debug("eos returned %d at %d\n", ret, __LINE__);
			return FALSE;
		}
        } 
	return TRUE;
}

static void start_feed (GstElement * pipeline, guint size, gst_app_t *app)
{
       	
	if (app->sourceid == 0) {
	
               pipe_buff[0] = 1;
               if(sync_data == 1){
		       pthread_mutex_lock(&(pipe_data_lock));
		       write(audio_sync_fd[1],pipe_buff,512);
		       pthread_mutex_unlock(&(pipe_data_lock));
		       printf("start feed audio\n");
               }
	       app->sourceid = g_idle_add ((GSourceFunc) read_data, app);
	}
}

static void stop_feed (GstElement * pipeline, gst_app_t *app)
{
	
        if (app->sourceid != 0) {
         
 	
                
                stop_buffer[0] = 0;
                if(sync_data == 1) {
		        pthread_mutex_lock(&(pipe_data_lock));
		        write(audio_sync_fd[1],stop_buffer,512);
		        pthread_mutex_unlock(&(pipe_data_lock));
		        printf("stop feed audio\n");
		}
                g_source_remove (app->sourceid);
		app->sourceid = 0;
	}
}


static void start_feed_video (GstElement * pipeline, guint size, gst_app_t *app)
{
	 
         if (app->video_sourceid == 0) {
		video_start_buf[0] = 3;
                if(sync_data == 1) {
		        pthread_mutex_lock(&(pipe_data_lock));
		        write(audio_sync_fd[1],video_start_buf,512);
		        pthread_mutex_unlock(&(pipe_data_lock));
		        printf("start feed video\n");
                }
		app->video_sourceid = g_idle_add ((GSourceFunc) read_video_data, app);
	}
}

static void stop_feed_video(GstElement * pipeline, gst_app_t *app)
{
	 
          if (app->video_sourceid != 0) {
		video_stop_buf[0] = 2;
                if(sync_data == 1) {
		        pthread_mutex_lock(&(pipe_data_lock));
		        write(audio_sync_fd[1],video_stop_buf,512);                             
		        pthread_mutex_unlock(&(pipe_data_lock));
		        printf("stop feed video\n");
		}
                g_source_remove (app->video_sourceid);
		app->video_sourceid = 0;
	}
}


 
static void on_pad_added (GstElement *element,GstPad *pad,gpointer data)
{
	GstPad *sinkpad;
	GstElement *demuxer = (GstElement *) data;

	sinkpad = gst_element_get_static_pad (demuxer, "sink");
	gst_pad_link (pad, sinkpad);
	gst_object_unref (sinkpad);
}

 
static gboolean bus_callback(GstBus *bus, GstMessage *message, gpointer *ptr)
{
	gst_app_t *app = (gst_app_t*)ptr;

	switch(GST_MESSAGE_TYPE(message)) {
	case GST_MESSAGE_ERROR: {
		gchar *debug;
		GError *err;

		gst_message_parse_error(message, &err, &debug);
		g_print("Error %s\n", err->message);
		g_error_free(err);
		g_free(debug);
		g_main_loop_quit(app->loop);
	}
	break;

	case GST_MESSAGE_WARNING: {
		gchar *debug;
		GError *err;
		gchar *name;

		gst_message_parse_warning(message, &err, &debug);
		g_print("Warning %s\nDebug %s\n", err->message, debug);

		name = GST_MESSAGE_SRC_NAME(message);

		g_print("Name of src %s\n", name ? name : "nil");
		g_error_free(err);
		g_free(debug);
	}
	break;

	case GST_MESSAGE_EOS:
		g_print("End of stream\n");
		g_main_loop_quit(app->loop);
		break;

	case GST_MESSAGE_STATE_CHANGED:
		break;
	
	default:
		g_print("got message %s\n", \
		gst_message_type_get_name (GST_MESSAGE_TYPE (message)));
		break;
	}

	return TRUE;
}
int gstreamer_main(char* input)
{
	gst_app_t *app = &gst_app;
	GstBus *bus;
	GstStateChangeReturn state_ret;
        int result;  
        GstClockID timer;
        GstClock *clock;  
        
 
	gst_init(NULL, NULL);
	app->pipeline = (GstPipeline*)gst_pipeline_new("mypipeline");
	bus = gst_pipeline_get_bus(app->pipeline);
	gst_bus_add_watch(bus, (GstBusFunc)bus_callback, app);
	gst_object_unref(bus);
        
        if(strcmp (input,"AUDIO_VIDEO") == 0) {        
        printf("audio and video pipeline\n");
        app->pipeline = gst_parse_launch("appsrc name=videosource ! flvdemux ! h264parse ! queue ! vaapidecode ! autovideosink  appsrc name=audiosource ! tsdemux ! queue ! decodebin ! audioconvert ! audioresample ! autoaudiosink",NULL);

       app->src = gst_bin_get_by_name (GST_BIN(app->pipeline), "audiosource");
       g_assert(app->src);
       g_signal_connect (app->src, "need-data", G_CALLBACK (start_feed), app);
       g_signal_connect (app->src, "enough-data", G_CALLBACK (stop_feed), app);
       
    
        app->videosrc = gst_bin_get_by_name (GST_BIN(app->pipeline), "videosource");
        g_assert(app->videosrc);
        g_signal_connect (app->videosrc, "need-data", G_CALLBACK (start_feed_video), app);
        g_signal_connect (app->videosrc, "enough-data", G_CALLBACK (stop_feed_video), app);
        }
        else if(strcmp (input,"AUDIO") == 0) {  
            printf("audio pipeline\n");     
            app->pipeline = gst_parse_launch("appsrc name=audiosource ! tsdemux ! queue ! decodebin ! audioconvert ! audioresample ! autoaudiosink",NULL);
            app->src = gst_bin_get_by_name (GST_BIN(app->pipeline), "audiosource");
       g_assert(app->src);
       g_signal_connect (app->src, "need-data", G_CALLBACK (start_feed), app);
       g_signal_connect (app->src, "enough-data", G_CALLBACK (stop_feed), app);


        }
        else if(strcmp (input,"VIDEO") == 0) {       
          printf("video pipeline \n");       
	  app->pipeline = gst_parse_launch("appsrc name=videosource ! flvdemux ! h264parse ! queue ! vaapidecode ! autovideosink",NULL);
          app->videosrc = gst_bin_get_by_name (GST_BIN(app->pipeline), "videosource");
          g_assert(app->videosrc);
          g_signal_connect (app->videosrc, "need-data", G_CALLBACK (start_feed_video), app);
          g_signal_connect (app->videosrc, "enough-data", G_CALLBACK (stop_feed_video), app);
        }     
       
       

       	state_ret = gst_element_set_state((GstElement*)app->pipeline, GST_STATE_PLAYING);
	g_warning("set state returned %d\n", state_ret);
#if 1 
     clock = gst_pipeline_get_clock(app->pipeline);
  timer = gst_clock_new_periodic_id  ( clock,
                                  gst_clock_get_time(clock),
                                1000000000);
  gst_clock_id_wait_async(timer,callBack,NULL,NULL);
#endif	
app->loop = g_main_loop_new(NULL, FALSE);
 
	g_main_loop_run(app->loop);
 
	state_ret = gst_element_set_state((GstElement*)app->pipeline, GST_STATE_NULL);
	g_warning("set state null returned %d\n", state_ret);
 
	return 0;
}
