#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>

#include <mpv/client.h>
#include <mpv/render.h>

static void require(bool condition, const char *message)
{
    if (!condition) {
        fprintf(stderr, "%s\n", message);
        exit(1);
    }
}

int main(int argc, char **argv)
{
    require(argc == 2, "expected the path to the test video");
    mpv_handle *mpv = mpv_create();
    require(mpv != NULL, "could not create mpv");
    require(mpv_set_option_string(mpv, "vo", "libmpv") >= 0, "could not select libmpv VO");
    require(mpv_set_option_string(mpv, "pause", "yes") >= 0, "could not start paused");
    require(mpv_initialize(mpv) >= 0, "could not initialize mpv");

    mpv_render_context *render = NULL;
    mpv_render_param init[] = {
        {MPV_RENDER_PARAM_API_TYPE, MPV_RENDER_API_TYPE_SW}, {0}
    };
    require(mpv_render_context_create(&render, mpv, init) >= 0,
            "could not create software render context");
    require(mpv_command(mpv, (const char *[]){"loadfile", argv[1], NULL}) >= 0,
            "could not load the test video");

    bool queued = false;
    for (int attempt = 0; attempt < 100; attempt++) {
        mpv_render_frame_info frame = {0};
        mpv_render_context_get_info(render, (mpv_render_param){
            MPV_RENDER_PARAM_NEXT_FRAME_INFO, &frame});
        if (frame.flags & MPV_RENDER_FRAME_INFO_PRESENT) {
            queued = true;
            break;
        }
        mpv_wait_event(mpv, 0.01);
    }
    require(queued, "no frame was queued");

    // Leave the frame pending beyond the VO's presentation deadline, like a
    // GUI whose render thread is briefly stalled while playback is paused.
    bool idle = false;
    for (int attempt = 0; attempt < 100; attempt++) {
        if (mpv_wait_event(mpv, 0.5)->event_id == MPV_EVENT_NONE) {
            idle = true;
            break;
        }
    }
    require(idle, "paused playback did not settle");
    require(mpv_render_context_update(render) & MPV_RENDER_UPDATE_FRAME,
            "timed-out paused frame was no longer advertised for rendering");
    mpv_render_frame_info frame = {0};
    mpv_render_context_get_info(render, (mpv_render_param){
        MPV_RENDER_PARAM_NEXT_FRAME_INFO, &frame});
    require(frame.flags == (MPV_RENDER_FRAME_INFO_PRESENT | MPV_RENDER_FRAME_INFO_REDRAW),
            "delayed frame did not report itself as a redraw");

    int size[2] = {32, 32};
    size_t stride = 128;
    _Alignas(64) unsigned char pixels[32 * 128] = {0};
    mpv_render_param params[] = {
        {MPV_RENDER_PARAM_SW_SIZE, size},
        {MPV_RENDER_PARAM_SW_FORMAT, "rgb0"},
        {MPV_RENDER_PARAM_SW_STRIDE, &stride},
        {MPV_RENDER_PARAM_SW_POINTER, pixels},
        {0},
    };
    require(mpv_render_context_render(render, params) >= 0, "could not render delayed frame");
    bool colored = false;
    for (size_t index = 0; index < sizeof(pixels); index += 4) {
        if (pixels[index] || pixels[index + 1] || pixels[index + 2]) {
            colored = true;
            break;
        }
    }
    require(colored, "delayed frame did not contain the decoded video");
    require(!(mpv_render_context_update(render) & MPV_RENDER_UPDATE_FRAME),
            "rendered frame stayed pending");

    mpv_render_context_free(render);
    mpv_terminate_destroy(mpv);
    return 0;
}
