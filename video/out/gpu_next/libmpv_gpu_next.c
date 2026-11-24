/*
 * This file is part of mpv.
 *
 * mpv is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * mpv is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with mpv.  If not, see <http://www.gnu.org/licenses/>.
 */

// The gpu-next render backend of the libmpv render API. It drives the same
// renderer as vo_gpu_next (gpu_next/video.c); the only difference is who owns
// the target. An OpenGL caller hands over a framebuffer to draw into through a
// libplacebo swapchain wrapped around it, while Vulkan and Direct3D 11 callers
// hand over an image they also present, so on those paths there is no
// swapchain at all.

#include <libplacebo/config.h>
#include <libplacebo/swapchain.h>

#include "config.h"
#include "common/common.h"
#include "common/msg.h"
#include "misc/bstr.h"
#include "mpv/render_gl.h"
#include "video/hwdec.h"
#include "video/out/gpu/context.h"
#include "video/out/gpu/libmpv_gpu.h"
#include "video/out/libmpv.h"
#include "video/out/placebo/ra_pl.h"
#include "video/out/placebo/utils.h"
#include "video/out/vo.h"
#include "context.h"
#include "video.h"

#if HAVE_GL && defined(PL_HAVE_OPENGL)
#define MP_LIBMPV_OPENGL 1
#include <libplacebo/opengl.h>
#include "video/out/opengl/ra_gl.h"
#endif

// Rendering into a target somebody else owns needs libplacebo's backend and
// nothing of mpv's own: no native context, no swapchain, and so neither the
// Vulkan loader nor the shader translator those contexts link against. Hence
// libplacebo's feature alone rather than mpv's -- which is what lets Windows
// have both of these without a Vulkan SDK on the machine.
// The headers are asked for as well as the feature. libplacebo can report a
// backend whose headers are nowhere on this build's include path -- its Vulkan
// ones live under its own 3rdparty and are not handed to whoever links it --
// and a backend that cannot be compiled is not one that can be offered.
#if defined(PL_HAVE_D3D11) && __has_include(<d3d11.h>)
#define MP_LIBMPV_D3D11 1
#ifndef COBJMACROS
#define COBJMACROS
#endif
#include <d3d11.h>
#include <libplacebo/d3d11.h>
#include "mpv/render_d3d11.h"
#endif

#if defined(PL_HAVE_VULKAN) && __has_include(<vulkan/vulkan.h>)
#define MP_LIBMPV_VULKAN 1
#include <libplacebo/vulkan.h>
#include "mpv/render_vk.h"
#endif

#if defined(MP_LIBMPV_OPENGL) || defined(MP_LIBMPV_VULKAN) || defined(MP_LIBMPV_D3D11)

struct priv {
    struct mp_log *log;
    struct gpu_ctx *gpu_ctx;
    struct mppl_video *renderer;
    struct mp_hwdec_devices *hwdec_devs;
    bool has_vo;
    struct mp_image_params rendered_target_params;
    struct pl_hdr_metadata rendered_hdr;
    bool rendered_hdr_valid;
    struct mp_image_params target_params;

    // OpenGL: the caller's GL context, and the swapchain over its framebuffer.
    struct libmpv_gpu_context *gl;

    // Vulkan and Direct3D 11: the caller's device, imported, and the target
    // it handed over last, wrapped once and again whenever it hands over a
    // different one. A Qt-style renderer keeps handing back the same image
    // until the item resizes, and wrapping one twice is undefined (Vulkan) or
    // takes a reference each time (Direct3D 11).
    pl_log pllog;
    pl_tex target;
    uint64_t target_id;
    int target_w, target_h, target_format;
#if defined(MP_LIBMPV_VULKAN)
    pl_vulkan vulkan;
    // Handing the image back requires a semaphore. A timeline one can be
    // signalled every frame without anybody waiting on it, which is what is
    // wanted here: ordering comes from both sides submitting to the same
    // queue, and libplacebo's own layout barrier carries the dependency.
    VkSemaphore hold_sem;
    uint64_t hold_value;
#endif
#if defined(MP_LIBMPV_D3D11)
    pl_d3d11 d3d11;
#endif
};

#if defined(MP_LIBMPV_OPENGL)
struct native_resource_entry {
    const char *name;
    size_t size;
};

static const struct native_resource_entry native_resource_map[] = {
    [MPV_RENDER_PARAM_X11_DISPLAY] = {
        .name = "x11",
    },
    [MPV_RENDER_PARAM_WL_DISPLAY] = {
        .name = "wl",
    },
    [MPV_RENDER_PARAM_DRM_DRAW_SURFACE_SIZE] = {
        .name = "drm_draw_surface_size",
        .size = sizeof(mpv_opengl_drm_draw_surface_size),
    },
    [MPV_RENDER_PARAM_DRM_DISPLAY_V2] = {
        .name = "drm_params_v2",
        .size = sizeof(mpv_opengl_drm_params_v2),
    },
};

// The hwdec interops find the window system through these.
static void add_native_resources(struct priv *p, struct ra *ra,
                                 mpv_render_param *params)
{
    for (int n = 0; params && params[n].type; n++) {
        if (params[n].type <= 0 ||
            params[n].type >= MP_ARRAY_SIZE(native_resource_map) ||
            !native_resource_map[params[n].type].name)
            continue;

        const struct native_resource_entry *entry =
            &native_resource_map[params[n].type];
        void *data = params[n].data;
        if (entry->size)
            data = talloc_memdup(p, data, entry->size);
        ra_add_native_resource(ra, entry->name, data);
    }
}

static int init_opengl(struct render_backend *ctx, struct priv *p,
                       mpv_render_param *params)
{
    p->gl = talloc_zero(p, struct libmpv_gpu_context);
    *p->gl = (struct libmpv_gpu_context) {
        .global = ctx->global,
        .log = ctx->log,
        .fns = &libmpv_gpu_context_gl,
    };
    int err = p->gl->fns->init(p->gl, params);
    if (err < 0)
        return err;
    add_native_resources(p, p->gl->ra_ctx->ra, params);

    // A software or pre-3.3 GL stack fails here: libplacebo rejects it.
    p->gpu_ctx = gpu_ctx_create(ctx->log, p->gl->ra_ctx, false);
    if (!p->gpu_ctx || !gpu_ctx_create_swapchain(p->gpu_ctx, 1))
        return MPV_ERROR_UNSUPPORTED;
    return 0;
}
#endif

#if defined(MP_LIBMPV_VULKAN) || defined(MP_LIBMPV_D3D11)
// An imported device presents nothing, so there is no swapchain behind this.
// It exists because the renderer reaches for one to ask about dithering depth
// and target colorimetry, and every one of those questions has the same answer
// here: the caller already decided.
static const struct ra_swapchain_fns external_swapchain_fns = {0};

// The renderer reaches for a ra_ctx for its timers and its hwdec interops. An
// imported device has neither a native context nor a window, so it gets one
// over the device itself.
static int init_external_gpu_ctx(struct render_backend *ctx, struct priv *p,
                                 pl_log pllog, pl_gpu gpu)
{
    struct ra_ctx *ra_ctx = talloc_zero(p, struct ra_ctx);
    ra_ctx->log = ctx->log;
    ra_ctx->global = ctx->global;
    ra_ctx->opts = (struct ra_ctx_opts) { .allow_sw = true };
    ra_ctx->ra = ra_create_pl(gpu, ctx->log);
    if (!ra_ctx->ra)
        return MPV_ERROR_UNSUPPORTED;
    ra_ctx->swapchain = talloc_zero(ra_ctx, struct ra_swapchain);
    ra_ctx->swapchain->ctx = ra_ctx;
    ra_ctx->swapchain->fns = &external_swapchain_fns;

    p->gpu_ctx = talloc_zero(p, struct gpu_ctx);
    *p->gpu_ctx = (struct gpu_ctx) {
        .log = ctx->log,
        .ra_ctx = ra_ctx,
        .pllog = pllog,
        .gpu = gpu,
    };
    return 0;
}
#endif

#if defined(MP_LIBMPV_D3D11)
static int init_d3d11(struct render_backend *ctx, struct priv *p,
                      mpv_render_param *params, pl_log pllog)
{
    mpv_d3d11_init_params *init =
        get_mpv_render_param(params, MPV_RENDER_PARAM_D3D11_INIT_PARAMS, NULL);
    if (!init || !init->device)
        return MPV_ERROR_INVALID_PARAMETER;

    p->d3d11 = pl_d3d11_create(pllog, pl_d3d11_params(
        .device = (ID3D11Device *) init->device,
    ));
    if (!p->d3d11) {
        MP_FATAL(ctx, "Failed taking the caller's Direct3D 11 device.\n");
        return MPV_ERROR_UNSUPPORTED;
    }
    return init_external_gpu_ctx(ctx, p, pllog, p->d3d11->gpu);
}

static bool d3d11_fbo_desc(mpv_d3d11_fbo *fbo, D3D11_TEXTURE2D_DESC *desc)
{
    if (!fbo || !fbo->tex)
        return false;
    ID3D11Texture2D_GetDesc((ID3D11Texture2D *) fbo->tex, desc);
    return desc->Width > 0 && desc->Height > 0;
}

static int wrap_d3d11_target(struct priv *p, mpv_render_param *params)
{
    mpv_d3d11_fbo *fbo = get_mpv_render_param(params, MPV_RENDER_PARAM_D3D11_FBO, NULL);
    D3D11_TEXTURE2D_DESC desc;
    if (!d3d11_fbo_desc(fbo, &desc))
        return MPV_ERROR_INVALID_PARAMETER;

    if (p->target && p->target_id == (uintptr_t) fbo->tex &&
        p->target_w == desc.Width && p->target_h == desc.Height)
        return 0;

    pl_gpu gpu = p->gpu_ctx->gpu;
    pl_tex_destroy(gpu, &p->target);
    p->target = pl_d3d11_wrap(gpu, pl_d3d11_wrap_params(
        .tex = (ID3D11Resource *) fbo->tex,
    ));
    if (!p->target) {
        MP_ERR(p, "Failed wrapping the caller's Direct3D 11 texture.\n");
        return MPV_ERROR_UNSUPPORTED;
    }
    p->target_id = (uintptr_t) fbo->tex;
    p->target_w = desc.Width;
    p->target_h = desc.Height;
    return 0;
}
#endif

#if defined(MP_LIBMPV_VULKAN)
static int init_vulkan(struct render_backend *ctx, struct priv *p,
                       mpv_render_param *params, pl_log pllog)
{
    mpv_vulkan_init_params *init =
        get_mpv_render_param(params, MPV_RENDER_PARAM_VULKAN_INIT_PARAMS, NULL);
    if (!init || !init->instance || !init->physical_device || !init->device)
        return MPV_ERROR_INVALID_PARAMETER;

    p->vulkan = pl_vulkan_import(pllog, pl_vulkan_import_params(
        .instance = (VkInstance) init->instance,
        .get_proc_addr = (PFN_vkGetInstanceProcAddr) init->get_instance_proc_addr,
        .phys_device = (VkPhysicalDevice) init->physical_device,
        .device = (VkDevice) init->device,
        .extensions = init->device_extensions,
        .num_extensions = init->num_device_extensions,
        .features = init->device_features,
        // One queue, the family's first. libplacebo picks within a family and
        // cannot be told an index, so this is the only arrangement in which
        // both sides provably submit to the same queue -- which is what lets
        // the layout barrier order the frame without a semaphore.
        .queue_graphics = {
            .index = init->queue_family_index,
            .count = 1,
        },
    ));
    if (!p->vulkan) {
        MP_FATAL(ctx, "Failed importing the caller's Vulkan device.\n");
        return MPV_ERROR_UNSUPPORTED;
    }

    int err = init_external_gpu_ctx(ctx, p, pllog, p->vulkan->gpu);
    if (err < 0)
        return err;

    p->hold_sem = pl_vulkan_sem_create(p->vulkan->gpu, pl_vulkan_sem_params(
        .type = VK_SEMAPHORE_TYPE_TIMELINE,
    ));
    if (!p->hold_sem) {
        MP_FATAL(ctx, "Failed creating the image handover semaphore.\n");
        return MPV_ERROR_UNSUPPORTED;
    }
    return 0;
}

static int wrap_vulkan_target(struct priv *p, mpv_render_param *params)
{
    mpv_vulkan_image *image =
        get_mpv_render_param(params, MPV_RENDER_PARAM_VULKAN_IMAGE, NULL);
    if (!image || image->w < 1 || image->h < 1)
        return MPV_ERROR_INVALID_PARAMETER;

    pl_gpu gpu = p->gpu_ctx->gpu;
    if (!p->target || p->target_id != image->image ||
        p->target_format != image->format ||
        p->target_w != image->w || p->target_h != image->h)
    {
        pl_tex_destroy(gpu, &p->target);
        p->target = pl_vulkan_wrap(gpu, pl_vulkan_wrap_params(
            .image = (VkImage) (uintptr_t) image->image,
            .width = image->w,
            .height = image->h,
            .format = (VkFormat) image->format,
            .usage = (VkImageUsageFlags) image->usage,
        ));
        if (!p->target) {
            MP_ERR(p, "Failed wrapping the caller's VkImage.\n");
            return MPV_ERROR_UNSUPPORTED;
        }
        p->target_id = image->image;
        p->target_format = image->format;
        p->target_w = image->w;
        p->target_h = image->h;
    }

    // A wrapped image starts out held by the caller. Hand it over for the
    // duration of the frame, saying which layout it arrives in; taking it
    // back afterwards is what leaves it sampleable.
    pl_vulkan_release_ex(gpu, pl_vulkan_release_params(
        .tex = p->target,
        .layout = (VkImageLayout) image->layout,
        .qf = VK_QUEUE_FAMILY_IGNORED,
    ));
    return 0;
}
#endif

static void destroy(struct render_backend *ctx)
{
    struct priv *p = ctx->priv;
    if (!p)
        return;

    if (p->renderer)
        mppl_video_uninit(p->renderer);
    hwdec_devices_destroy(p->hwdec_devs);
    ctx->hwdec_devs = NULL;

    if (p->gl) {
        gpu_ctx_destroy(&p->gpu_ctx);
        p->gl->fns->destroy(p->gl);
        talloc_free(p->gl->priv);
    } else {
        // The wrapper goes and its reference with it; the caller's image and
        // device stay, because neither was ever ours.
        if (p->gpu_ctx) {
            pl_tex_destroy(p->gpu_ctx->gpu, &p->target);
            // ra_pl frees itself, as with vulkan/context.c.
            struct ra *ra = p->gpu_ctx->ra_ctx->ra;
            ra->fns->destroy(ra);
        }
#if defined(MP_LIBMPV_VULKAN)
        if (p->vulkan) {
            pl_vulkan_sem_destroy(p->vulkan->gpu, &p->hold_sem);
            pl_vulkan_destroy(&p->vulkan);
        }
#endif
#if defined(MP_LIBMPV_D3D11)
        pl_d3d11_destroy(&p->d3d11);
#endif
        pl_log_destroy(&p->pllog);
    }

    talloc_free(p);
    ctx->priv = NULL;
}

static int init(struct render_backend *ctx, mpv_render_param *params)
{
    char *api = get_mpv_render_param(params, MPV_RENDER_PARAM_API_TYPE, NULL);
    if (!api)
        return MPV_ERROR_INVALID_PARAMETER;

    ctx->priv = talloc_zero(NULL, struct priv);
    struct priv *p = ctx->priv;
    p->log = ctx->log;

    int err = MPV_ERROR_NOT_IMPLEMENTED;
#if defined(MP_LIBMPV_OPENGL)
    if (strcmp(api, MPV_RENDER_API_TYPE_OPENGL) == 0)
        err = init_opengl(ctx, p, params);
#endif
#if defined(MP_LIBMPV_VULKAN) || defined(MP_LIBMPV_D3D11)
    bool vulkan = strcmp(api, MPV_RENDER_API_TYPE_VULKAN) == 0;
    bool d3d11 = strcmp(api, MPV_RENDER_API_TYPE_D3D11) == 0;
    if (vulkan || d3d11) {
        p->pllog = mppl_log_create(p, p->log);
        err = MPV_ERROR_UNSUPPORTED;
#if defined(MP_LIBMPV_VULKAN)
        if (vulkan && p->pllog)
            err = init_vulkan(ctx, p, params, p->pllog);
#endif
#if defined(MP_LIBMPV_D3D11)
        if (d3d11 && p->pllog)
            err = init_d3d11(ctx, p, params, p->pllog);
#endif
    }
#endif
    // Anything else must fail too: mpv_render_context_create treats 0 as an
    // initialised backend and goes on to use it.
    if (err < 0 || !p->gpu_ctx || !p->gpu_ctx->gpu) {
        destroy(ctx);
        return err < 0 ? err : MPV_ERROR_UNSUPPORTED;
    }

    p->renderer = mppl_video_init(ctx->log, ctx->global, p->gpu_ctx);
    p->hwdec_devs = hwdec_devices_create();
    ctx->hwdec_devs = p->hwdec_devs;
    ctx->driver_caps = VO_CAP_ROTATE90 | VO_CAP_FILM_GRAIN | VO_CAP_VFLIP;
    mppl_video_init_hwdecs(p->renderer, p->hwdec_devs, true);
    return 0;
}

static bool check_format(struct render_backend *ctx, int imgfmt)
{
    struct priv *p = ctx->priv;
    return mppl_video_check_format(p->renderer, imgfmt);
}

static int set_parameter(struct render_backend *ctx, mpv_render_param param)
{
    struct priv *p = ctx->priv;

    switch (param.type) {
    case MPV_RENDER_PARAM_ICC_PROFILE: {
        mpv_byte_array *data = param.data;
        mppl_video_set_icc_profile(p->renderer, (bstr){data->data, data->size});
        return 0;
    }
    default:
        return MPV_ERROR_NOT_IMPLEMENTED;
    }
}

static void reconfig(struct render_backend *ctx, struct mp_image_params *params)
{
    struct priv *p = ctx->priv;
    mppl_video_config(p->renderer, params);
}

static void reset(struct render_backend *ctx)
{
    struct priv *p = ctx->priv;
    mppl_video_reset(p->renderer);
}

static void screenshot(struct render_backend *ctx, struct vo_frame *frame,
                       struct voctrl_screenshot *args)
{
    struct priv *p = ctx->priv;
    mppl_video_screenshot(p->renderer, args);
}

static void perfdata(struct render_backend *ctx, struct voctrl_performance_data *out)
{
    struct priv *p = ctx->priv;
    mppl_video_perfdata(p->renderer, out);
}

static struct mp_image *get_image(struct render_backend *ctx, int imgfmt,
                                  int w, int h, int stride_align, int flags)
{
    struct priv *p = ctx->priv;
    return mppl_video_get_image(p->renderer, imgfmt, w, h, stride_align, flags);
}

static void update_external(struct render_backend *ctx, struct vo *vo)
{
    struct priv *p = ctx->priv;
    p->has_vo = vo != NULL;
    mppl_video_set_osd_source(p->renderer, vo ? vo->osd : NULL);
    if (vo) {
        mppl_video_reload_options(p->renderer);
        mppl_video_configure_queue(p->renderer, vo);
    }
}

static void resize(struct render_backend *ctx, struct mp_rect *src,
                   struct mp_rect *dst, struct mp_osd_res *osd)
{
    struct priv *p = ctx->priv;
    mppl_video_resize(p->renderer, *src, *dst, *osd);
}

static int get_target_size(struct render_backend *ctx, mpv_render_param *params,
                           int *out_w, int *out_h)
{
#if defined(MP_LIBMPV_D3D11)
    mpv_d3d11_fbo *d3d11_fbo = get_mpv_render_param(params, MPV_RENDER_PARAM_D3D11_FBO, NULL);
    if (d3d11_fbo) {
        D3D11_TEXTURE2D_DESC desc;
        if (!d3d11_fbo_desc(d3d11_fbo, &desc))
            return MPV_ERROR_INVALID_PARAMETER;
        *out_w = desc.Width;
        *out_h = desc.Height;
        return 0;
    }
#endif
#if defined(MP_LIBMPV_VULKAN)
    mpv_vulkan_image *image = get_mpv_render_param(params, MPV_RENDER_PARAM_VULKAN_IMAGE, NULL);
    if (image) {
        if (image->w < 1 || image->h < 1)
            return MPV_ERROR_INVALID_PARAMETER;
        *out_w = image->w;
        *out_h = image->h;
        return 0;
    }
#endif
    mpv_opengl_fbo *fbo = get_mpv_render_param(params, MPV_RENDER_PARAM_OPENGL_FBO, NULL);
    if (!fbo || fbo->w < 1 || fbo->h < 1)
        return MPV_ERROR_INVALID_PARAMETER;
    *out_w = fbo->w;
    *out_h = fbo->h;
    return 0;
}

// Point the OpenGL swapchain at the caller's framebuffer and start a frame.
static int start_gl_frame(struct priv *p, mpv_render_param *params,
                          struct pl_swapchain_frame *swframe)
{
#if defined(MP_LIBMPV_OPENGL)
    mpv_opengl_fbo *fbo = get_mpv_render_param(params, MPV_RENDER_PARAM_OPENGL_FBO, NULL);
    if (!fbo || fbo->w < 1 || fbo->h < 1)
        return MPV_ERROR_INVALID_PARAMETER;

    int err = p->gl->fns->start_frame(p->gl, params);
    if (err < 0)
        return err;

    // The render API flag requests compensation for bottom-left GL targets;
    // libplacebo wants the target origin instead.
    bool flip = GET_MPV_RENDER_PARAM(params, MPV_RENDER_PARAM_FLIP_Y, int, 0);
    pl_swapchain sw = p->gpu_ctx->swapchain;
    pl_opengl_swapchain_update_fb(sw, &(struct pl_opengl_framebuffer) {
        .id = fbo->fbo,
        .flipped = !flip,
    });
    if (!gpu_ctx_resize(p->gpu_ctx, fbo->w, fbo->h) ||
        !pl_swapchain_start_frame(sw, swframe))
        return MPV_ERROR_GENERIC;
    return 0;
#else
    return MPV_ERROR_NOT_IMPLEMENTED;
#endif
}

static int render(struct render_backend *ctx, mpv_render_param *params,
                  struct vo_frame *frame)
{
    struct priv *p = ctx->priv;
    if (!p->has_vo)
        return MPV_ERROR_UNINITIALIZED;

    mppl_video_begin_frame(p->renderer, frame, (struct pl_color_space){0}, 0);
    struct pl_color_space hint = {0};
    enum mppl_video_hint hint_mode = mppl_video_get_target_hint(p->renderer, frame, &hint);

    struct pl_swapchain_frame swframe;
    int err = MPV_ERROR_NOT_IMPLEMENTED;
    if (p->gl) {
        pl_swapchain_colorspace_hint(p->gpu_ctx->swapchain,
                                     hint_mode == MPPL_VIDEO_HINT_SET ? &hint : NULL);
        err = start_gl_frame(p, params, &swframe);
    } else {
#if defined(MP_LIBMPV_VULKAN)
        if (p->vulkan)
            err = wrap_vulkan_target(p, params);
#endif
#if defined(MP_LIBMPV_D3D11)
        // Nothing to hand over: one device, one immediate context, and the
        // caller is not using it while this runs.
        if (p->d3d11)
            err = wrap_d3d11_target(p, params);
#endif
        // The caller owns the target and its presentation, so there is no
        // swapchain to ask and nothing to wait for. Colorimetry is left at the
        // sRGB default and shaped by the target options, which is the only
        // thing that can describe an image mpv never created.
        swframe = (struct pl_swapchain_frame) {
            .fbo = p->target,
            .color_repr = pl_color_repr_rgb,
            .color_space = pl_color_space_srgb,
        };
        hint_mode = MPPL_VIDEO_HINT_NONE;
    }
    if (err < 0) {
        mppl_video_skip_frame(p->renderer, frame);
        if (p->gl)
            p->gl->fns->done_frame(p->gl, frame->display_synced);
        return err;
    }

    struct mp_image_params target_params;
    bool ok = mppl_video_render_frame(p->renderer, frame, &swframe, 0,
                                      hint_mode == MPPL_VIDEO_HINT_NONE ? NULL : &hint,
                                      &target_params);

    pl_gpu gpu = p->gpu_ctx->gpu;
    if (p->gl) {
        if (!pl_swapchain_submit_frame(p->gpu_ctx->swapchain)) {
            MP_ERR(ctx, "Failed presenting frame!\n");
            ok = false;
        }
        p->gl->fns->done_frame(p->gl, frame->display_synced);
    } else {
#if defined(MP_LIBMPV_VULKAN)
        if (p->vulkan) {
            // Give the image back in the layout the caller will sample it
            // from. Flushing below puts the work in the queue before whatever
            // the caller submits next.
            pl_vulkan_hold_ex(gpu, pl_vulkan_hold_params(
                .tex = p->target,
                .layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                .qf = VK_QUEUE_FAMILY_IGNORED,
                .semaphore = {
                    .sem = p->hold_sem,
                    .value = ++p->hold_value,
                },
            ));
        }
#endif
        // Direct3D 11: the immediate context orders this against whatever the
        // caller draws next, so flushing is the whole of the handover.
        pl_gpu_flush(gpu);
    }
    if (pl_gpu_is_failed(gpu))
        ok = false;

    if (ok) {
        p->rendered_target_params = target_params;
        p->rendered_hdr_valid = mppl_video_get_hdr_metadata(p->renderer, &p->rendered_hdr);
    }
    return ok ? 0 : MPV_ERROR_GENERIC;
}

static void publish_frame(struct render_backend *ctx, struct vo *vo)
{
    struct priv *p = ctx->priv;
    mp_mutex_lock(&vo->params_mutex);
    p->target_params = p->rendered_target_params;
    vo->target_params = &p->target_params;
    if (vo->params) {
        vo->has_peak_detect_values = p->rendered_hdr_valid;
        if (p->rendered_hdr_valid)
            vo->params->color.hdr = p->rendered_hdr;
    }
    mp_mutex_unlock(&vo->params_mutex);
}

const struct render_backend_fns render_backend_gpu_next = {
    .name = "gpu-next",
    .init = init,
    .check_format = check_format,
    .set_parameter = set_parameter,
    .reconfig = reconfig,
    .reset = reset,
    .screenshot = screenshot,
    .perfdata = perfdata,
    .get_image = get_image,
    .update_external = update_external,
    .resize = resize,
    .get_target_size = get_target_size,
    .render = render,
    .publish_frame = publish_frame,
    .destroy = destroy,
};

#else

static int init_unavailable(struct render_backend *ctx, mpv_render_param *params)
{
    return MPV_ERROR_NOT_IMPLEMENTED;
}

const struct render_backend_fns render_backend_gpu_next = {
    .name = "gpu-next",
    .init = init_unavailable,
};

#endif
