#include "include/thumblr_linux/thumblr_linux_plugin.h"

#include <extractor.h>

#include <flutter_linux/flutter_linux.h>
#include <gtk/gtk.h>
#include <sys/utsname.h>

#include <cstring>

#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavformat/avio.h>
#include <libswscale/swscale.h>

#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>

static void SaveFrame(AVFrame *pFrame, int width, int height, int iFrame);

#define THUMBLR_LINUX_PLUGIN(obj) \
  (G_TYPE_CHECK_INSTANCE_CAST((obj), thumblr_linux_plugin_get_type(), \
                              ThumblrLinuxPlugin))

struct _ThumblrLinuxPlugin {
  GObject parent_instance;
};

G_DEFINE_TYPE(ThumblrLinuxPlugin, thumblr_linux_plugin, g_object_get_type())

static void SaveFrame(AVFrame *pFrame, int width, int height, int iFrame)
{
  FILE *pFile;
  char szFilename[32];
  int y;

  // Open file
  sprintf(szFilename, "frame%d.ppm", iFrame);
  pFile = fopen(szFilename, "wb");
  if (pFile == NULL)
    return;

  // Write header
  fprintf(pFile, "P6\n%d %d\n255\n", width, height);

  // Write pixel data
  for (y = 0; y < height; y++)
    fwrite(pFrame->data[0] + y * pFrame->linesize[0], 1, width * 3, pFile);

  // Close file
  fclose(pFile);
}

// Called when a method call is received from Flutter.
static void thumblr_linux_plugin_handle_method_call(
    ThumblrLinuxPlugin* self,
    FlMethodCall* method_call) {
  g_autoptr(FlMethodResponse) response = nullptr;

  const gchar* method = fl_method_call_get_name(method_call);

  if (strcmp(method, "generateThumbnail") == 0)
  {
    AVFormatContext *pFormatCtx;
    int i, videoStream;
    AVCodecContext *pCodecCtx;
    AVCodec *pCodec;
    AVFrame *pFrame;
    AVFrame *pFrameRGB;
    AVPacket packet;
    int frameFinished;
    int numBytes;
    uint8_t *buffer;

    FlValue *args = fl_method_call_get_args(method_call);
    FlValue *filePath = fl_value_lookup_string(args, "filePath");
    FlValue *position = fl_value_lookup_string(args, "position");

    if (avformat_open_input(&pFormatCtx, filePath, NULL, 0, NULL) != 0)
      response = FL_METHOD_RESPONSE(fl_method_error_response_new("-1", "Could not open file", nullptr));

    // Retrieve stream information
    if (avformat_find_stream_info(pFormatCtx) < 0)
      response = FL_METHOD_RESPONSE(fl_method_error_response_new("-1", "Could not find stream information", nullptr));

    // Dump information about file onto standard error
    av_dump_format(pFormatCtx, 0, filePath, false);

    // Find the first video stream
    videoStream = -1;
    for (i = 0; i < pFormatCtx->nb_streams; i++)
      if (pFormatCtx->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_VIDEO)
      {
        videoStream = i;
        break;
      }
    if (videoStream == -1)
      response = FL_METHOD_RESPONSE(fl_method_error_response_new("-1", "Could not find a video stream", nullptr));

    // Get a pointer to the codec context for the video stream
    pCodecCtx = pFormatCtx->streams[videoStream]->AVStream::codecpar;

    // Find the decoder for the video stream
    pCodec = avcodec_find_decoder(pCodecCtx->codec_id);

    if (pCodec == NULL)
      response = FL_METHOD_RESPONSE(fl_method_error_response_new("-1", "Codec not found", nullptr));

    // Open codec
    if (avcodec_open(pCodecCtx, pCodec) < 0)
      response = FL_METHOD_RESPONSE(fl_method_error_response_new("-1", "Could not open codec", nullptr));

    // Hack to correct wrong frame rates that seem to be generated by some codecs
    if (pCodecCtx->time_base.num > 1000 && pCodecCtx->time_base.den == 1)
      pCodecCtx->time_base.den = 1000;

    // Allocate video frame
    pFrame = avcodec_alloc_frame();

    // Allocate an AVFrame structure
    pFrameRGB = avcodec_alloc_frame();
    if (pFrameRGB == NULL)
      response = FL_METHOD_RESPONSE(fl_method_error_response_new("-1", "Could not allocate an AVFrame structure", nullptr));

    // Determine required buffer size and allocate buffer
    numBytes = atk_image_get_image_size(AV_PIX_FMT_RGB24, pCodecCtx->width,
                                        pCodecCtx->height);

    buffer = malloc(numBytes);

    // Assign appropriate parts of buffer to image planes in pFrameRGB
    av_image_fill_arrays((AVPicture *)pFrameRGB, buffer, AV_PIX_FMT_RGB24,
                   pCodecCtx->width, pCodecCtx->height);

    // Read frames and save first frame to disk
    i = 0;
    while (av_read_frame(pFormatCtx, &packet) >= 0)
    {
      // Is this a packet from the video stream?
      if (packet.stream_index == videoStream)
      {
        // Decode video frame
        avcodec_decode_video(pCodecCtx, pFrame, &frameFinished,
                             packet.data, packet.size);

        // Did we get a video frame?
        if (frameFinished)
        {
          static struct SwsContext *img_convert_ctx;

#if 0
				// Older removed code
                // Convert the image from its native format to RGB swscale
                img_convert((AVPicture *)pFrameRGB, AV_PIX_FMT_RGB24, 
                    (AVPicture*)pFrame, pCodecCtx->pix_fmt, pCodecCtx->width, 
                    pCodecCtx->height);
				
				// function template, for reference
				int sws_scale(struct SwsContext *context, uint8_t* src[], int srcStride[], int srcSliceY,
							  int srcSliceH, uint8_t* dst[], int dstStride[]);
#endif
          // Convert the image into YUV format that SDL uses
          if (img_convert_ctx == NULL)
          {
            int w = pCodecCtx->width;
            int h = pCodecCtx->height;

            img_convert_ctx = sws_getContext(w, h,
                                             pCodecCtx->pix_fmt,
                                             w, h, AV_PIX_FMT_RGB24, SWS_BICUBIC,
                                             NULL, NULL, NULL);
            if (img_convert_ctx == NULL)
            {
              fprintf(stderr, "Cannot initialize the conversion context!\n");
              exit(1);
            }
          }
          int ret = sws_scale(img_convert_ctx, pFrame->data, pFrame->linesize, 0,
                              pCodecCtx->height, pFrameRGB->data, pFrameRGB->linesize);
#if 0 // this use to be true, as of 1/2009, but apparently it is no longer true in 3/2009
				if(ret) {
					fprintf(stderr, "SWS_Scale failed [%d]!\n", ret);
					exit(-1);
				}
#endif
          // Save the frame to disk
          if (i++ <= 1)
            SaveFrame(pFrameRGB, pCodecCtx->width, pCodecCtx->height, i);
        }
      }

      // Free the packet that was allocated by av_read_frame
      av_packet_unref(&packet);
    }

    FlValue *res = fl_value_new_map();
    res.fl_value_set(FlValue(pFrameRGB->data) from_bytes(pFrameRGB->data), fl_value_new_string("data"));

    // Free the RGB image
    free(buffer);
    av_free(pFrameRGB);

    // Free the YUV frame
    av_free(pFrame);

    // Close the codec
    avcodec_close(pCodecCtx);

    // Close the video file
    avformat_close_input(pFormatCtx);

    response = FL_METHOD_RESPONSE(fl_method_success_response_new(res));
  }
  else
  {
    response = FL_METHOD_RESPONSE(fl_method_not_implemented_response_new());
  }

  fl_method_call_respond(method_call, response, nullptr);
}

static void thumblr_linux_plugin_dispose(GObject* object) {
  G_OBJECT_CLASS(thumblr_linux_plugin_parent_class)->dispose(object);
}

static void thumblr_linux_plugin_class_init(ThumblrLinuxPluginClass* klass) {
  G_OBJECT_CLASS(klass)->dispose = thumblr_linux_plugin_dispose;
}

static void thumblr_linux_plugin_init(ThumblrLinuxPlugin* self) {}

static void method_call_cb(FlMethodChannel* channel, FlMethodCall* method_call,
                           gpointer user_data) {
  ThumblrLinuxPlugin* plugin = THUMBLR_LINUX_PLUGIN(user_data);
  thumblr_linux_plugin_handle_method_call(plugin, method_call);
}

void thumblr_linux_plugin_register_with_registrar(FlPluginRegistrar* registrar) {
  ThumblrLinuxPlugin* plugin = THUMBLR_LINUX_PLUGIN(
      g_object_new(thumblr_linux_plugin_get_type(), nullptr));

  g_autoptr(FlStandardMethodCodec) codec = fl_standard_method_codec_new();
  g_autoptr(FlMethodChannel) channel =
      fl_method_channel_new(fl_plugin_registrar_get_messenger(registrar),
                            "thumblr_linux",
                            FL_METHOD_CODEC(codec));
  fl_method_channel_set_method_call_handler(channel, method_call_cb,
                                            g_object_ref(plugin),
                                            g_object_unref);

  g_object_unref(plugin);
}
