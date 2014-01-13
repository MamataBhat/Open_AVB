/*
 * Gstreamer AvbSrc plugin header file
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


#ifndef __GST_AVB_SRC_H__
#define __GST_AVB_SRC_H__

#include <gst/gst.h>
#include <gst/base/gstpushsrc.h>

G_BEGIN_DECLS


#define GST_TYPE_AVB_SRC \
  (gst_avb_src_get_type())
#define GST_AVB_SRC(obj) \
  (G_TYPE_CHECK_INSTANCE_CAST((obj),GST_TYPE_AVB_SRC,GstAvbSrc))
#define GST_AVB_SRC_CLASS(klass) \
  (G_TYPE_CHECK_CLASS_CAST((klass),GST_TYPE_AVB_SRC,GstAvbSrcClass))
#define GST_IS_AVB_SRC(obj) \
  (G_TYPE_CHECK_INSTANCE_TYPE((obj),GST_TYPE_AVB_SRC))
#define GST_IS_AVB_SRC_CLASS(klass) \
  (G_TYPE_CHECK_CLASS_TYPE((klass),GST_TYPE_AVB_SRC))


typedef struct _GstAvbSrc GstAvbSrc;
typedef struct _GstAvbSrcClass GstAvbSrcClass;

/**
 * GstAvbSrc:
 *
 * Opaque #GstAvbSrc data structure.
 */
struct _GstAvbSrc {
  GstPushSrc element;

  /*< private >*/
  /* new_fd is copied to fd on READY->PAUSED */
  gint new_fd;
  gint write_fd;
  gchar *interface;
  gint  mediaType;
  gint  dataSync;
  gint dataStart;
  
  /* fd and flag indicating whether fd is seekable */
  gint fd;
  gboolean seekable_fd;
  guint64 size;

  /* poll timeout */
  guint64 timeout;

  gchar *uri;

  GstPoll *fdset;

  gulong curoffset; /* current offset in file */
};

struct _GstAvbSrcClass {
  GstPushSrcClass parent_class;

  /* signals */
  void (*timeout) (GstElement *element);
};

G_GNUC_INTERNAL GType gst_avb_src_get_type(void);

G_END_DECLS

#endif /* __GST_AVB_SRC_H__ */
