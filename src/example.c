#include "libuvc/libuvc.h"
#include <stdio.h>
#include <unistd.h>

#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libswscale/swscale.h>
#include <SDL2/SDL.h>

#define SCREEN_WIDTH  1280
#define SCREEN_HEIGHT 720

// 定义一个用户事件类型
#define SDL_USER_UPDATE_TEXTURE (SDL_USEREVENT + 1)

typedef struct {
    AVCodecContext *codec_ctx;
    SDL_Texture *texture;
    SDL_Renderer *renderer;
    struct SwsContext *sws_ctx;
} H264Context;

typedef struct {
    const uint8_t* y_plane;
    const uint8_t* u_plane;
    const uint8_t* v_plane;
    int y_pitch;
    int uv_pitch;
} YUVPlanes;

void decode_and_render(H264Context *h264_ctx, const uint8_t *data, int size) {
    AVPacket pkt;
    av_init_packet(&pkt);
    pkt.data = (uint8_t *)data;
    pkt.size = size;

    if (avcodec_send_packet(h264_ctx->codec_ctx, &pkt) < 0) {
        fprintf(stderr, "Error sending packet to decoder\n");
        return;
    }

    AVFrame *frame = av_frame_alloc();
    while (avcodec_receive_frame(h264_ctx->codec_ctx, frame) >= 0) {
        // 将解码的帧转换为YUV格式
        uint8_t *y_plane = (uint8_t *)malloc(SCREEN_WIDTH * SCREEN_HEIGHT);
        uint8_t *u_plane = (uint8_t *)malloc(SCREEN_WIDTH * SCREEN_HEIGHT / 4);
        uint8_t *v_plane = (uint8_t *)malloc(SCREEN_WIDTH * SCREEN_HEIGHT / 4);

        uint8_t *dst_data[3] = {y_plane, u_plane, v_plane};
        int dst_linesize[3] = {SCREEN_WIDTH, SCREEN_WIDTH / 2, SCREEN_WIDTH / 2};

        sws_scale(h264_ctx->sws_ctx, (const uint8_t * const *)frame->data, frame->linesize, 0,
                  h264_ctx->codec_ctx->height, dst_data, dst_linesize);


        YUVPlanes* planes = (YUVPlanes*)malloc(sizeof(YUVPlanes));
        planes->y_plane = y_plane;
        planes->u_plane = u_plane;
        planes->v_plane = v_plane;
        planes->y_pitch = SCREEN_WIDTH;
        planes->uv_pitch = SCREEN_WIDTH / 2;

        SDL_Event event;
        event.type = SDL_USER_UPDATE_TEXTURE;
        event.user.code = 0;
        event.user.data1 = h264_ctx;
        event.user.data2 = planes;
        SDL_PushEvent(&event);
    }
    av_frame_free(&frame);
}

/* This callback function runs once per frame. Use it to perform any
 * quick processing you need, or have it put the frame into your application's
 * input queue. If this function takes too long, you'll start losing frames. */
#define NALU_TYPE_MASK 0x1F
#define NALU_TYPE_SPS 7
#define NALU_TYPE_PPS 8
#define NALU_TYPE_IDR 5
int find_key_frame = 0;

void cb(uvc_frame_t *frame, void *ptr)
{
  uvc_frame_t *bgr;
  uvc_error_t ret;
  H264Context *h264_ctx = (H264Context *)ptr;

  static int jpeg_count = 0;
  static const char *H264_FILE = "output.h264";
  // static const char *MJPEG_FILE = ".jpeg";
  // char filename[16];

  /* We'll convert the image from YUV/JPEG to BGR, so allocate space */
  bgr = uvc_allocate_frame(frame->width * frame->height * 3);
  if (!bgr) {
    printf("unable to allocate bgr frame!\n");
    return;
  }

  // printf("callback! frame_format = %d, width = %d, height = %d, length = %lu, ptr = %p\n",
  //        frame->frame_format, frame->width, frame->height, frame->data_bytes, ptr);

  uint8_t *byte_data = (uint8_t *)frame->data;
  if (byte_data[0] != 0x00 || byte_data[1] != 0x00 || byte_data[2] != 0x00 || byte_data[3] != 0x01) {
    printf("invalid h264 data found %ld\n", frame->data_bytes);
  }
  else {
    if (!find_key_frame) {
      uint8_t nalu_type = byte_data[4] & NALU_TYPE_MASK;
      printf("nalu_type is %d\n", nalu_type);
      if (nalu_type == NALU_TYPE_SPS)
      {
        find_key_frame = 1;
        decode_and_render(h264_ctx, frame->data, frame->data_bytes);
      }
    }
    else {
      decode_and_render(h264_ctx, frame->data, frame->data_bytes);
    }
  }

  if (frame->sequence % 30 == 0) {
    printf(" * got image %u\n",  frame->sequence);
  }

  uvc_free_frame(bgr);
}

// 处理用户事件的回调
void handle_user_event(SDL_Event* event) {
    // 从 event->user.data1 和 event->user.data2 获取数据
    H264Context *h264_ctx = (H264Context *)event->user.data1;
    YUVPlanes *planes = (YUVPlanes *)event->user.data2;

      SDL_UpdateYUVTexture(h264_ctx->texture, NULL,
                            planes->y_plane, planes->y_pitch,
                            planes->u_plane, planes->uv_pitch,
                            planes->v_plane, planes->uv_pitch);

      // 渲染图像
      SDL_RenderClear(h264_ctx->renderer);
      SDL_RenderCopy(h264_ctx->renderer, h264_ctx->texture, NULL, NULL);
      SDL_RenderPresent(h264_ctx->renderer);

      free(planes->y_plane);
      free(planes->u_plane);
      free(planes->v_plane);
      free(planes);
}

// 主线程中的事件循环
void main_loop() {
    SDL_Event event;
    while (SDL_WaitEvent(&event)) {
        if (event.type == SDL_USER_UPDATE_TEXTURE) {
            handle_user_event(&event);
        } else if (event.type == SDL_QUIT) {
            break;
        }
        // 处理其他事件
    }
}

int main(int argc, char **argv) {

  // 初始化FFmpeg和SDL
  avformat_network_init();

  SDL_Init(SDL_INIT_VIDEO);

  SDL_Window *window = SDL_CreateWindow("H.264 Player",
                                        SDL_WINDOWPOS_UNDEFINED,
                                        SDL_WINDOWPOS_UNDEFINED,
                                        SCREEN_WIDTH, SCREEN_HEIGHT,
                                        0);

  SDL_Renderer *renderer = SDL_CreateRenderer(window, -1, 0);
  SDL_Texture *texture = SDL_CreateTexture(renderer,
                                            SDL_PIXELFORMAT_YV12,
                                            SDL_TEXTUREACCESS_STREAMING,
                                            SCREEN_WIDTH, SCREEN_HEIGHT);

  // 初始化H.264解码器
  AVCodec *codec = avcodec_find_decoder(AV_CODEC_ID_H264);
  AVCodecContext *codec_ctx = avcodec_alloc_context3(codec);
  codec_ctx->flags |= AV_CODEC_FLAG_LOW_DELAY;
  codec_ctx->flags2 |= AV_CODEC_FLAG2_FAST;
  codec_ctx->thread_count = 1;
  avcodec_open2(codec_ctx, codec, NULL);
  codec_ctx->width = 1920;
  codec_ctx->height = 1080;
  codec_ctx->pix_fmt = AV_PIX_FMT_YUV420P;
  struct SwsContext *sws_ctx = sws_getContext(codec_ctx->width, codec_ctx->height, codec_ctx->pix_fmt,
                                SCREEN_WIDTH, SCREEN_HEIGHT, AV_PIX_FMT_YUV420P,
                                SWS_BILINEAR, NULL, NULL, NULL);
  H264Context h264_ctx = {
      .codec_ctx = codec_ctx,
      .texture = texture,
      .renderer = renderer,
      .sws_ctx = sws_ctx,
  };

  uvc_context_t *ctx;
  uvc_device_t *dev;
  uvc_device_handle_t *devh;
  uvc_stream_ctrl_t ctrl;
  uvc_error_t res;

  /* Initialize a UVC service context. Libuvc will set up its own libusb
   * context. Replace NULL with a libusb_context pointer to run libuvc
   * from an existing libusb context. */
  res = uvc_init(&ctx, NULL);

  if (res < 0) {
    uvc_perror(res, "uvc_init");
    return res;
  }

  puts("UVC initialized");

  /* Locates the first attached UVC device, stores in dev */
  res = uvc_find_device(
      ctx, &dev,
      0, 0, NULL); /* filter devices: vendor_id, product_id, "serial_num" */

  if (res < 0) {
    uvc_perror(res, "uvc_find_device"); /* no devices found */
  } else {
    puts("Device found");

    /* Try to open the device: requires exclusive access */
    res = uvc_open(dev, &devh);

    if (res < 0) {
      uvc_perror(res, "uvc_open"); /* unable to open device */
    } else {
      puts("Device opened");

      /* Print out a message containing all the information that libuvc
       * knows about the device */
      uvc_print_diag(devh, stderr);

      const uvc_format_desc_t *format_desc = uvc_get_format_descs(devh);
      const uvc_frame_desc_t *frame_desc = format_desc->frame_descs;
      enum uvc_frame_format frame_format;
      int width = 640;
      int height = 480;
      int fps = 30;

      switch (format_desc->bDescriptorSubtype) {
      case UVC_VS_FORMAT_MJPEG:
        frame_format = UVC_COLOR_FORMAT_MJPEG;
        break;
      case UVC_VS_FORMAT_FRAME_BASED:
        frame_format = UVC_FRAME_FORMAT_H264;
        break;
      default:
        frame_format = UVC_FRAME_FORMAT_YUYV;
        break;
      }

      if (frame_desc) {
        width = frame_desc->wWidth;
        height = frame_desc->wHeight;
        fps = (int)(10000000.0 / frame_desc->dwDefaultFrameInterval + 0.5);
      }

      printf("\nFirst format: (%4s) %dx%d %dfps\n", format_desc->fourccFormat, width, height, fps);

      /* Try to negotiate first stream profile */
      res = uvc_get_stream_ctrl_format_size(
          devh, &ctrl, /* result stored in ctrl */
          frame_format,
          width, height, fps /* width, height, fps */
      );

      /* Print out the result */
      uvc_print_stream_ctrl(&ctrl, stderr);

      if (res < 0) {
        uvc_perror(res, "get_mode"); /* device doesn't provide a matching stream */
      } else {
        /* Start the video stream. The library will call user function cb:
         *   cb(frame, (void *) 12345)
         */
        res = uvc_start_streaming(devh, &ctrl, cb, &h264_ctx, 0);

        if (res < 0) {
          uvc_perror(res, "start_streaming"); /* unable to start stream */
        } else {
          puts("Streaming...");

          main_loop();

          /* End the stream. Blocks until last callback is serviced */
          uvc_stop_streaming(devh);
          puts("Done streaming.");
        }
      }

      /* Release our handle on the device */
      uvc_close(devh);
      puts("Device closed");
    }

    /* Release the device descriptor */
    uvc_unref_device(dev);
  }

  /* Close the UVC context. This closes and cleans up any existing device handles,
   * and it closes the libusb context if one was not provided. */
  uvc_exit(ctx);
  puts("UVC exited");

  sws_freeContext(h264_ctx.sws_ctx);
  avcodec_free_context(&codec_ctx);
  SDL_DestroyTexture(texture);
  SDL_DestroyRenderer(renderer);
  SDL_DestroyWindow(window);
  SDL_Quit();

  return 0;
}
