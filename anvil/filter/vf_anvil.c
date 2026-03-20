/*
 * vf_anvil.c - ANVIL Video Frame Interpolation filter for mpv
 *
 * Frame-doubling VFI: 30fps → 60fps via MV prealign v2 + QNN HTP INT8.
 *
 * State machine:
 *   NEED_INPUT  → read frame, compute interpolated, output original curr
 *   HAVE_INTERP → output stored interpolated frame, transition to NEED_INPUT
 *
 * Interpolated frame split-screen overlay:
 *   Left of split line:  previous original frame (held still = 30fps)
 *   Right of split line: ANVIL interpolated frame (MV prealign + QNN residual)
 *
 * QNN HTP inference via dlopen (no subprocess).
 *
 * Usage: --vf=anvil:split=0.5
 *
 * This file is part of mpv.
 * License: LGPL 2.1+
 */

// ════════════════════════════════════════════════════════════════════
// Section 1: Includes
// ════════════════════════════════════════════════════════════════════

#include <math.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <dlfcn.h>

#include <libavutil/motion_vector.h>
#include <libavutil/frame.h>

#include "common/common.h"
#include "filters/filter.h"
#include "filters/filter_internal.h"
#include "filters/user_filters.h"
#include "options/m_option.h"
#include "video/mp_image.h"
#include "video/img_format.h"

// QNN headers (compile-time types only; runtime via dlopen)
#include "QNN/QnnInterface.h"
#include "QNN/System/QnnSystemInterface.h"
#include "QNN/HTP/QnnHtpPerfInfrastructure.h"

// ════════════════════════════════════════════════════════════════════
// Section 2: Prealign v2 functions (frozen recipe)
// ════════════════════════════════════════════════════════════════════

#define PA_DS        4
#define PA_MED_K     5
#define PA_GAUSS_S   2.0f

// Dense flow from AVMotionVector[]: ZOH fill
static void zoh_fill(const AVMotionVector *mvs, int n_mvs,
                     float *fx, float *fy, int H, int W)
{
    memset(fx, 0, H * W * sizeof(float));
    memset(fy, 0, H * W * sizeof(float));
    for (int i = 0; i < n_mvs; i++) {
        const AVMotionVector *m = &mvs[i];
        if (m->source != -1) continue;
        float scale = m->motion_scale > 0 ? (float)m->motion_scale : 1.0f;
        float mx = -(float)m->motion_x / scale;
        float my = -(float)m->motion_y / scale;
        int bx = m->dst_x - m->w / 2, by = m->dst_y - m->h / 2;
        int x0 = bx < 0 ? 0 : bx, y0 = by < 0 ? 0 : by;
        int x1 = bx + m->w > W ? W : bx + m->w;
        int y1 = by + m->h > H ? H : by + m->h;
        for (int y = y0; y < y1; y++)
            for (int x = x0; x < x1; x++) {
                fx[y * W + x] = mx;
                fy[y * W + x] = my;
            }
    }
}

// Nearest-neighbor downsample
static void downsample(const float *in, float *out, int H, int W, int s,
                       int *oH, int *oW)
{
    *oH = H / s; *oW = W / s;
    for (int y = 0; y < *oH; y++)
        for (int x = 0; x < *oW; x++)
            out[y * (*oW) + x] = in[y * s * W + x * s];
}

// Median filter 2D
static int cmp_float(const void *a, const void *b) {
    float fa = *(const float *)a, fb = *(const float *)b;
    return (fa > fb) - (fa < fb);
}

static void median_2d(const float *in, float *out, int H, int W, int k) {
    int half = k / 2;
    float buf[PA_MED_K * PA_MED_K];
    for (int y = 0; y < H; y++)
        for (int x = 0; x < W; x++) {
            int cnt = 0;
            for (int ky = -half; ky <= half; ky++) {
                int yy = y + ky; if (yy < 0) yy = 0; if (yy >= H) yy = H - 1;
                for (int kx = -half; kx <= half; kx++) {
                    int xx = x + kx; if (xx < 0) xx = 0; if (xx >= W) xx = W - 1;
                    buf[cnt++] = in[yy * W + xx];
                }
            }
            qsort(buf, cnt, sizeof(float), cmp_float);
            out[y * W + x] = buf[cnt / 2];
        }
}

// Gaussian blur separable
static void gauss_sep(float *data, float *tmp, int H, int W, float sigma) {
    int ks = (int)(sigma * 6.0f) | 1;
    if (ks < 3) ks = 3; if (ks > 49) ks = 49;
    int half = ks / 2;
    float kern[50], sum = 0;
    for (int i = 0; i < ks; i++) {
        float d = (float)(i - half);
        kern[i] = expf(-d * d / (2.0f * sigma * sigma));
        sum += kern[i];
    }
    for (int i = 0; i < ks; i++) kern[i] /= sum;
    for (int y = 0; y < H; y++)
        for (int x = 0; x < W; x++) {
            float s = 0;
            for (int k = -half; k <= half; k++) {
                int xx = x + k; if (xx < 0) xx = 0; if (xx >= W) xx = W - 1;
                s += data[y * W + xx] * kern[k + half];
            }
            tmp[y * W + x] = s;
        }
    for (int y = 0; y < H; y++)
        for (int x = 0; x < W; x++) {
            float s = 0;
            for (int k = -half; k <= half; k++) {
                int yy = y + k; if (yy < 0) yy = 0; if (yy >= H) yy = H - 1;
                s += tmp[yy * W + x] * kern[k + half];
            }
            data[y * W + x] = s;
        }
}

// Bilinear upsample
static void upsample(const float *in, float *out, int iH, int iW, int oH, int oW) {
    float sy = (float)iH / oH, sx = (float)iW / oW;
    for (int y = 0; y < oH; y++) {
        float fy = y * sy; int y0 = (int)fy, y1 = y0 + 1; float wy = fy - y0;
        if (y0 >= iH) y0 = iH - 1; if (y1 >= iH) y1 = iH - 1;
        for (int x = 0; x < oW; x++) {
            float fx = x * sx; int x0 = (int)fx, x1 = x0 + 1; float wx = fx - x0;
            if (x0 >= iW) x0 = iW - 1; if (x1 >= iW) x1 = iW - 1;
            out[y * oW + x] = (1-wx)*(1-wy)*in[y0*iW+x0] + wx*(1-wy)*in[y0*iW+x1]
                             + (1-wx)*wy*in[y1*iW+x0] + wx*wy*in[y1*iW+x1];
        }
    }
}

// Bilinear sample (single plane, uint8)
static inline uint8_t sample_bilinear(const uint8_t *src, int W, int H,
                                       int stride, float sx, float sy)
{
    int x0 = (int)floorf(sx), y0 = (int)floorf(sy);
    int x1 = x0 + 1, y1 = y0 + 1;
    float wx = sx - x0, wy = sy - y0;
    if (x0 < 0) x0 = 0; if (x0 >= W) x0 = W-1;
    if (x1 < 0) x1 = 0; if (x1 >= W) x1 = W-1;
    if (y0 < 0) y0 = 0; if (y0 >= H) y0 = H-1;
    if (y1 < 0) y1 = 0; if (y1 >= H) y1 = H-1;
    float v = (1-wx)*(1-wy)*src[y0*stride+x0] + wx*(1-wy)*src[y0*stride+x1]
            + (1-wx)*wy*src[y1*stride+x0] + wx*wy*src[y1*stride+x1];
    int iv = (int)(v + 0.5f);
    return iv < 0 ? 0 : (iv > 255 ? 255 : (uint8_t)iv);
}

// Bilinear sample float (for flow-warping float RGB planes)
static inline float sample_bilinear_f(const float *src, int W, int H,
                                       float sx, float sy)
{
    int x0 = (int)floorf(sx), y0 = (int)floorf(sy);
    int x1 = x0 + 1, y1 = y0 + 1;
    float wx = sx - x0, wy = sy - y0;
    if (x0 < 0) x0 = 0; if (x0 >= W) x0 = W-1;
    if (x1 < 0) x1 = 0; if (x1 >= W) x1 = W-1;
    if (y0 < 0) y0 = 0; if (y0 >= H) y0 = H-1;
    if (y1 < 0) y1 = 0; if (y1 >= H) y1 = H-1;
    return (1-wx)*(1-wy)*src[y0*W+x0] + wx*(1-wy)*src[y0*W+x1]
         + (1-wx)*wy*src[y1*W+x0] + wx*wy*src[y1*W+x1];
}

// ════════════════════════════════════════════════════════════════════
// Section 3: YUV <-> RGB conversion (BT.709)
// ════════════════════════════════════════════════════════════════════

// YUV420P -> planar float RGB [0,1], each plane is H*W floats
static void yuv420p_to_rgb_float(const uint8_t *Y, int y_stride,
                                  const uint8_t *U, int u_stride,
                                  const uint8_t *V, int v_stride,
                                  float *R, float *G, float *B,
                                  int W, int H)
{
    for (int y = 0; y < H; y++) {
        const uint8_t *yrow = Y + y * y_stride;
        const uint8_t *urow = U + (y >> 1) * u_stride;
        const uint8_t *vrow = V + (y >> 1) * v_stride;
        for (int x = 0; x < W; x++) {
            float yf = 1.164f * ((float)yrow[x] - 16.0f);
            float uf = (float)urow[x >> 1] - 128.0f;
            float vf = (float)vrow[x >> 1] - 128.0f;
            float r = yf + 1.793f * vf;
            float g = yf - 0.213f * uf - 0.533f * vf;
            float b = yf + 2.112f * uf;
            int idx = y * W + x;
            R[idx] = r / 255.0f;
            G[idx] = g / 255.0f;
            B[idx] = b / 255.0f;
        }
    }
}

// Float RGB [0,1] -> YUV420P uint8
static void rgb_float_to_yuv420p(const float *R, const float *G, const float *B,
                                  uint8_t *Y, int y_stride,
                                  uint8_t *U, int u_stride,
                                  uint8_t *V, int v_stride,
                                  int W, int H)
{
    for (int y = 0; y < H; y++) {
        uint8_t *yrow = Y + y * y_stride;
        for (int x = 0; x < W; x++) {
            int idx = y * W + x;
            float r = R[idx] * 255.0f;
            float g = G[idx] * 255.0f;
            float b = B[idx] * 255.0f;
            float yv = 0.183f * r + 0.614f * g + 0.062f * b + 16.0f;
            int iv = (int)(yv + 0.5f);
            yrow[x] = iv < 0 ? 0 : (iv > 255 ? 255 : (uint8_t)iv);
        }
    }
    // Chroma: average 2x2 blocks
    int cW = W >> 1, cH = H >> 1;
    for (int cy = 0; cy < cH; cy++) {
        uint8_t *urow = U + cy * u_stride;
        uint8_t *vrow = V + cy * v_stride;
        for (int cx = 0; cx < cW; cx++) {
            float ru = 0, gu = 0, bu = 0;
            for (int dy = 0; dy < 2; dy++)
                for (int dx = 0; dx < 2; dx++) {
                    int idx = (cy * 2 + dy) * W + (cx * 2 + dx);
                    ru += R[idx]; gu += G[idx]; bu += B[idx];
                }
            ru *= 255.0f / 4.0f; gu *= 255.0f / 4.0f; bu *= 255.0f / 4.0f;
            float uv = -0.101f * ru - 0.339f * gu + 0.439f * bu + 128.0f;
            float vv =  0.439f * ru - 0.399f * gu - 0.040f * bu + 128.0f;
            int iu = (int)(uv + 0.5f); int iv = (int)(vv + 0.5f);
            urow[cx] = iu < 0 ? 0 : (iu > 255 ? 255 : (uint8_t)iu);
            vrow[cx] = iv < 0 ? 0 : (iv > 255 ? 255 : (uint8_t)iv);
        }
    }
}

// ════════════════════════════════════════════════════════════════════
// Section 4: QNN loader (dlopen, from bench_e2e_pipeline.cpp pattern)
// ════════════════════════════════════════════════════════════════════

#define QNN_DEFAULT_DIR "/data/data/is.xyz.mpv/files/anvil"
#define QNN_CTX_BIN     "context.serialized.bin"
#define QNN_MAX_TENSORS 4

struct qnn_state {
    void *htp_lib;
    void *sys_lib;

    QNN_INTERFACE_VER_TYPE qnn;
    QNN_SYSTEM_INTERFACE_VER_TYPE sys;

    Qnn_BackendHandle_t backend;
    Qnn_ContextHandle_t context;
    Qnn_GraphHandle_t graph;
    QnnSystemContext_Handle_t sys_ctx;

    // I/O tensor arrays for graphExecute
    Qnn_Tensor_t in_tensors[QNN_MAX_TENSORS];
    Qnn_Tensor_t out_tensors[QNN_MAX_TENSORS];
    uint32_t n_in, n_out;

    // Pre-allocated I/O buffers
    float *input_buf;    // H*W*6 float32 NHWC
    float *output_buf;   // H*W*3 float32 NHWC
    uint32_t in_bytes, out_bytes;

    int ready;
};

static const char *qnn_get_dir(void)
{
    const char *env = getenv("QNN_DIR");
    return (env && env[0]) ? env : QNN_DEFAULT_DIR;
}

static int qnn_init(struct qnn_state *q, struct mp_filter *f, int W, int H)
{
    memset(q, 0, sizeof(*q));

    const char *dir = qnn_get_dir();
    char path[512];

    // Set library search paths for HTP backend's internal dlopen calls
    setenv("LD_LIBRARY_PATH", dir, 1);
    setenv("ADSP_LIBRARY_PATH", dir, 1);

    // Pre-load HTP dependencies with absolute paths.
    // Android linker namespaces ignore LD_LIBRARY_PATH set via setenv after
    // process start, so we dlopen each dep with its full path.
    // Note: libQnnHtpV*Skel.so is 32-bit Hexagon code loaded by DSP RPC, NOT by us.
    // libcdsprpc.so is a system library in /vendor/lib64/.

    // QNN libs are bundled in the APK's jniLibs/arm64-v8a/ and loaded by name.
    // libcdsprpc.so is a vendor library declared via <uses-native-library> in
    // the manifest — Android linker resolves it from /vendor/lib64/.
    // The Skel file + context binary are extracted from assets/ to files/anvil/.
    const char *preload[] = {
        "libcdsprpc.so",        // vendor DSP RPC (from /vendor/lib64/ via manifest)
        "libQnnHtpPrepare.so",  // from APK jniLibs
        "libQnnHtpV75Stub.so",  // from APK jniLibs
        NULL
    };
    for (int i = 0; preload[i]; i++) {
        void *h = dlopen(preload[i], RTLD_NOW | RTLD_GLOBAL);
        if (!h)
            MP_WARN(f, "QNN: preload %s: %s (non-fatal)\n", preload[i], dlerror());
    }

    // Load libQnnHtp.so (from APK jniLibs, by name)
    q->htp_lib = dlopen("libQnnHtp.so", RTLD_NOW | RTLD_GLOBAL);
    if (!q->htp_lib) {
        MP_WARN(f, "QNN: dlopen libQnnHtp.so: %s\n", dlerror());
        return -1;
    }

    // Load libQnnSystem.so (from APK jniLibs, by name)
    q->sys_lib = dlopen("libQnnSystem.so", RTLD_NOW | RTLD_GLOBAL);
    if (!q->sys_lib) {
        MP_WARN(f, "QNN: dlopen %s: %s\n", path, dlerror());
        return -1;
    }

    // Resolve QNN interface
    typedef Qnn_ErrorHandle_t (*GetProvidersFn)(const QnnInterface_t***, uint32_t*);
    GetProvidersFn getProviders =
        (GetProvidersFn)dlsym(q->htp_lib, "QnnInterface_getProviders");
    if (!getProviders) {
        MP_WARN(f, "QNN: QnnInterface_getProviders not found\n");
        return -1;
    }
    const QnnInterface_t **providerList = NULL;
    uint32_t numProviders = 0;
    if (getProviders(&providerList, &numProviders) != QNN_SUCCESS || numProviders == 0) {
        MP_WARN(f, "QNN: getProviders failed\n");
        return -1;
    }
    q->qnn = providerList[0]->QNN_INTERFACE_VER_NAME;

    // Resolve System interface
    typedef Qnn_ErrorHandle_t (*SysGetProvidersFn)(const QnnSystemInterface_t***, uint32_t*);
    SysGetProvidersFn sysGetProviders =
        (SysGetProvidersFn)dlsym(q->sys_lib, "QnnSystemInterface_getProviders");
    if (!sysGetProviders) {
        MP_WARN(f, "QNN: QnnSystemInterface_getProviders not found\n");
        return -1;
    }
    const QnnSystemInterface_t **sysProviderList = NULL;
    uint32_t numSysProviders = 0;
    if (sysGetProviders(&sysProviderList, &numSysProviders) != QNN_SUCCESS ||
        numSysProviders == 0)
    {
        MP_WARN(f, "QNN: sysGetProviders failed\n");
        return -1;
    }
    q->sys = sysProviderList[0]->QNN_SYSTEM_INTERFACE_VER_NAME;

    // Create backend
    if (q->qnn.backendCreate(NULL, NULL, &q->backend) != QNN_SUCCESS) {
        MP_WARN(f, "QNN: backendCreate failed\n");
        return -1;
    }

    // Read context binary
    snprintf(path, sizeof(path), "%s/%s", dir, QNN_CTX_BIN);
    FILE *fp = fopen(path, "rb");
    if (!fp) {
        MP_WARN(f, "QNN: cannot open %s\n", path);
        return -1;
    }
    fseek(fp, 0, SEEK_END);
    long file_size = ftell(fp);
    fseek(fp, 0, SEEK_SET);
    void *binary = malloc(file_size);
    if (!binary || fread(binary, 1, file_size, fp) != (size_t)file_size) {
        MP_WARN(f, "QNN: failed to read context binary\n");
        fclose(fp); free(binary);
        return -1;
    }
    fclose(fp);

    // Extract graph metadata via QnnSystem
    if (q->sys.systemContextCreate(&q->sys_ctx) != QNN_SUCCESS) {
        MP_WARN(f, "QNN: systemContextCreate failed\n");
        free(binary);
        return -1;
    }

    const QnnSystemContext_BinaryInfo_t *binaryInfo = NULL;
    Qnn_ContextBinarySize_t binaryInfoSize = 0;
    if (q->sys.systemContextGetBinaryInfo(q->sys_ctx, binary, (uint64_t)file_size,
                                           &binaryInfo, &binaryInfoSize) != QNN_SUCCESS)
    {
        MP_WARN(f, "QNN: systemContextGetBinaryInfo failed\n");
        free(binary);
        return -1;
    }

    // Get first graph's tensor info
    const QnnSystemContext_GraphInfo_t *graphs = NULL;
    uint32_t numGraphs = 0;
    const char *graph_name = NULL;

    if (binaryInfo->version == QNN_SYSTEM_CONTEXT_BINARY_INFO_VERSION_1) {
        graphs = binaryInfo->contextBinaryInfoV1.graphs;
        numGraphs = binaryInfo->contextBinaryInfoV1.numGraphs;
    } else if (binaryInfo->version == QNN_SYSTEM_CONTEXT_BINARY_INFO_VERSION_2) {
        graphs = binaryInfo->contextBinaryInfoV2.graphs;
        numGraphs = binaryInfo->contextBinaryInfoV2.numGraphs;
    } else if (binaryInfo->version == QNN_SYSTEM_CONTEXT_BINARY_INFO_VERSION_3) {
        graphs = binaryInfo->contextBinaryInfoV3.graphs;
        numGraphs = binaryInfo->contextBinaryInfoV3.numGraphs;
    }

    if (numGraphs == 0) {
        MP_WARN(f, "QNN: no graphs in context binary\n");
        free(binary);
        return -1;
    }

    const Qnn_Tensor_t *meta_inputs = NULL, *meta_outputs = NULL;

    if (graphs[0].version == QNN_SYSTEM_CONTEXT_GRAPH_INFO_VERSION_1) {
        graph_name  = graphs[0].graphInfoV1.graphName;
        q->n_in     = graphs[0].graphInfoV1.numGraphInputs;
        meta_inputs = graphs[0].graphInfoV1.graphInputs;
        q->n_out    = graphs[0].graphInfoV1.numGraphOutputs;
        meta_outputs= graphs[0].graphInfoV1.graphOutputs;
    } else if (graphs[0].version == QNN_SYSTEM_CONTEXT_GRAPH_INFO_VERSION_2) {
        graph_name  = graphs[0].graphInfoV2.graphName;
        q->n_in     = graphs[0].graphInfoV2.numGraphInputs;
        meta_inputs = graphs[0].graphInfoV2.graphInputs;
        q->n_out    = graphs[0].graphInfoV2.numGraphOutputs;
        meta_outputs= graphs[0].graphInfoV2.graphOutputs;
    } else if (graphs[0].version == QNN_SYSTEM_CONTEXT_GRAPH_INFO_VERSION_3) {
        graph_name  = graphs[0].graphInfoV3.graphName;
        q->n_in     = graphs[0].graphInfoV3.numGraphInputs;
        meta_inputs = graphs[0].graphInfoV3.graphInputs;
        q->n_out    = graphs[0].graphInfoV3.numGraphOutputs;
        meta_outputs= graphs[0].graphInfoV3.graphOutputs;
    }

    if (!graph_name || q->n_in == 0 || q->n_out == 0) {
        MP_WARN(f, "QNN: invalid graph metadata\n");
        free(binary);
        return -1;
    }

    MP_INFO(f, "QNN: graph '%s', %u inputs, %u outputs\n",
            graph_name, q->n_in, q->n_out);

    // Calculate buffer sizes from tensor dims
    q->in_bytes = 0;
    for (uint32_t i = 0; i < q->n_in && i < QNN_MAX_TENSORS; i++) {
        uint32_t elem = 1;
        for (uint32_t d = 0; d < meta_inputs[i].v1.rank; d++)
            elem *= meta_inputs[i].v1.dimensions[d];
        q->in_bytes += elem * sizeof(float);
    }
    q->out_bytes = 0;
    for (uint32_t i = 0; i < q->n_out && i < QNN_MAX_TENSORS; i++) {
        uint32_t elem = 1;
        for (uint32_t d = 0; d < meta_outputs[i].v1.rank; d++)
            elem *= meta_outputs[i].v1.dimensions[d];
        q->out_bytes += elem * sizeof(float);
    }

    q->input_buf  = (float *)calloc(1, q->in_bytes);
    q->output_buf = (float *)calloc(1, q->out_bytes);

    // Load context from binary
    Qnn_ErrorHandle_t ctx_err = q->qnn.contextCreateFromBinary(
        q->backend, NULL, NULL, binary,
        (Qnn_ContextBinarySize_t)file_size, &q->context, NULL);
    if (ctx_err != QNN_SUCCESS) {
        MP_WARN(f, "QNN: contextCreateFromBinary failed (err=0x%x). "
                "Check ADSP_LIBRARY_PATH=%s has libQnnHtpV*Skel.so\n",
                (unsigned)ctx_err, dir);
        free(binary);
        return -1;
    }

    // Retrieve graph handle
    if (q->qnn.graphRetrieve(q->context, graph_name, &q->graph) != QNN_SUCCESS) {
        MP_WARN(f, "QNN: graphRetrieve failed\n");
        free(binary);
        return -1;
    }

    // Set up tensor arrays pointing to our buffers
    uint32_t in_off = 0;
    for (uint32_t i = 0; i < q->n_in && i < QNN_MAX_TENSORS; i++) {
        q->in_tensors[i] = meta_inputs[i];
        q->in_tensors[i].v1.memType = QNN_TENSORMEMTYPE_RAW;
        uint32_t elem = 1;
        for (uint32_t d = 0; d < q->in_tensors[i].v1.rank; d++)
            elem *= q->in_tensors[i].v1.dimensions[d];
        uint32_t sz = elem * sizeof(float);
        q->in_tensors[i].v1.clientBuf.data = (uint8_t *)q->input_buf + in_off;
        q->in_tensors[i].v1.clientBuf.dataSize = sz;
        in_off += sz;
    }

    uint32_t out_off = 0;
    for (uint32_t i = 0; i < q->n_out && i < QNN_MAX_TENSORS; i++) {
        q->out_tensors[i] = meta_outputs[i];
        q->out_tensors[i].v1.memType = QNN_TENSORMEMTYPE_RAW;
        uint32_t elem = 1;
        for (uint32_t d = 0; d < q->out_tensors[i].v1.rank; d++)
            elem *= q->out_tensors[i].v1.dimensions[d];
        uint32_t sz = elem * sizeof(float);
        q->out_tensors[i].v1.clientBuf.data = (uint8_t *)q->output_buf + out_off;
        q->out_tensors[i].v1.clientBuf.dataSize = sz;
        out_off += sz;
    }

    free(binary);
    q->ready = 1;
    MP_INFO(f, "QNN: initialized (in=%u bytes, out=%u bytes)\n", q->in_bytes, q->out_bytes);
    return 0;
}

static Qnn_ErrorHandle_t qnn_execute(struct qnn_state *q)
{
    return q->qnn.graphExecute(q->graph,
                               q->in_tensors, q->n_in,
                               q->out_tensors, q->n_out,
                               NULL, NULL);
}

static void qnn_cleanup(struct qnn_state *q)
{
    if (!q) return;
    if (q->context) q->qnn.contextFree(q->context, NULL);
    if (q->backend) q->qnn.backendFree(q->backend);
    if (q->sys_ctx) q->sys.systemContextFree(q->sys_ctx);
    free(q->input_buf);
    free(q->output_buf);
    if (q->htp_lib) dlclose(q->htp_lib);
    if (q->sys_lib) dlclose(q->sys_lib);
    memset(q, 0, sizeof(*q));
}

// ════════════════════════════════════════════════════════════════════
// Section 5: Filter state machine (frame doubling)
// ════════════════════════════════════════════════════════════════════

enum anvil_state {
    STATE_NEED_INPUT,
    STATE_HAVE_INTERP,
};

struct anvil_opts {
    float split;
};

struct priv {
    struct anvil_opts *opts;

    enum anvil_state state;
    int frame_count;

    // Previous frame for interpolation
    struct mp_image *prev;

    // Stored interpolated frame (output in HAVE_INTERP state)
    struct mp_image *interp;
    double interp_pts;

    // Prealign workspace
    int alloc_w, alloc_h;
    float *flow_x, *flow_y;
    float *sm_a, *sm_b, *sm_tmp;
    int sH, sW;

    // Float RGB workspace (3 planes each for prev and curr)
    float *rgb_prev[3];   // R, G, B
    float *rgb_curr[3];
    float *rgb_warp0[3];  // warped prev
    float *rgb_warp1[3];  // warped curr
    float *rgb_out[3];    // final interpolated RGB

    // QNN state
    struct qnn_state qnn;
    int qnn_checked;
};

static void free_workspace(struct priv *p)
{
    free(p->flow_x);  free(p->flow_y);
    free(p->sm_a);    free(p->sm_b);    free(p->sm_tmp);
    p->flow_x = p->flow_y = p->sm_a = p->sm_b = p->sm_tmp = NULL;

    for (int c = 0; c < 3; c++) {
        free(p->rgb_prev[c]);  p->rgb_prev[c] = NULL;
        free(p->rgb_curr[c]);  p->rgb_curr[c] = NULL;
        free(p->rgb_warp0[c]); p->rgb_warp0[c] = NULL;
        free(p->rgb_warp1[c]); p->rgb_warp1[c] = NULL;
        free(p->rgb_out[c]);   p->rgb_out[c] = NULL;
    }
    p->alloc_w = p->alloc_h = 0;
}

static void alloc_workspace(struct priv *p, int w, int h)
{
    if (p->alloc_w == w && p->alloc_h == h) return;
    free_workspace(p);

    int px = w * h;
    p->sH = h / PA_DS; p->sW = w / PA_DS;
    int spx = p->sH * p->sW;

    p->flow_x  = calloc(px, sizeof(float));
    p->flow_y  = calloc(px, sizeof(float));
    p->sm_a    = malloc(spx * sizeof(float));
    p->sm_b    = malloc(spx * sizeof(float));
    p->sm_tmp  = malloc(spx * sizeof(float));

    for (int c = 0; c < 3; c++) {
        p->rgb_prev[c]  = malloc(px * sizeof(float));
        p->rgb_curr[c]  = malloc(px * sizeof(float));
        p->rgb_warp0[c] = malloc(px * sizeof(float));
        p->rgb_warp1[c] = malloc(px * sizeof(float));
        p->rgb_out[c]   = malloc(px * sizeof(float));
    }

    p->alloc_w = w; p->alloc_h = h;
}

// Run prealign v2 on the flow field (modifies flow_x/flow_y in place)
static void run_prealign_v2(struct priv *p, int W, int H)
{
    int dH, dW;
    downsample(p->flow_x, p->sm_a, H, W, PA_DS, &dH, &dW);
    downsample(p->flow_y, p->sm_b, H, W, PA_DS, &dH, &dW);
    median_2d(p->sm_a, p->sm_tmp, dH, dW, PA_MED_K);
    median_2d(p->sm_b, p->sm_a,   dH, dW, PA_MED_K);
    // sm_tmp = filtered fx at 1/4, sm_a = filtered fy at 1/4
    gauss_sep(p->sm_tmp, p->sm_b, dH, dW, PA_GAUSS_S);
    gauss_sep(p->sm_a,   p->sm_b, dH, dW, PA_GAUSS_S);
    upsample(p->sm_tmp, p->flow_x, dH, dW, H, W);
    upsample(p->sm_a,   p->flow_y, dH, dW, H, W);
}

// Warp float RGB planes using half-flow (bilinear remap)
static void warp_rgb_planes(float *dst_r, float *dst_g, float *dst_b,
                             const float *src_r, const float *src_g, const float *src_b,
                             const float *flow_x, const float *flow_y,
                             int W, int H, float flow_sign)
{
    for (int y = 0; y < H; y++) {
        for (int x = 0; x < W; x++) {
            int idx = y * W + x;
            float fx = flow_x[idx] * 0.5f * flow_sign;
            float fy = flow_y[idx] * 0.5f * flow_sign;
            float sx = (float)x + fx;
            float sy = (float)y + fy;
            dst_r[idx] = sample_bilinear_f(src_r, W, H, sx, sy);
            dst_g[idx] = sample_bilinear_f(src_g, W, H, sx, sy);
            dst_b[idx] = sample_bilinear_f(src_b, W, H, sx, sy);
        }
    }
}

// Pack warped prev+curr RGB into 6ch NHWC float32 for QNN input
static void pack_nhwc6(float *nhwc, const float *r0, const float *g0, const float *b0,
                        const float *r1, const float *g1, const float *b1,
                        int W, int H)
{
    for (int y = 0; y < H; y++) {
        for (int x = 0; x < W; x++) {
            int idx = y * W + x;
            float *out = nhwc + idx * 6;
            out[0] = r0[idx]; out[1] = g0[idx]; out[2] = b0[idx];
            out[3] = r1[idx]; out[4] = g1[idx]; out[5] = b1[idx];
        }
    }
}

// Compute prealigned blend + QNN residual → output RGB
// If no QNN, just does blend. Output is float RGB [0,1].
static void compute_interpolated(struct priv *p, struct mp_filter *f,
                                  struct mp_image *prev, struct mp_image *curr,
                                  const AVMotionVector *mvs, int n_mvs,
                                  int W, int H)
{
    alloc_workspace(p, W, H);

    // Build dense flow from MVs
    zoh_fill(mvs, n_mvs, p->flow_x, p->flow_y, H, W);

    // Prealign v2: downsample → median → gaussian → upsample
    run_prealign_v2(p, W, H);

    // Convert both frames from YUV420P to float RGB
    yuv420p_to_rgb_float(prev->planes[0], prev->stride[0],
                          prev->planes[1], prev->stride[1],
                          prev->planes[2], prev->stride[2],
                          p->rgb_prev[0], p->rgb_prev[1], p->rgb_prev[2],
                          W, H);
    yuv420p_to_rgb_float(curr->planes[0], curr->stride[0],
                          curr->planes[1], curr->stride[1],
                          curr->planes[2], curr->stride[2],
                          p->rgb_curr[0], p->rgb_curr[1], p->rgb_curr[2],
                          W, H);

    // Warp prev backward by -flow/2, curr forward by +flow/2
    warp_rgb_planes(p->rgb_warp0[0], p->rgb_warp0[1], p->rgb_warp0[2],
                    p->rgb_prev[0],  p->rgb_prev[1],  p->rgb_prev[2],
                    p->flow_x, p->flow_y, W, H, -1.0f);
    warp_rgb_planes(p->rgb_warp1[0], p->rgb_warp1[1], p->rgb_warp1[2],
                    p->rgb_curr[0],  p->rgb_curr[1],  p->rgb_curr[2],
                    p->flow_x, p->flow_y, W, H, +1.0f);

    // Prealigned blend: (warp0 + warp1) / 2
    int px = W * H;
    for (int c = 0; c < 3; c++) {
        for (int i = 0; i < px; i++) {
            float *w0 = c == 0 ? p->rgb_warp0[0] : (c == 1 ? p->rgb_warp0[1] : p->rgb_warp0[2]);
            float *w1 = c == 0 ? p->rgb_warp1[0] : (c == 1 ? p->rgb_warp1[1] : p->rgb_warp1[2]);
            p->rgb_out[c][i] = (w0[i] + w1[i]) * 0.5f;
        }
    }

    // Try QNN inference for residual
    if (p->qnn.ready) {
        pack_nhwc6(p->qnn.input_buf,
                   p->rgb_warp0[0], p->rgb_warp0[1], p->rgb_warp0[2],
                   p->rgb_warp1[0], p->rgb_warp1[1], p->rgb_warp1[2],
                   W, H);

        Qnn_ErrorHandle_t err = qnn_execute(&p->qnn);
        if (err == QNN_SUCCESS) {
            // Add 3ch NHWC residual to blend
            const float *res = p->qnn.output_buf;
            for (int y = 0; y < H; y++) {
                for (int x = 0; x < W; x++) {
                    int idx = y * W + x;
                    const float *r = res + idx * 3;
                    p->rgb_out[0][idx] += r[0];
                    p->rgb_out[1][idx] += r[1];
                    p->rgb_out[2][idx] += r[2];
                }
            }
        } else {
            MP_WARN(f, "QNN: graphExecute failed (0x%lx), using blend only\n",
                    (unsigned long)err);
        }
    }

    // Clamp to [0,1]
    for (int c = 0; c < 3; c++) {
        for (int i = 0; i < px; i++) {
            if (p->rgb_out[c][i] < 0.0f) p->rgb_out[c][i] = 0.0f;
            if (p->rgb_out[c][i] > 1.0f) p->rgb_out[c][i] = 1.0f;
        }
    }
}

// Build the interpolated output frame with split-line overlay:
//   Left of split:  prev original (held still = 30fps)
//   Right of split: ANVIL interpolated
static struct mp_image *build_interp_frame(struct priv *p,
                                            struct mp_image *prev,
                                            struct mp_image *curr,
                                            int W, int H)
{
    struct mp_image *out = mp_image_new_copy(prev);
    float split = p->opts->split;
    if (split < 0.0f) split = 0.0f;
    if (split > 1.0f) split = 1.0f;
    int split_x = (int)(split * W);
    if (split_x < 0) split_x = 0;
    if (split_x > W) split_x = W;

    // Left side: prev original (already there from mp_image_new_copy)
    // Right side: convert ANVIL interpolated RGB back to YUV420P
    // We do this by writing the full interpolated result to a temp frame,
    // then copying the right portion.

    // Convert interpolated float RGB to YUV into the output frame's right half
    // Strategy: write full YUV from RGB, then restore left half from prev
    struct mp_image *interp_yuv = mp_image_new_copy(prev);

    rgb_float_to_yuv420p(p->rgb_out[0], p->rgb_out[1], p->rgb_out[2],
                          interp_yuv->planes[0], interp_yuv->stride[0],
                          interp_yuv->planes[1], interp_yuv->stride[1],
                          interp_yuv->planes[2], interp_yuv->stride[2],
                          W, H);

    // Copy right half from interp_yuv into out
    int n_planes = out->fmt.num_planes;
    if (n_planes > 3) n_planes = 3;

    for (int pl = 0; pl < n_planes; pl++) {
        int pw = mp_image_plane_w(out, pl);
        int ph = mp_image_plane_h(out, pl);
        int split_px = split_x;
        if (pl > 0) split_px = split_x >> 1;  // chroma subsampling

        for (int y = 0; y < ph; y++) {
            uint8_t *dst = out->planes[pl] + y * out->stride[pl];
            const uint8_t *src = interp_yuv->planes[pl] + y * interp_yuv->stride[pl];
            for (int x = split_px; x < pw; x++)
                dst[x] = src[x];
        }
    }

    mp_image_unrefp(&interp_yuv);

    // Draw 2px white vertical line at split position on Y plane
    if (split_x > 0 && split_x < W) {
        for (int y = 0; y < H; y++) {
            out->planes[0][y * out->stride[0] + split_x] = 255;
            if (split_x + 1 < W)
                out->planes[0][y * out->stride[0] + split_x + 1] = 255;
        }
        // Chroma: set to neutral (128) at split line
        int cW = W >> 1;
        int cH = H >> 1;
        int cx = split_x >> 1;
        if (cx > 0 && cx < cW) {
            for (int y = 0; y < cH; y++) {
                out->planes[1][y * out->stride[1] + cx] = 128;
                out->planes[2][y * out->stride[2] + cx] = 128;
            }
        }
    }

    return out;
}

static void f_process(struct mp_filter *f)
{
    struct priv *p = f->priv;

    if (p->state == STATE_HAVE_INTERP) {
        // Output the stored interpolated frame
        if (!mp_pin_in_needs_data(f->ppins[1]))
            return;

        struct mp_image *out = p->interp;
        p->interp = NULL;
        out->pts = p->interp_pts;
        mp_pin_in_write(f->ppins[1], MAKE_FRAME(MP_FRAME_VIDEO, out));
        p->state = STATE_NEED_INPUT;
        return;
    }

    // STATE_NEED_INPUT: read a frame from input
    if (!mp_pin_can_transfer_data(f->ppins[1], f->ppins[0]))
        return;

    struct mp_frame frame = mp_pin_out_read(f->ppins[0]);

    if (mp_frame_is_signaling(frame)) {
        mp_pin_in_write(f->ppins[1], frame);
        return;
    }
    if (frame.type != MP_FRAME_VIDEO) {
        mp_pin_in_write(f->ppins[1], frame);
        return;
    }

    struct mp_image *mpi = frame.data;
    p->frame_count++;
    int W = mpi->w, H = mpi->h;

    // Initialize QNN on first frame
    if (!p->qnn_checked) {
        p->qnn_checked = 1;
        if (qnn_init(&p->qnn, f, W, H) == 0) {
            MP_INFO(f, "ANVIL: QNN HTP ready at %s\n", qnn_get_dir());
        } else {
            MP_INFO(f, "ANVIL: QNN not available, using MV blend fallback\n");
        }
    }

    // Get MVs from side data
    const AVMotionVector *mvs = NULL;
    int n_mvs = 0;
    for (int n = 0; n < mpi->num_ff_side_data; n++) {
        if (mpi->ff_side_data[n].type == AV_FRAME_DATA_MOTION_VECTORS) {
            AVBufferRef *buf = mpi->ff_side_data[n].buf;
            if (buf && buf->data) {
                mvs = (const AVMotionVector *)buf->data;
                n_mvs = buf->size / sizeof(AVMotionVector);
            }
            break;
        }
    }

    if (p->frame_count % 60 == 1) {
        MP_INFO(f, "frame %d: %dx%d, %d MVs, qnn=%d\n",
                p->frame_count, W, H, n_mvs, p->qnn.ready);
    }

    // Need prev + MVs for interpolation
    if (!p->prev || n_mvs == 0) {
        mp_image_unrefp(&p->prev);
        p->prev = mp_image_new_ref(mpi);
        // Pass through original frame (no interpolated frame to emit)
        mp_pin_in_write(f->ppins[1], frame);
        return;
    }

    // Compute interpolated frame between prev and curr
    compute_interpolated(p, f, p->prev, mpi, mvs, n_mvs, W, H);

    // Build the split-screen interpolated frame
    p->interp = build_interp_frame(p, p->prev, mpi, W, H);
    p->interp_pts = (p->prev->pts + mpi->pts) / 2.0;

    // Update prev
    mp_image_unrefp(&p->prev);
    p->prev = mp_image_new_ref(mpi);

    // Output original curr frame first (unmodified)
    mp_pin_in_write(f->ppins[1], frame);

    // Transition: next call will output interpolated frame
    p->state = STATE_HAVE_INTERP;
    mp_filter_internal_mark_progress(f);
}

static void f_reset(struct mp_filter *f)
{
    struct priv *p = f->priv;
    mp_image_unrefp(&p->prev);
    mp_image_unrefp(&p->interp);
    p->state = STATE_NEED_INPUT;
}

static void f_destroy(struct mp_filter *f)
{
    struct priv *p = f->priv;
    mp_image_unrefp(&p->prev);
    mp_image_unrefp(&p->interp);
    free_workspace(p);
    qnn_cleanup(&p->qnn);
}

static const struct mp_filter_info filter = {
    .name = "anvil",
    .process = f_process,
    .reset = f_reset,
    .destroy = f_destroy,
    .priv_size = sizeof(struct priv),
};

// ════════════════════════════════════════════════════════════════════
// Section 6: Filter registration
// ════════════════════════════════════════════════════════════════════

static struct mp_filter *f_create(struct mp_filter *parent, void *options)
{
    struct mp_filter *f = mp_filter_create(parent, &filter);
    if (!f) {
        talloc_free(options);
        return NULL;
    }

    struct priv *p = f->priv;
    p->opts = talloc_ptrtype(f, p->opts);
    if (options) {
        *p->opts = *(struct anvil_opts *)options;
    } else {
        p->opts->split = 0.5f;
    }
    talloc_free(options);

    p->state = STATE_NEED_INPUT;

    mp_filter_add_pin(f, MP_PIN_IN, "in");
    mp_filter_add_pin(f, MP_PIN_OUT, "out");

    MP_INFO(f, "ANVIL VFI frame-doubler: split=%.2f (left=30fps hold, right=ANVIL 60fps)\n",
            p->opts->split);
    return f;
}

#define OPT_BASE_STRUCT struct anvil_opts
static const m_option_t vf_opts_fields[] = {
    {"split", OPT_FLOAT(split), M_RANGE(0.0, 1.0)},
    {0}
};

const struct mp_user_filter_entry vf_anvil = {
    .desc = {
        .description = "ANVIL video frame interpolation",
        .name = "anvil",
        .priv_size = sizeof(OPT_BASE_STRUCT),
        .priv_defaults = &(const OPT_BASE_STRUCT){
            .split = 0.5f,
        },
        .options = vf_opts_fields,
    },
    .create = f_create,
};
