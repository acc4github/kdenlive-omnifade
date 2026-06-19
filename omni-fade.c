#include <stdlib.h>
#include <stdint.h>
#include <math.h>
#include <frei0r.h>

#define CURVE_LUT_SIZE     512
#define REFERENCE_WIDTH    1920.0f
#define SQUARE_SAMPLES     4
#define MIN_SCALE          0.01f

/* -------------------------------------------------------------------------
   Plugin instance data
   ------------------------------------------------------------------------- */
typedef struct {
    int width, height;
    float resolution_scale;

    float position;
    float speed_curve;
    float gentle_arrival;
    float zoom_strength;
    int arrival_zoom;          /* 0=Expand, 1=Static, 2=Shrink */
    int departure_zoom;
    float blur_strength;
    int invert;
    int fill_background;

    /* Content bounds – cached once per instance */
    int bounds_calculated;
    int out_min_x, out_min_y, out_max_x, out_max_y;
    int in_min_x,  in_min_y,  in_max_x,  in_max_y;
    uint32_t avg_out, avg_in;

    /* Fixed crop bounds for Shrink modes */
    int out_crop_min_x, out_crop_min_y, out_crop_max_x, out_crop_max_y;
    int in_crop_min_x,  in_crop_min_y,  in_crop_max_x,  in_crop_max_y;

    float curve_lut[CURVE_LUT_SIZE];
    float last_speed_curve;
} omni_fade_t;

/* Forward declarations */
static void build_curve_lut(omni_fade_t *inst);
static float curve_lookup(const float *lut, float t);
static float reversed_linear(const omni_fade_t *inst, float t);
static float get_progress(omni_fade_t *inst, float linear_p);
static float get_instant_speed(omni_fade_t *inst, float linear_p);

static void calculate_content_bounds(const uint32_t* buf, int w, int h,
                                     int* min_x, int* min_y, int* max_x, int* max_y);
static uint32_t compute_average_color(const uint32_t *buf, int w, int h,
                                      int min_x, int min_y, int max_x, int max_y);

/* -------------------------------------------------------------------------
   Frei0r interface
   ------------------------------------------------------------------------- */
int f0r_init(void) { return 1; }
void f0r_deinit(void) {}

void f0r_get_plugin_info(f0r_plugin_info_t *info) {
    info->name = "OmniFade";
    info->author = "acc4commissions and Grok (optimized v0.29)";
    info->plugin_type = F0R_PLUGIN_TYPE_MIXER2;
    info->color_model = F0R_COLOR_MODEL_PACKED32;
    info->frei0r_version = FREI0R_MAJOR_VERSION;
    info->major_version = 0;
    info->minor_version = 29;
    info->num_params = 9;
    info->explanation = "Ultra-fast OmniFade with reduced branching and unrolled blur.";
}

void f0r_get_param_info(f0r_param_info_t *info, int idx) {
    const char* names[] = {"position","speed_curve","gentle_arrival","zoom_strength",
                           "arrival_zoom","departure_zoom","blur_strength","invert","fill_background"};
    const char* expl[] = {"Fade position (progress)","Speed Curve (%)","Gentle Arrival (%)",
                          "Zoom Strength","Arrival Zoom (0=Expand,1=Static,2=Shrink)",
                          "Departure Zoom (0=Expand,1=Static,2=Shrink)","Blur Strength (%)",
                          "Invert clips","Fill Background (average)"};
    info->name = names[idx];
    info->explanation = expl[idx];
    info->type = (idx == 7 || idx == 8) ? F0R_PARAM_BOOL : F0R_PARAM_DOUBLE;
}

f0r_instance_t f0r_construct(unsigned int w, unsigned int h) {
    omni_fade_t *inst = calloc(1, sizeof(omni_fade_t));
    if (!inst) return NULL;
    inst->width = w;
    inst->height = h;
    inst->resolution_scale = (float)w / REFERENCE_WIDTH;
    inst->last_speed_curve = -1.0f;
    inst->bounds_calculated = 0;
    return (f0r_instance_t)inst;
}

void f0r_destruct(f0r_instance_t i) { free(i); }

void f0r_set_param_value(f0r_instance_t i, f0r_param_t p, int idx) {
    omni_fade_t *inst = (omni_fade_t*)i;
    double v = *(double*)p;
    switch (idx) {
        case 0: inst->position = (float)v; break;
        case 1: inst->speed_curve = (float)v; break;
        case 2: inst->gentle_arrival = (float)v; break;
        case 3: inst->zoom_strength = (float)v; inst->bounds_calculated = 0; break;
        case 4:
            inst->arrival_zoom = (int)(v + 0.5);
            if (inst->arrival_zoom < 0) inst->arrival_zoom = 0;
            else if (inst->arrival_zoom > 2) inst->arrival_zoom = 2;
            inst->bounds_calculated = 0; break;
        case 5:
            inst->departure_zoom = (int)(v + 0.5);
            if (inst->departure_zoom < 0) inst->departure_zoom = 0;
            else if (inst->departure_zoom > 2) inst->departure_zoom = 2;
            inst->bounds_calculated = 0; break;
        case 6: inst->blur_strength = (float)v; break;
        case 7: inst->invert = (v > 0.5); inst->bounds_calculated = 0; break;
        case 8: inst->fill_background = (v > 0.5); inst->bounds_calculated = 0; break;
    }
}

void f0r_get_param_value(f0r_instance_t i, f0r_param_t p, int idx) {
    omni_fade_t *inst = (omni_fade_t*)i;
    double *out = (double*)p;
    switch (idx) {
        case 0: *out = inst->position; break;
        case 1: *out = inst->speed_curve; break;
        case 2: *out = inst->gentle_arrival; break;
        case 3: *out = inst->zoom_strength; break;
        case 4: *out = inst->arrival_zoom; break;
        case 5: *out = inst->departure_zoom; break;
        case 6: *out = inst->blur_strength; break;
        case 7: *out = inst->invert; break;
        case 8: *out = inst->fill_background; break;
    }
}

/* -------------------------------------------------------------------------
   Progress & curves
   ------------------------------------------------------------------------- */
static void build_curve_lut(omni_fade_t *inst) {
    float c = inst->speed_curve;
    if (c <= 0.0f) {
        for (int i = 0; i < CURVE_LUT_SIZE; ++i)
            inst->curve_lut[i] = i / (float)(CURVE_LUT_SIZE - 1);
    } else {
        float exp_val = 1.0f + (c / 100.0f) * 10.0f;
        for (int i = 0; i < CURVE_LUT_SIZE; ++i) {
            float t = i / (float)(CURVE_LUT_SIZE - 1);
            inst->curve_lut[i] = powf(t, exp_val);
        }
    }
    inst->last_speed_curve = c;
}

static float curve_lookup(const float *lut, float t) {
    if (t <= 0.0f) return 0.0f;
    if (t >= 1.0f) return 1.0f;
    float idx = t * (CURVE_LUT_SIZE - 1);
    int i = (int)idx;
    if (i >= CURVE_LUT_SIZE - 1) return lut[CURVE_LUT_SIZE - 1];
    if (i < 0) return lut[0];
    float frac = idx - i;
    return lut[i] * (1.0f - frac) + lut[i + 1] * frac;
}

static float reversed_linear(const omni_fade_t *inst, float t) {
    float strength = 1.0f + (inst->gentle_arrival / 100.0f) * 10.0f;
    return 1.0f - powf(1.0f - t, strength);
}

static float get_progress(omni_fade_t *inst, float linear_p) {
    if (linear_p <= 0.0f) return 0.0f;
    if (linear_p >= 1.0f) return 1.0f;
    if (fabsf(inst->speed_curve - inst->last_speed_curve) > 0.0001f)
        build_curve_lut(inst);

    if (inst->gentle_arrival <= 0.001f) {
        return curve_lookup(inst->curve_lut, linear_p);
    }
    if (inst->speed_curve <= 0.0f) {
        return reversed_linear(inst, linear_p);
    }
    float g = inst->gentle_arrival / 100.0f;
    float main_end = 1.0f - g;
    if (linear_p <= main_end) {
        return main_end * curve_lookup(inst->curve_lut, linear_p / main_end);
    }
    float zone_t = (linear_p - main_end) / g;
    return main_end + (1.0f - curve_lookup(inst->curve_lut, 1.0f - zone_t)) * g;
}

static float get_instant_speed(omni_fade_t *inst, float linear_p) {
    if (linear_p <= 0.0f || linear_p >= 1.0f) return 0.0f;
    const float eps = 0.0005f;
    return (get_progress(inst, linear_p + eps) - get_progress(inst, linear_p)) / eps * 0.6f;
}

/* -------------------------------------------------------------------------
   Content bounds & average
   ------------------------------------------------------------------------- */
static void calculate_content_bounds(const uint32_t* buf, int w, int h,
                                     int* min_x, int* min_y, int* max_x, int* max_y) {
    int left = w, right = -1, top = h, bottom = -1;
    for (int x = 0; x < w; ++x) {
        const uint8_t* p = (const uint8_t*)&buf[(h/2) * w + x];
        if (p[0] || p[1] || p[2] || p[3]) {
            if (x < left) left = x;
            if (x > right) right = x;
        }
    }
    for (int y = 0; y < h; ++y) {
        const uint8_t* p = (const uint8_t*)&buf[y * w + (w/2)];
        if (p[0] || p[1] || p[2] || p[3]) {
            if (y < top) top = y;
            if (y > bottom) bottom = y;
        }
    }
    if (left > right || top > bottom) {
        *min_x = *min_y = 0;
        *max_x = w - 1; *max_y = h - 1;
    } else {
        *min_x = left; *max_x = right;
        *min_y = top;  *max_y = bottom;
    }
}

static uint32_t compute_average_color(const uint32_t *buf, int w, int h,
                                      int min_x, int min_y, int max_x, int max_y) {
    if (min_x > max_x || min_y > max_y) return 0;
    uint64_t sum_r = 0, sum_g = 0, sum_b = 0, sum_a = 0;
    int total = 0;
    int stride = 4;
    for (int y = min_y; y <= max_y; y += stride) {
        for (int x = min_x; x <= max_x; x += stride) {
            uint32_t px = buf[y * w + x];
            sum_a += (px >> 24) & 0xFF;
            sum_r += (px >> 16) & 0xFF;
            sum_g += (px >> 8)  & 0xFF;
            sum_b += px & 0xFF;
            total++;
        }
    }
    if (total == 0) return 0;
    return ((uint32_t)(sum_a / total) << 24) |
           ((uint32_t)(sum_r / total) << 16) |
           ((uint32_t)(sum_g / total) << 8)  |
           (uint32_t)(sum_b / total);
}

/* -------------------------------------------------------------------------
   Sampling helpers
   ------------------------------------------------------------------------- */
static inline uint32_t sample_pixel(const uint32_t *restrict buf, int w, int h,
                                    int x, int y, uint32_t avg, int fill,
                                    int minx, int miny, int maxx, int maxy) {
    if (x < minx || x > maxx || y < miny || y > maxy || x < 0 || x >= w || y < 0 || y >= h)
        return fill ? avg : 0;
    return buf[y * w + x];
}

static inline uint32_t sample_zoomed_fast(const uint32_t *restrict buf, int w, int h,
                                          float inv_scale, float cx, float cy,
                                          int x, int y, uint32_t avg, int fill,
                                          int minx, int miny, int maxx, int maxy) {
    int ix = (int)(cx + (x - cx) * inv_scale + 0.5f);
    int iy = (int)(cy + (y - cy) * inv_scale + 0.5f);
    return sample_pixel(buf, w, h, ix, iy, avg, fill, minx, miny, maxx, maxy);
}

static inline uint32_t sample_square_blurred_fast(const uint32_t *restrict buf, int w, int h,
                                                  float inv_scale, float cx, float cy,
                                                  int x, int y, float dir_step_factor,
                                                  int edge_prot, uint32_t avg, int fill,
                                                  int minx, int miny, int maxx, int maxy) {
    int sx = (int)(cx + (x - cx) * inv_scale + 0.5f);
    int sy = (int)(cy + (y - cy) * inv_scale + 0.5f);

    float max_comp = fmaxf(fabsf(sx - cx), fabsf(sy - cy));
    if (max_comp < 3.0f)
        return sample_pixel(buf, w, h, sx, sy, avg, fill, minx, miny, maxx, maxy);

    uint32_t px = sample_pixel(buf, w, h, sx, sy, avg, fill, minx, miny, maxx, maxy);
    float r = ((px >> 16) & 0xFF);
    float g = ((px >> 8)  & 0xFF);
    float b = (px & 0xFF);
    float a = ((px >> 24) & 0xFF);
    float tot = 1.0f;

    float step_x = (sx - cx) * dir_step_factor;
    float step_y = (sy - cy) * dir_step_factor;

    for (int i = 1; i <= SQUARE_SAMPLES; ++i) {
        int ox = sx + (int)(step_x * i);
        int oy = sy + (int)(step_y * i);
        if (edge_prot && (ox < minx || ox > maxx || oy < miny || oy > maxy))
            break;
        uint32_t sp = sample_pixel(buf, w, h, ox, oy, avg, fill, minx, miny, maxx, maxy);
        float wt = 1.0f - (float)i / (SQUARE_SAMPLES + 3);
        r += ((sp >> 16) & 0xFF) * wt;
        g += ((sp >> 8)  & 0xFF) * wt;
        b += (sp & 0xFF) * wt;
        a += ((sp >> 24) & 0xFF) * wt;
        tot += wt;
    }
    float inv_tot = 1.0f / tot;
    return ((uint32_t)(a * inv_tot) << 24) | ((uint32_t)(r * inv_tot) << 16) |
           ((uint32_t)(g * inv_tot) << 8) | (uint32_t)(b * inv_tot);
}

static inline void blend_pixel(uint32_t out_px, uint32_t in_px, float t, uint32_t *restrict result) {
    uint8_t a1 = (out_px >> 24) & 0xFF, r1 = (out_px >> 16) & 0xFF,
            g1 = (out_px >> 8)  & 0xFF, b1 = out_px & 0xFF;
    uint8_t a2 = (in_px >> 24) & 0xFF, r2 = (in_px >> 16) & 0xFF,
            g2 = (in_px >> 8)  & 0xFF, b2 = in_px & 0xFF;

    float alpha_out = a1 / 255.0f;
    float alpha_in  = a2 / 255.0f;
    float final_a = alpha_out * (1.0f - t) + alpha_in * t;
    if (final_a < 0.02f) { *result = 0; return; }

    float out_c = alpha_out * (1.0f - t);
    float in_c  = alpha_in * t;
    uint8_t r = (uint8_t)((r1 * out_c + r2 * in_c) / final_a + 0.5f);
    uint8_t g = (uint8_t)((g1 * out_c + g2 * in_c) / final_a + 0.5f);
    uint8_t b = (uint8_t)((b1 * out_c + b2 * in_c) / final_a + 0.5f);
    uint8_t a = (uint8_t)(final_a * 255.0f + 0.5f);
    *result = (a << 24) | (r << 16) | (g << 8) | b;
}

/* -------------------------------------------------------------------------
   Specialized renderers
   ------------------------------------------------------------------------- */
static inline void render_simple(omni_fade_t *inst, uint32_t *restrict out,
                                 const uint32_t *restrict clip_out,
                                 const uint32_t *restrict clip_in,
                                 float p, int w, int h) {
    for (int y = 0; y < h; ++y) {
        int row = y * w;
        for (int x = 0; x < w; ++x) {
            blend_pixel(clip_out[row + x], clip_in[row + x], p, &out[row + x]);
        }
    }
}

static inline void render_zoom_only(omni_fade_t *inst, uint32_t *restrict out,
                                    const uint32_t *restrict clip_out,
                                    const uint32_t *restrict clip_in,
                                    float p, float zf, int dep_mode, int arr_mode,
                                    int w, int h) {
    float cx = (w - 1) * 0.5f;
    float cy = (h - 1) * 0.5f;

    float out_scale = (dep_mode == 0) ? sqrtf(1.0f + zf * p) :
                      (dep_mode == 2) ? fmaxf(MIN_SCALE, sqrtf(fmaxf(0.0f, 1.0f - zf * p))) : 1.0f;
    float in_scale  = (arr_mode == 0) ? sqrtf(1.0f - zf + zf * p) :
                      (arr_mode == 2) ? sqrtf(1.0f + zf - zf * p) : 1.0f;

    float inv_out = 1.0f / out_scale;
    float inv_in  = 1.0f / in_scale;

    int fill = inst->fill_background;
    uint32_t avg_out = fill ? inst->avg_out : 0;
    uint32_t avg_in  = fill ? inst->avg_in : 0;

    int out_crop = (dep_mode == 2);
    int in_crop  = (arr_mode == 2);
    int ocx1 = inst->out_crop_min_x, ocy1 = inst->out_crop_min_y, ocx2 = inst->out_crop_max_x, ocy2 = inst->out_crop_max_y;
    int icx1 = inst->in_crop_min_x,  icy1 = inst->in_crop_min_y,  icx2 = inst->in_crop_max_x,  icy2 = inst->in_crop_max_y;

    for (int y = 0; y < h; ++y) {
        int row = y * w;
        for (int x = 0; x < w; ++x) {
            uint32_t out_px = (out_crop && (x < ocx1 || x > ocx2 || y < ocy1 || y > ocy2)) ?
                              (fill ? avg_out : 0) :
                              sample_zoomed_fast(clip_out, w, h, inv_out, cx, cy, x, y, avg_out, fill,
                                                 inst->out_min_x, inst->out_min_y, inst->out_max_x, inst->out_max_y);

            uint32_t in_px = (in_crop && (x < icx1 || x > icx2 || y < icy1 || y > icy2)) ?
                             (fill ? avg_in : 0) :
                             sample_zoomed_fast(clip_in, w, h, inv_in, cx, cy, x, y, avg_in, fill,
                                                inst->in_min_x, inst->in_min_y, inst->in_max_x, inst->in_max_y);

            blend_pixel(out_px, in_px, p, &out[row + x]);
        }
    }
}

static inline void render_zoom_blur(omni_fade_t *inst, uint32_t *restrict out,
                                    const uint32_t *restrict clip_out,
                                    const uint32_t *restrict clip_in,
                                    float p, float zf, float base_speed,
                                    int dep_mode, int arr_mode, int w, int h) {
    float cx = (w - 1) * 0.5f;
    float cy = (h - 1) * 0.5f;

    float out_scale = (dep_mode == 0) ? sqrtf(1.0f + zf * p) :
                      (dep_mode == 2) ? fmaxf(MIN_SCALE, sqrtf(fmaxf(0.0f, 1.0f - zf * p))) : 1.0f;
    float in_scale  = (arr_mode == 0) ? sqrtf(1.0f - zf + zf * p) :
                      (arr_mode == 2) ? sqrtf(1.0f + zf - zf * p) : 1.0f;

    float inv_out = 1.0f / out_scale;
    float inv_in  = 1.0f / in_scale;

    float norm_blur = (inst->blur_strength / 100.0f) * 0.5f;
    float out_motion = (dep_mode == 0 || dep_mode == 2) ? zf * base_speed * 100.0f : 0.0f;
    float in_motion  = (arr_mode == 0 || arr_mode == 2) ? zf * base_speed * 100.0f : 0.0f;
    float blur_out_val = norm_blur * (base_speed + out_motion);
    float blur_in_val  = norm_blur * (base_speed + in_motion);

    int do_blur_out = (blur_out_val > 0.7f);
    int do_blur_in  = (blur_in_val > 0.7f);

    float max_dim = (float)(w > h ? w : h);
    float out_step = blur_out_val * inst->resolution_scale * 2.0f / (max_dim * SQUARE_SAMPLES);
    float in_step  = blur_in_val  * inst->resolution_scale * 2.0f / (max_dim * SQUARE_SAMPLES);
    float out_dir = (dep_mode == 0) ? out_step : -out_step;
    float in_dir  = (arr_mode == 0) ? in_step  : -in_step;

    int fill = inst->fill_background;
    uint32_t avg_out = fill ? inst->avg_out : 0;
    uint32_t avg_in  = fill ? inst->avg_in : 0;

    int out_edge_prot = (dep_mode == 0) && !fill;
    int in_edge_prot  = (arr_mode == 0) && !fill;

    int out_crop = (dep_mode == 2);
    int in_crop  = (arr_mode == 2);
    int ocx1 = inst->out_crop_min_x, ocy1 = inst->out_crop_min_y, ocx2 = inst->out_crop_max_x, ocy2 = inst->out_crop_max_y;
    int icx1 = inst->in_crop_min_x,  icy1 = inst->in_crop_min_y,  icx2 = inst->in_crop_max_x,  icy2 = inst->in_crop_max_y;

    for (int y = 0; y < h; ++y) {
        int row = y * w;
        for (int x = 0; x < w; ++x) {
            uint32_t out_px = (out_crop && (x < ocx1 || x > ocx2 || y < ocy1 || y > ocy2)) ? (fill ? avg_out : 0) :
                (do_blur_out ? sample_square_blurred_fast(clip_out, w, h, inv_out, cx, cy, x, y,
                                                          out_dir, out_edge_prot, avg_out, fill,
                                                          inst->out_min_x, inst->out_min_y, inst->out_max_x, inst->out_max_y) :
                               sample_zoomed_fast(clip_out, w, h, inv_out, cx, cy, x, y, avg_out, fill,
                                                  inst->out_min_x, inst->out_min_y, inst->out_max_x, inst->out_max_y));

            uint32_t in_px = (in_crop && (x < icx1 || x > icx2 || y < icy1 || y > icy2)) ? (fill ? avg_in : 0) :
                (do_blur_in ? sample_square_blurred_fast(clip_in, w, h, inv_in, cx, cy, x, y,
                                                         in_dir, in_edge_prot, avg_in, fill,
                                                         inst->in_min_x, inst->in_min_y, inst->in_max_x, inst->in_max_y) :
                              sample_zoomed_fast(clip_in, w, h, inv_in, cx, cy, x, y, avg_in, fill,
                                                 inst->in_min_x, inst->in_min_y, inst->in_max_x, inst->in_max_y));

            blend_pixel(out_px, in_px, p, &out[row + x]);
        }
    }
}

/* -------------------------------------------------------------------------
   Main dispatcher
   ------------------------------------------------------------------------- */
static void apply_fade(omni_fade_t *inst, uint32_t *restrict out,
                       const uint32_t *restrict clip_out,
                       const uint32_t *restrict clip_in,
                       float linear_p) {
    int w = inst->width, h = inst->height;
    float p = get_progress(inst, linear_p);
    float zf = inst->zoom_strength / 100.0f;

    int dep_mode = inst->departure_zoom;
    int arr_mode = inst->arrival_zoom;
    int has_zoom = (zf > 0.001f && (dep_mode != 1 || arr_mode != 1));
    int has_blur = (inst->blur_strength > 0.1f);

    if (!inst->bounds_calculated) {
        calculate_content_bounds(clip_out, w, h, &inst->out_min_x, &inst->out_min_y, &inst->out_max_x, &inst->out_max_y);
        calculate_content_bounds(clip_in, w, h, &inst->in_min_x, &inst->in_min_y, &inst->in_max_x, &inst->in_max_y);

        if (inst->fill_background) {
            inst->avg_out = compute_average_color(clip_out, w, h, inst->out_min_x, inst->out_min_y, inst->out_max_x, inst->out_max_y);
            inst->avg_in  = compute_average_color(clip_in, w, h, inst->in_min_x, inst->in_min_y, inst->in_max_x, inst->in_max_y);
        } else {
            inst->avg_out = inst->avg_in = 0;
        }

        float cx = (w - 1) * 0.5f;
        float cy = (h - 1) * 0.5f;

        if (dep_mode == 2) {
            float scale_init = 1.0f;
            inst->out_crop_min_x = (int)(cx + (inst->out_min_x - cx) * scale_init + 0.5f);
            inst->out_crop_max_x = (int)(cx + (inst->out_max_x - cx) * scale_init + 0.5f);
            inst->out_crop_min_y = (int)(cy + (inst->out_min_y - cy) * scale_init + 0.5f);
            inst->out_crop_max_y = (int)(cy + (inst->out_max_y - cy) * scale_init + 0.5f);
        }
        if (arr_mode == 2) {
            float scale_init = sqrtf(1.0f + zf);
            inst->in_crop_min_x = (int)(cx + (inst->in_min_x - cx) * scale_init + 0.5f);
            inst->in_crop_max_x = (int)(cx + (inst->in_max_x - cx) * scale_init + 0.5f);
            inst->in_crop_min_y = (int)(cy + (inst->in_min_y - cy) * scale_init + 0.5f);
            inst->in_crop_max_y = (int)(cy + (inst->in_max_y - cy) * scale_init + 0.5f);
        }
        inst->bounds_calculated = 1;
    }

    if (!has_zoom && !has_blur) {
        render_simple(inst, out, clip_out, clip_in, p, w, h);
    } else if (!has_blur) {
        render_zoom_only(inst, out, clip_out, clip_in, p, zf, dep_mode, arr_mode, w, h);
    } else {
        float base_speed = get_instant_speed(inst, linear_p);
        render_zoom_blur(inst, out, clip_out, clip_in, p, zf, base_speed, dep_mode, arr_mode, w, h);
    }
}

void f0r_update2(f0r_instance_t i, double time,
                 const uint32_t *in1, const uint32_t *in2,
                 const uint32_t *in3, uint32_t *out) {
    (void)time; (void)in3;
    omni_fade_t *inst = (omni_fade_t*)i;
    float p = fmaxf(0.0f, fminf(1.0f, inst->position));
    const uint32_t *clip_out = inst->invert ? in2 : in1;
    const uint32_t *clip_in  = inst->invert ? in1 : in2;
    apply_fade(inst, out, clip_out, clip_in, p);
}
