#include "test_utils.h"
#include "video/out/gpu/utils.h"
#include "video/out/filter_kernels.h"

static void test_padded_scaler_lut(void)
{
    struct filter_kernel kernel = *mp_find_filter_kernel(SCALER_LANCZOS);
    kernel.w = *mp_find_filter_window(kernel.window);
    const int sizes[] = {2, 4, 6, 8, 0};
    assert_true(mp_init_filter(&kernel, sizes, 1.0));
    assert_int_equal(kernel.size, 6);

    // Six taps occupy two RGBA texels. The whole vector texture, including
    // spare channels, must be initialized independently of allocator contents.
    float lut[256 * 8];
    for (int n = 0; n < MP_ARRAY_SIZE(lut); n++)
        lut[n] = NAN;
    mp_compute_lut(&kernel, 256, 8, lut);
    for (int row = 0; row < 256; row++) {
        float sum = 0;
        for (int tap = 0; tap < kernel.size; tap++) {
            assert_true(isfinite(lut[row * 8 + tap]));
            sum += lut[row * 8 + tap];
        }
        assert_float_equal(sum, 1.0, 4 * FLT_EPSILON);
        for (int padding = kernel.size; padding < 8; padding++)
            assert_true(lut[row * 8 + padding] == 0.0f);
    }
}

int main(void)
{
    test_padded_scaler_lut();
    float x;

    x = gl_video_scale_ambient_lux(16.0, 64.0, 2.40, 1.961, 16.0);
    assert_float_equal(x, 2.40f, FLT_EPSILON);

    x = gl_video_scale_ambient_lux(16.0, 64.0, 2.40, 1.961, 64.0);
    assert_float_equal(x, 1.961f, FLT_EPSILON);

    x = gl_video_scale_ambient_lux(16.0, 64.0, 1.961, 2.40, 64.0);
    assert_float_equal(x, 2.40f, FLT_EPSILON);

    x = gl_video_scale_ambient_lux(16.0, 64.0, 2.40, 1.961, 0.0);
    assert_float_equal(x, 2.40f, FLT_EPSILON);

    // 32 corresponds to the midpoint after converting lux to the log10 scale
    x = gl_video_scale_ambient_lux(16.0, 64.0, 2.40, 1.961, 32.0);
    float mid_gamma = (2.40 - 1.961) / 2 + 1.961;
    assert_float_equal(x, mid_gamma, FLT_EPSILON);
    return 0;
}
