/*
 * Copyright (c) 2017 Ronald S. Bultje <rsbultje@gmail.com>
 * Copyright (c) 2017 Ashish Pratap Singh <ashk43712@gmail.com>
 *
 * This file is part of FFmpeg.
 *
 * FFmpeg is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * FFmpeg is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with FFmpeg; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA
 */

/**
 * @file
 * Calculate the VMAF between two input videos.
 */

#include <inttypes.h>
#include <pthread.h>
#include <string.h>
#include <libvmaf.h>
#include "libavutil/avstring.h"
#include "libavutil/opt.h"
#include "libavutil/pixdesc.h"
#include "avfilter.h"
#include "dualinput.h"
#include "drawutils.h"
#include "formats.h"
#include "internal.h"
#include "video.h"

typedef struct VMAFContext {
    const AVClass *class;
    FFDualInputContext dinput;
    char *format;
    int width;
    int height;
    double vmaf_score;
    pthread_t vmaf_thread;
    pthread_mutex_t lock;
    pthread_cond_t cond;
    int eof;
    AVFrame *gmain;
    AVFrame *gref;
    int frame_set;
    char *model_path;
    char *log_path;
    char *log_fmt;
    int disable_clip;
    int disable_avx;
    int enable_transform;
    int phone_model;
    int psnr;
    int ssim;
    int ms_ssim;
    char *pool;
} VMAFContext;

#define OFFSET(x) offsetof(VMAFContext, x)
#define FLAGS AV_OPT_FLAG_FILTERING_PARAM|AV_OPT_FLAG_VIDEO_PARAM

static const AVOption vmaf_options[] = {
    {"model_path",  "Set the model to be used for computing vmaf.",            OFFSET(model_path), AV_OPT_TYPE_STRING, {.str="/usr/local/share/model/vmaf_v0.6.1.pkl"}, 0, 1, FLAGS},
    {"log_path",  "Set the file path to be used to store logs.",            OFFSET(log_path), AV_OPT_TYPE_STRING, {.str=NULL}, 0, 1, FLAGS},
    {"log_fmt",  "Set the format of the log (xml or json).",            OFFSET(log_fmt), AV_OPT_TYPE_STRING, {.str=NULL}, 0, 1, FLAGS},
    {"disable_clip",  "Disables clip for computing vmaf.",            OFFSET(disable_clip), AV_OPT_TYPE_BOOL, {.i64=0}, 0, 1, FLAGS},
    {"disable avx",  "Disables avx for computing vmaf.",            OFFSET(disable_avx), AV_OPT_TYPE_BOOL, {.i64=0}, 0, 1, FLAGS},
    {"enable_transform",  "Enables transform for computing vmaf.",            OFFSET(enable_transform), AV_OPT_TYPE_BOOL, {.i64=0}, 0, 1, FLAGS},
    {"phone_model",  "Invokes the phone model that will generate higher VMAF scores.",            OFFSET(phone_model), AV_OPT_TYPE_BOOL, {.i64=0}, 0, 1, FLAGS},
    {"psnr",  "Enables computing psnr along with vmaf.",            OFFSET(psnr), AV_OPT_TYPE_BOOL, {.i64=0}, 0, 1, FLAGS},
    {"ssim",  "Enables computing ssim along with vmaf.",            OFFSET(ssim), AV_OPT_TYPE_BOOL, {.i64=0}, 0, 1, FLAGS},
    {"ms_ssim",  "Enables computing ms-ssim along with vmaf.",            OFFSET(ms_ssim), AV_OPT_TYPE_BOOL, {.i64=0}, 0, 1, FLAGS},
    {"pool",  "Set the pool method to be used for computing vmaf.",            OFFSET(pool), AV_OPT_TYPE_STRING, {.str=NULL}, 0, 1, FLAGS},
    { NULL }
};

AVFILTER_DEFINE_CLASS(vmaf);

static int read_frame_8bit(float *ref_data, float *main_data, float *temp_data,
                           int stride, double *score, void *ctx){

    VMAFContext *s = (VMAFContext *) ctx;

    pthread_mutex_lock(&s->lock);

    while (!s->frame_set && !s->eof) {
        pthread_cond_wait(&s->cond, &s->lock);
    }

    if (s->frame_set) {

        int ref_stride = s->gref->linesize[0];
        int main_stride = s->gmain->linesize[0];

        uint8_t *ptr = s->gref->data[0];
        float *ptr1 = ref_data;

        int h = s->height;
        int w = s->width;

        int i,j;

        for (i = 0; i < h; i++) {
            for ( j = 0; j < w; j++) {
                ptr1[j] = (float)ptr[j];
            }
            ptr += ref_stride/sizeof(*ptr);
            ptr1 += stride/sizeof(*ptr1);
        }

        ptr = s->gmain->data[0];
        ptr1 = main_data;

        for (i = 0; i < h; i++) {
            for (j = 0; j < w; j++) {
                ptr1[j] = (float)ptr[j];
            }
            ptr += main_stride/sizeof(*ptr);
            ptr1 += stride/sizeof(*ptr1);
        }
    }

    int ret = !s->frame_set;

    s->frame_set = 0;

    pthread_cond_signal(&s->cond);
    pthread_mutex_unlock(&s->lock);

    if (ret) {
        return 2;
    }

    return 0;
}

static int read_frame_10bit(float *ref_data, float *main_data, float *temp_data,
                            int stride, double *score, void *ctx){

    VMAFContext *s = (VMAFContext *) ctx;

    pthread_mutex_lock(&s->lock);

    while (!s->frame_set && !s->eof) {
        pthread_cond_wait(&s->cond, &s->lock);
    }

    if (s->frame_set) {

        int ref_stride = s->gref->linesize[0];
        int main_stride = s->gmain->linesize[0];

        uint16_t *ptr = s->gref->data[0];
        float *ptr1 = ref_data;

        int h = s->height;
        int w = s->width;

        int i,j;

        for (i = 0; i < h; i++) {
            for ( j = 0; j < w; j++) {
                ptr1[j] = (float)ptr[j];
            }
            ptr += ref_stride/sizeof(*ptr);
            ptr1 += stride/sizeof(*ptr1);
        }

        ptr = s->gmain->data[0];
        ptr1 = main_data;

        for (i = 0; i < h; i++) {
            for (j = 0; j < w; j++) {
                ptr1[j] = (float)ptr[j];
            }
            ptr += main_stride/sizeof(*ptr);
            ptr1 += stride/sizeof(*ptr1);
        }
    }

    int ret = !s->frame_set;

    s->frame_set = 0;

    pthread_cond_signal(&s->cond);
    pthread_mutex_unlock(&s->lock);

    if (ret) {
        return 2;
    }

    return 0;
}

static void compute_vmaf_score(VMAFContext *s)
{
    int (*read_frame)(float *ref_data, float *main_data, int stride,
                      double *score, void *ctx);

    if (strcmp(s->format, "yuv420p") || strcmp(s->format, "yuv422p") ||
        strcmp(s->format, "yuv444p")) {
        read_frame = read_frame_8bit;
    } else {
        read_frame = read_frame_10bit;
    }

    s->vmaf_score = compute_vmaf(s->format, s->width, s->height, read_frame, s,
                                 s->model_path, s->log_path, s->log_fmt,
                                 s->disable_clip, s->disable_avx,
                                 s->enable_transform, s->phone_model,
                                 s->psnr, s->ssim, s->ms_ssim, s->pool);
}

static void *call_vmaf(void *ctx)
{
    VMAFContext *s = (VMAFContext *) ctx;
    compute_vmaf_score(s);
    pthread_exit(NULL);
}

static AVFrame *do_vmaf(AVFilterContext *ctx, AVFrame *main, const AVFrame *ref)
{
    VMAFContext *s = ctx->priv;

    pthread_mutex_lock(&s->lock);

    while (s->frame_set != 0) {
        pthread_cond_wait(&s->cond, &s->lock);
    }

    av_frame_ref(s->gref, ref);
    av_frame_ref(s->gmain, main);

    s->frame_set = 1;

    pthread_cond_signal(&s->cond);
    pthread_mutex_unlock(&s->lock);

    return main;
}

static av_cold int init(AVFilterContext *ctx)
{
    VMAFContext *s = ctx->priv;

    s->gref = av_frame_alloc();
    s->gmain = av_frame_alloc();

    pthread_mutex_init(&s->lock, NULL);
    pthread_cond_init (&s->cond, NULL);

    s->dinput.process = do_vmaf;
    return 0;
}

static int query_formats(AVFilterContext *ctx)
{
    static const enum AVPixelFormat pix_fmts[] = {
        AV_PIX_FMT_YUV444P, AV_PIX_FMT_YUV422P, AV_PIX_FMT_YUV420P,
        AV_PIX_FMT_YUV444P10LE, AV_PIX_FMT_YUV422P10LE, AV_PIX_FMT_YUV420P10LE,
        AV_PIX_FMT_NONE
    };

    AVFilterFormats *fmts_list = ff_make_format_list(pix_fmts);
    if (!fmts_list)
        return AVERROR(ENOMEM);
    return ff_set_common_formats(ctx, fmts_list);
}


static int config_input_ref(AVFilterLink *inlink)
{
    const AVPixFmtDescriptor *desc = av_pix_fmt_desc_get(inlink->format);
    AVFilterContext *ctx  = inlink->dst;
    VMAFContext *s = ctx->priv;

    if (ctx->inputs[0]->w != ctx->inputs[1]->w ||
        ctx->inputs[0]->h != ctx->inputs[1]->h) {
        av_log(ctx, AV_LOG_ERROR, "Width and height of input videos must be
               same.\n");
        return AVERROR(EINVAL);
    }
    if (ctx->inputs[0]->format != ctx->inputs[1]->format) {
        av_log(ctx, AV_LOG_ERROR, "Inputs must be of same pixel format.\n");
        return AVERROR(EINVAL);
    }
    if (!(s->model_path)) {
        av_log(ctx, AV_LOG_ERROR, "No model specified.\n");
        return AVERROR(EINVAL);
    }

    s->format = av_get_pix_fmt_name(ctx->inputs[0]->format);
    s->width = ctx->inputs[0]->w;
    s->height = ctx->inputs[0]->h;

    int rc = pthread_create(&s->vmaf_thread, NULL, call_vmaf, (void *)s);
    if (rc) {
        av_log(ctx, AV_LOG_ERROR, "Thread creation failed.\n");
        return AVERROR(EINVAL);
    }

    return 0;
}


static int config_output(AVFilterLink *outlink)
{
    AVFilterContext *ctx = outlink->src;
    VMAFContext *s = ctx->priv;
    AVFilterLink *mainlink = ctx->inputs[0];
    int ret;

    outlink->w = mainlink->w;
    outlink->h = mainlink->h;
    outlink->time_base = mainlink->time_base;
    outlink->sample_aspect_ratio = mainlink->sample_aspect_ratio;
    outlink->frame_rate = mainlink->frame_rate;
    if ((ret = ff_dualinput_init(ctx, &s->dinput)) < 0)
        return ret;

    return 0;
}

static int filter_frame(AVFilterLink *inlink, AVFrame *inpicref)
{
    VMAFContext *s = inlink->dst->priv;
    return ff_dualinput_filter_frame(&s->dinput, inlink, inpicref);
}

static int request_frame(AVFilterLink *outlink)
{
    VMAFContext *s = outlink->src->priv;
    return ff_dualinput_request_frame(&s->dinput, outlink);
}

static av_cold void uninit(AVFilterContext *ctx)
{
    VMAFContext *s = ctx->priv;

    ff_dualinput_uninit(&s->dinput);

    pthread_mutex_lock(&s->lock);
    s->eof = 1;
    pthread_cond_signal(&s->cond);
    pthread_mutex_unlock(&s->lock);

    pthread_join(s->vmaf_thread, NULL);

    av_frame_free(&s->gref);
    av_frame_free(&s->gmain);

    pthread_mutex_destroy(&s->lock);
    pthread_cond_destroy(&s->cond);

    av_log(ctx, AV_LOG_INFO, "VMAF score: %f\n",s->vmaf_score);
}

static const AVFilterPad vmaf_inputs[] = {
    {
        .name         = "main",
        .type         = AVMEDIA_TYPE_VIDEO,
        .filter_frame = filter_frame,
    },{
        .name         = "reference",
        .type         = AVMEDIA_TYPE_VIDEO,
        .filter_frame = filter_frame,
        .config_props = config_input_ref,
    },
    { NULL }
};

static const AVFilterPad vmaf_outputs[] = {
    {
        .name          = "default",
        .type          = AVMEDIA_TYPE_VIDEO,
        .config_props  = config_output,
        .request_frame = request_frame,
    },
    { NULL }
};

AVFilter ff_vf_vmaf = {
    .name          = "vmaf",
    .description   = NULL_IF_CONFIG_SMALL("Calculate the VMAF between two video streams."),
    .init          = init,
    .uninit        = uninit,
    .query_formats = query_formats,
    .priv_size     = sizeof(VMAFContext),
    .priv_class    = &vmaf_class,
    .inputs        = vmaf_inputs,
    .outputs       = vmaf_outputs,
};
