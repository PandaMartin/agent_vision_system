"""把上位机积木链生成为 model.mod 里的预处理。

整帧工作区不放在 4MB 模块里，而用模块区后面的空闲 DDR：
0x21400000 起 8MB。摄像头帧是这块缓冲的输入，不能原地改。
"""
from __future__ import annotations

OPS_BASE = 0x21400000
OPS_BYTES = 0x00800000
OPS_MAX_W = 1280
OPS_MAX_H = 720

_RUNTIME = r"""
extern "C" void nn_wdt_kick(void);
extern "C" float powf(float, float);

static const unsigned OPS_BASE_ADDR = 0x21400000u;
static const int OPS_MAX_PIX = 1280 * 720;

static unsigned char* ops_rgb() { return (unsigned char*)OPS_BASE_ADDR; }
static unsigned char* ops_tmp() { return ops_rgb() + OPS_MAX_PIX * 3; }

static int clipu(int v)
{
    if (v < 0) return 0;
    if (v > 255) return 255;
    return v;
}

static int ref101(int p, int n)
{
    if (n <= 1) return 0;
    while (p < 0 || p >= n)
    {
        if (p < 0) p = -p;
        else p = 2 * n - 2 - p;
    }
    return p;
}

static int luma_at(const unsigned char* p)
{
    return (int)(0.299f * p[0] + 0.587f * p[1] + 0.114f * p[2] + 0.5f);
}

static void op_invert(unsigned char* img, int w, int h)
{
    const int n = w * h * 3;
    for (int i = 0; i < n; i++)
    {
        img[i] = (unsigned char)(255 - img[i]);
        if ((i & 65535) == 0) nn_wdt_kick();
    }
}

static void op_brightness(unsigned char* img, int w, int h, int beta)
{
    const int n = w * h * 3;
    for (int i = 0; i < n; i++)
    {
        img[i] = (unsigned char)clipu((int)img[i] + beta);
        if ((i & 65535) == 0) nn_wdt_kick();
    }
}

static void op_contrast(unsigned char* img, int w, int h, float alpha)
{
    const int n = w * h * 3;
    for (int i = 0; i < n; i++)
    {
        img[i] = (unsigned char)clipu((int)(img[i] * alpha + 0.5f));
        if ((i & 65535) == 0) nn_wdt_kick();
    }
}

static void op_gray(unsigned char* img, int w, int h)
{
    const int n = w * h;
    for (int i = 0; i < n; i++)
    {
        unsigned char* p = img + i * 3;
        const int y = luma_at(p);
        p[0] = p[1] = p[2] = (unsigned char)y;
        if ((i & 32767) == 0) nn_wdt_kick();
    }
}

static void op_white_balance(unsigned char* img, int w, int h, int manual,
                             float gr, float gg, float gb)
{
    float r = gr, g = gg, b = gb;
    if (!manual)
    {
        double sr = 0, sg = 0, sb = 0;
        const int n = w * h;
        for (int i = 0; i < n; i++)
        {
            sr += img[i * 3];
            sg += img[i * 3 + 1];
            sb += img[i * 3 + 2];
        }
        sr = sr / n; sg = sg / n; sb = sb / n;
        if (sr < 1) sr = 1;
        if (sg < 1) sg = 1;
        if (sb < 1) sb = 1;
        const double gray = (sr + sg + sb) / 3.0;
        r = (float)(gray / sr);
        g = (float)(gray / sg);
        b = (float)(gray / sb);
    }
    const int n = w * h;
    for (int i = 0; i < n; i++)
    {
        unsigned char* p = img + i * 3;
        p[0] = (unsigned char)clipu((int)(p[0] * r + 0.5f));
        p[1] = (unsigned char)clipu((int)(p[1] * g + 0.5f));
        p[2] = (unsigned char)clipu((int)(p[2] * b + 0.5f));
        if ((i & 32767) == 0) nn_wdt_kick();
    }
}

static void gauss_taps(int k, float sigma, float* ker)
{
    if (sigma <= 0.f)
        sigma = 0.3f * (((k - 1) * 0.5f) - 1.f) + 0.8f;
    const int c = k / 2;
    float sum = 0.f;
    for (int i = 0; i < k; i++)
    {
        const float x = (float)(i - c);
        ker[i] = expf(-(x * x) / (2.f * sigma * sigma));
        sum += ker[i];
    }
    for (int i = 0; i < k; i++)
        ker[i] /= sum;
}

static void conv_sep(unsigned char* img, int w, int h, const float* k, int ksz)
{
    unsigned char* tmp = ops_tmp();
    const int rad = ksz / 2;
    for (int y = 0; y < h; y++)
    {
        if ((y & 7) == 0) nn_wdt_kick();
        for (int x = 0; x < w; x++)
        {
            for (int c = 0; c < 3; c++)
            {
                float s = 0.f;
                for (int i = 0; i < ksz; i++)
                {
                    const int xx = ref101(x + i - rad, w);
                    s += k[i] * img[(y * w + xx) * 3 + c];
                }
                tmp[(y * w + x) * 3 + c] = (unsigned char)clipu((int)(s + 0.5f));
            }
        }
    }
    for (int y = 0; y < h; y++)
    {
        if ((y & 7) == 0) nn_wdt_kick();
        for (int x = 0; x < w; x++)
        {
            for (int c = 0; c < 3; c++)
            {
                float s = 0.f;
                for (int i = 0; i < ksz; i++)
                {
                    const int yy = ref101(y + i - rad, h);
                    s += k[i] * tmp[(yy * w + x) * 3 + c];
                }
                img[(y * w + x) * 3 + c] = (unsigned char)clipu((int)(s + 0.5f));
            }
        }
    }
}

static void op_box(unsigned char* img, int w, int h, int k)
{
    if (k < 1) k = 1;
    if ((k & 1) == 0) k += 1;
    if (k > 21) k = 21;
    float ker[21];
    const float v = 1.f / (float)k;
    for (int i = 0; i < k; i++) ker[i] = v;
    conv_sep(img, w, h, ker, k);
}

static void op_gaussian(unsigned char* img, int w, int h, int k, float sigma)
{
    if (k < 1) k = 1;
    if ((k & 1) == 0) k += 1;
    if (k > 21) k = 21;
    float ker[21];
    gauss_taps(k, sigma, ker);
    conv_sep(img, w, h, ker, k);
}

static void op_sharpen(unsigned char* img, int w, int h, float amount, int radius)
{
    int k = radius;
    if (k < 1) k = 1;
    if ((k & 1) == 0) k += 1;
    if (k > 21) k = 21;
    float ker[21];
    gauss_taps(k, 0.f, ker);
    unsigned char* tmp = ops_tmp();
    const int rad = k / 2;
    for (int y = 0; y < h; y++)
    {
        if ((y & 7) == 0) nn_wdt_kick();
        for (int x = 0; x < w; x++)
            for (int c = 0; c < 3; c++)
            {
                float s = 0.f;
                for (int i = 0; i < k; i++)
                    s += ker[i] * img[(y * w + ref101(x + i - rad, w)) * 3 + c];
                tmp[(y * w + x) * 3 + c] = (unsigned char)clipu((int)(s + 0.5f));
            }
    }
    for (int y = 0; y < h; y++)
    {
        if ((y & 7) == 0) nn_wdt_kick();
        for (int x = 0; x < w; x++)
            for (int c = 0; c < 3; c++)
            {
                float blur = 0.f;
                for (int i = 0; i < k; i++)
                    blur += ker[i] * tmp[(ref101(y + i - rad, h) * w + x) * 3 + c];
                const int i = (y * w + x) * 3 + c;
                const float s = img[i] * (1.f + amount) - blur * amount;
                img[i] = (unsigned char)clipu((int)(s + 0.5f));
            }
    }
}

static void paint_gray(unsigned char* img, int i, int y)
{
    unsigned char* p = img + i * 3;
    const unsigned char v = (unsigned char)y;
    p[0] = p[1] = p[2] = v;
}

static int otsu_level(const int* hist, int total)
{
    double sum = 0;
    for (int i = 0; i < 256; i++) sum += (double)i * hist[i];
    double sum_b = 0;
    int w_b = 0;
    double best = -1;
    int level = 0;
    for (int t = 0; t < 256; t++)
    {
        w_b += hist[t];
        if (w_b == 0) continue;
        const int w_f = total - w_b;
        if (w_f == 0) break;
        sum_b += (double)t * hist[t];
        const double m_b = sum_b / w_b;
        const double m_f = (sum - sum_b) / w_f;
        const double var = (double)w_b * (double)w_f * (m_b - m_f) * (m_b - m_f);
        if (var > best)
        {
            best = var;
            level = t;
        }
    }
    return level;
}

static void op_binary(unsigned char* img, int w, int h, int method, int thresh, int block)
{
    const int n = w * h;
    if (method == 2)
    {
        if (block < 3) block = 3;
        if ((block & 1) == 0) block += 1;
        const int rad = block / 2;
        unsigned char* src = ops_tmp();
        for (int i = 0; i < n * 3; i++) src[i] = img[i];
        for (int y = 0; y < h; y++)
        {
            if ((y & 3) == 0) nn_wdt_kick();
            for (int x = 0; x < w; x++)
            {
                int acc = 0, cnt = 0;
                for (int dy = -rad; dy <= rad; dy++)
                {
                    const int yy = ref101(y + dy, h);
                    for (int dx = -rad; dx <= rad; dx++)
                    {
                        const int xx = ref101(x + dx, w);
                        acc += luma_at(src + (yy * w + xx) * 3);
                        cnt++;
                    }
                }
                const int yv = luma_at(src + (y * w + x) * 3);
                const int mean = acc / cnt;
                paint_gray(img, y * w + x, yv > mean - 2 ? 255 : 0);
            }
        }
        return;
    }
    int level = thresh;
    if (method != 1)
    {
        int hist[256];
        for (int i = 0; i < 256; i++) hist[i] = 0;
        for (int i = 0; i < n; i++) hist[luma_at(img + i * 3)]++;
        level = otsu_level(hist, n);
    }
    for (int i = 0; i < n; i++)
    {
        const int yv = luma_at(img + i * 3);
        paint_gray(img, i, yv > level ? 255 : 0);
    }
}

static void sample_grad(const unsigned char* img, int w, int h, int x, int y,
                        int* gx, int* gy)
{
    int sx = 0, sy = 0;
    for (int dy = -1; dy <= 1; dy++)
    {
        const int yy = ref101(y + dy, h);
        for (int dx = -1; dx <= 1; dx++)
        {
            const int xx = ref101(x + dx, w);
            const int yv = luma_at(img + (yy * w + xx) * 3);
            const int kx = (dx == 0) ? 0 : dx;
            const int ky = (dy == 0) ? 0 : dy;
            const int wy = (dy == 0) ? 2 : 1;
            const int wx = (dx == 0) ? 2 : 1;
            sx += kx * wy * yv;
            sy += ky * wx * yv;
        }
    }
    *gx = sx;
    *gy = sy;
}

static void op_sobel(unsigned char* img, int w, int h, int axis)
{
    const int n = w * h;
    unsigned short* mag = (unsigned short*)ops_tmp();
    int mn = 65535, mx = 0;
    for (int y = 0; y < h; y++)
    {
        if ((y & 7) == 0) nn_wdt_kick();
        for (int x = 0; x < w; x++)
        {
            int gx = 0, gy = 0;
            sample_grad(img, w, h, x, y, &gx, &gy);
            if (axis == 1) gy = 0;
            if (axis == 2) gx = 0;
            const float sq = (float)gx * (float)gx + (float)gy * (float)gy;
            int m = 0;
            if (sq > 0.f)
                m = (int)(powf(sq, 0.5f) + 0.5f);
            if (m > 65535) m = 65535;
            mag[y * w + x] = (unsigned short)m;
            if (m < mn) mn = m;
            if (m > mx) mx = m;
        }
    }
    for (int i = 0; i < n; i++)
    {
        int v = 0;
        if (mx > mn)
            v = (int)(((int)mag[i] - mn) * 255 / (mx - mn));
        paint_gray(img, i, clipu(v));
    }
}

static void op_canny(unsigned char* img, int w, int h, int low, int high)
{
    const int n = w * h;
    unsigned short* mag = (unsigned short*)ops_tmp();
    unsigned char* edge = (unsigned char*)(mag + n);
    for (int y = 0; y < h; y++)
    {
        if ((y & 7) == 0) nn_wdt_kick();
        for (int x = 0; x < w; x++)
        {
            int gx = 0, gy = 0;
            sample_grad(img, w, h, x, y, &gx, &gy);
            int m = (gx < 0 ? -gx : gx) + (gy < 0 ? -gy : gy);
            if (m > 65535) m = 65535;
            mag[y * w + x] = (unsigned short)m;
        }
    }
    if (high < low + 1) high = low + 1;
    for (int y = 0; y < h; y++)
    {
        for (int x = 0; x < w; x++)
        {
            int gx = 0, gy = 0;
            sample_grad(img, w, h, x, y, &gx, &gy);
            const int ax = gx < 0 ? -gx : gx;
            const int ay = gy < 0 ? -gy : gy;
            int dx = 1, dy = 0;
            if (ay > ax)
            {
                dx = 0; dy = 1;
            }
            else if (ax > 0 && ay * 2 > ax)
            {
                dx = 1; dy = (gy < 0) == (gx < 0) ? 1 : -1;
            }
            const int m0 = mag[y * w + x];
            const int m1 = mag[ref101(y + dy, h) * w + ref101(x + dx, w)];
            const int m2 = mag[ref101(y - dy, h) * w + ref101(x - dx, w)];
            int kind = 0;
            if (m0 >= m1 && m0 >= m2)
            {
                if (m0 >= high) kind = 255;
                else if (m0 >= low) kind = 128;
            }
            edge[y * w + x] = (unsigned char)kind;
        }
    }
    int changed = 1;
    int guard = w + h;
    while (changed && guard-- > 0)
    {
        changed = 0;
        nn_wdt_kick();
        for (int y = 0; y < h; y++)
        {
            for (int x = 0; x < w; x++)
            {
                if (edge[y * w + x] != 128) continue;
                int strong = 0;
                for (int dy = -1; dy <= 1 && !strong; dy++)
                    for (int dx = -1; dx <= 1; dx++)
                        if (edge[ref101(y + dy, h) * w + ref101(x + dx, w)] == 255)
                            strong = 1;
                if (strong)
                {
                    edge[y * w + x] = 255;
                    changed = 1;
                }
            }
        }
    }
    for (int i = 0; i < n; i++)
        paint_gray(img, i, edge[i] == 255 ? 255 : 0);
}

static void ycrcb_of(const unsigned char* p, int* Y, int* Cr, int* Cb)
{
    const float r = p[0], g = p[1], b = p[2];
    const float y = 0.299f * r + 0.587f * g + 0.114f * b;
    *Y = clipu((int)(y + 0.5f));
    *Cr = clipu((int)((r - y) * 0.713f + 128.5f));
    *Cb = clipu((int)((b - y) * 0.564f + 128.5f));
}

static void ycrcb_to_rgb(int Y, int Cr, int Cb, unsigned char* p)
{
    const float y = (float)Y;
    const float cr = (float)(Cr - 128);
    const float cb = (float)(Cb - 128);
    p[0] = (unsigned char)clipu((int)(y + 1.403f * cr + 0.5f));
    p[1] = (unsigned char)clipu((int)(y - 0.714f * cr - 0.344f * cb + 0.5f));
    p[2] = (unsigned char)clipu((int)(y + 1.773f * cb + 0.5f));
}

static void op_hist_eq(unsigned char* img, int w, int h)
{
    const int n = w * h;
    int hist[256];
    for (int i = 0; i < 256; i++) hist[i] = 0;
    for (int i = 0; i < n; i++)
    {
        int Y, Cr, Cb;
        ycrcb_of(img + i * 3, &Y, &Cr, &Cb);
        hist[Y]++;
    }
    int cdf[256];
    int acc = 0, cdf_min = 0, seen = 0;
    for (int i = 0; i < 256; i++)
    {
        acc += hist[i];
        cdf[i] = acc;
        if (!seen && hist[i])
        {
            cdf_min = acc;
            seen = 1;
        }
    }
    unsigned char lut[256];
    const int denom = n - cdf_min;
    for (int i = 0; i < 256; i++)
    {
        if (denom <= 0) lut[i] = (unsigned char)i;
        else lut[i] = (unsigned char)clipu((int)(((cdf[i] - cdf_min) * 255.0f) / (float)denom + 0.5f));
    }
    for (int i = 0; i < n; i++)
    {
        int Y, Cr, Cb;
        ycrcb_of(img + i * 3, &Y, &Cr, &Cb);
        ycrcb_to_rgb(lut[Y], Cr, Cb, img + i * 3);
        if ((i & 32767) == 0) nn_wdt_kick();
    }
}

static float srgb_to_lin(float c)
{
    c /= 255.f;
    if (c <= 0.04045f) return c / 12.92f;
    return powf((c + 0.055f) / 1.055f, 2.4f);
}

static float lin_to_srgb(float c)
{
    if (c <= 0.0031308f) c *= 12.92f;
    else c = 1.055f * powf(c > 0.f ? c : 0.f, 1.f / 2.4f) - 0.055f;
    return c * 255.f;
}

static float lab_f(float t)
{
    const float d = 6.f / 29.f;
    if (t > d * d * d) return powf(t > 0.f ? t : 0.f, 1.f / 3.f);
    return t / (3.f * d * d) + 4.f / 29.f;
}

static float lab_finv(float t)
{
    const float d = 6.f / 29.f;
    if (t > d) return t * t * t;
    return 3.f * d * d * (t - 4.f / 29.f);
}

static void rgb_to_lab(const unsigned char* p, int* L, int* A, int* B)
{
    const float r = srgb_to_lin(p[0]);
    const float g = srgb_to_lin(p[1]);
    const float b = srgb_to_lin(p[2]);
    float X = r * 0.412453f + g * 0.357580f + b * 0.180423f;
    float Y = r * 0.212671f + g * 0.715160f + b * 0.072169f;
    float Z = r * 0.019334f + g * 0.119193f + b * 0.950227f;
    X /= 0.950456f;
    Z /= 1.088754f;
    const float fx = lab_f(X);
    const float fy = lab_f(Y);
    const float fz = lab_f(Z);
    *L = clipu((int)((116.f * fy - 16.f) * 2.55f + 0.5f));
    *A = clipu((int)(500.f * (fx - fy) + 128.5f));
    *B = clipu((int)(200.f * (fy - fz) + 128.5f));
}

static void lab_to_rgb(int L, int A, int Bch, unsigned char* p)
{
    const float fy = ((float)L / 2.55f + 16.f) / 116.f;
    const float fx = fy + ((float)A - 128.f) / 500.f;
    const float fz = fy - ((float)Bch - 128.f) / 200.f;
    const float X = lab_finv(fx) * 0.950456f;
    const float Y = lab_finv(fy);
    const float Z = lab_finv(fz) * 1.088754f;
    const float r = 3.240479f * X - 1.537150f * Y - 0.498535f * Z;
    const float g = -0.969256f * X + 1.875992f * Y + 0.041556f * Z;
    const float b = 0.055648f * X - 0.204043f * Y + 1.057311f * Z;
    p[0] = (unsigned char)clipu((int)(lin_to_srgb(r) + 0.5f));
    p[1] = (unsigned char)clipu((int)(lin_to_srgb(g) + 0.5f));
    p[2] = (unsigned char)clipu((int)(lin_to_srgb(b) + 0.5f));
}

static void op_clahe(unsigned char* img, int w, int h, float clip, int grid)
{
    if (grid < 2) grid = 2;
    if (grid > 8) grid = 8;
    const int n = w * h;
    unsigned char* plane = ops_tmp();
    unsigned char* cr = plane + n;
    unsigned char* cb = cr + n;
    for (int i = 0; i < n; i++)
    {
        int Y, Cr, Cb;
        rgb_to_lab(img + i * 3, &Y, &Cr, &Cb);
        plane[i] = (unsigned char)Y;
        cr[i] = (unsigned char)Cr;
        cb[i] = (unsigned char)Cb;
    }
    const int tw = (w + grid - 1) / grid;
    const int th = (h + grid - 1) / grid;
    unsigned char lut[8 * 8 * 256];
    for (int ty = 0; ty < grid; ty++)
    {
        for (int tx = 0; tx < grid; tx++)
        {
            nn_wdt_kick();
            int hist[256];
            for (int i = 0; i < 256; i++) hist[i] = 0;
            const int x0 = tx * tw;
            const int y0 = ty * th;
            const int x1 = x0 + tw < w ? x0 + tw : w;
            const int y1 = y0 + th < h ? y0 + th : h;
            int count = 0;
            for (int y = y0; y < y1; y++)
                for (int x = x0; x < x1; x++)
                {
                    hist[plane[y * w + x]]++;
                    count++;
                }
            if (count < 1) count = 1;
            int limit = (int)(clip * (float)count / 256.f);
            if (limit < 1) limit = 1;
            int excess = 0;
            for (int i = 0; i < 256; i++)
                if (hist[i] > limit)
                {
                    excess += hist[i] - limit;
                    hist[i] = limit;
                }
            const int add = excess / 256;
            int rem = excess % 256;
            for (int i = 0; i < 256; i++) hist[i] += add;
            for (int i = 0; i < rem; i++) hist[(i * 255) / (rem > 1 ? rem : 1)]++;
            int acc = 0;
            unsigned char* tile = lut + (ty * grid + tx) * 256;
            for (int i = 0; i < 256; i++)
            {
                acc += hist[i];
                tile[i] = (unsigned char)clipu((int)(acc * 255.f / (float)count + 0.5f));
            }
        }
    }
    for (int y = 0; y < h; y++)
    {
        if ((y & 7) == 0) nn_wdt_kick();
        for (int x = 0; x < w; x++)
        {
            const float fx = (tw > 0) ? ((float)x / (float)tw - 0.5f) : 0.f;
            const float fy = (th > 0) ? ((float)y / (float)th - 0.5f) : 0.f;
            int tx0 = (int)fx;
            int ty0 = (int)fy;
            if (fx < 0) tx0 = 0;
            if (fy < 0) ty0 = 0;
            if (tx0 >= grid) tx0 = grid - 1;
            if (ty0 >= grid) ty0 = grid - 1;
            int tx1 = tx0 + 1;
            int ty1 = ty0 + 1;
            if (tx1 >= grid) tx1 = grid - 1;
            if (ty1 >= grid) ty1 = grid - 1;
            float ax = fx - (float)tx0;
            float ay = fy - (float)ty0;
            if (ax < 0) ax = 0;
            if (ay < 0) ay = 0;
            if (ax > 1) ax = 1;
            if (ay > 1) ay = 1;
            const int v = plane[y * w + x];
            const float l00 = lut[(ty0 * grid + tx0) * 256 + v];
            const float l10 = lut[(ty0 * grid + tx1) * 256 + v];
            const float l01 = lut[(ty1 * grid + tx0) * 256 + v];
            const float l11 = lut[(ty1 * grid + tx1) * 256 + v];
            const float l0 = l00 + (l10 - l00) * ax;
            const float l1 = l01 + (l11 - l01) * ax;
            const int Y = clipu((int)(l0 + (l1 - l0) * ay + 0.5f));
            lab_to_rgb(Y, cr[y * w + x], cb[y * w + x], img + (y * w + x) * 3);
        }
    }
}

static void op_median(unsigned char* img, int w, int h, int k)
{
    if (k < 1) k = 1;
    if ((k & 1) == 0) k += 1;
    if (k > 15) k = 15;
    unsigned char* src = ops_tmp();
    const int n = w * h * 3;
    for (int i = 0; i < n; i++) src[i] = img[i];
    const int rad = k / 2;
    const int need = k * k;
    unsigned char win[225];
    for (int y = 0; y < h; y++)
    {
        if ((y & 3) == 0) nn_wdt_kick();
        for (int x = 0; x < w; x++)
        {
            for (int c = 0; c < 3; c++)
            {
                int m = 0;
                for (int dy = -rad; dy <= rad; dy++)
                {
                    const int yy = ref101(y + dy, h);
                    for (int dx = -rad; dx <= rad; dx++)
                    {
                        const int xx = ref101(x + dx, w);
                        win[m++] = src[(yy * w + xx) * 3 + c];
                    }
                }
                for (int i = 1; i < need; i++)
                {
                    const unsigned char key = win[i];
                    int j = i - 1;
                    while (j >= 0 && win[j] > key)
                    {
                        win[j + 1] = win[j];
                        j--;
                    }
                    win[j + 1] = key;
                }
                img[(y * w + x) * 3 + c] = win[need / 2];
            }
        }
    }
}

static int apply_board_ops(const unsigned char* src, int w, int h, int src_rgb,
                           unsigned char** out)
{
    if (w <= 0 || h <= 0 || (long long)w * h > OPS_MAX_PIX)
    {
        xil_printf("[mod] ops: %dx%d exceeds 1280x720\r\n", w, h);
        return -1;
    }
    unsigned char* rgb = ops_rgb();
    const int n = w * h;
    for (int i = 0; i < n; i++)
    {
        const unsigned char* p = src + i * 3;
        unsigned char* d = rgb + i * 3;
        if (src_rgb)
        {
            d[0] = p[0]; d[1] = p[1]; d[2] = p[2];
        }
        else
        {
            d[0] = p[2]; d[1] = p[1]; d[2] = p[0];
        }
        if ((i & 32767) == 0) nn_wdt_kick();
    }
    @CALLS@
    *out = rgb;
    return 0;
}
"""


def _num(value, default=0.0) -> float:
    try:
        return float(value)
    except (TypeError, ValueError):
        return float(default)


def _cf(value) -> str:
    """C++ 浮点字面量。1 写成 1f 会变成自定义字面量，必须带小数点。"""
    text = f"{float(value):.8g}"
    if "." not in text and "e" not in text.lower():
        text += ".0"
    return text + "f"


def _call(step: dict) -> str:
    op = step["op"]
    p = step.get("params") or {}
    if op == "invert":
        return "op_invert(rgb, w, h);"
    if op == "brightness":
        return "op_brightness(rgb, w, h, %d);" % int(round(_num(p.get("beta"), 20)))
    if op == "contrast":
        return "op_contrast(rgb, w, h, %s);" % _cf(_num(p.get("alpha"), 1.3))
    if op == "grayscale":
        return "op_gray(rgb, w, h);"
    if op == "white_balance":
        manual = 1 if str(p.get("method") or "gray_world") == "manual" else 0
        return "op_white_balance(rgb, w, h, %d, %s, %s, %s);" % (
            manual, _cf(_num(p.get("r"), 1)), _cf(_num(p.get("g"), 1)), _cf(_num(p.get("b"), 1)))
    if op == "box_blur":
        return "op_box(rgb, w, h, %d);" % int(round(_num(p.get("ksize"), 5)))
    if op == "gaussian":
        return "op_gaussian(rgb, w, h, %d, %s);" % (
            int(round(_num(p.get("ksize"), 5))), _cf(_num(p.get("sigma"), 1)))
    if op == "sharpen":
        return "op_sharpen(rgb, w, h, %s, %d);" % (
            _cf(_num(p.get("amount"), 1.5)), int(round(_num(p.get("radius"), 3))))
    if op == "binary":
        method = str(p.get("method") or "otsu")
        code = {"fixed": 1, "adaptive": 2}.get(method, 0)
        return "op_binary(rgb, w, h, %d, %d, %d);" % (
            code, int(round(_num(p.get("thresh"), 127))), int(round(_num(p.get("block"), 11))))
    if op == "sobel":
        axis = {"x": 1, "y": 2}.get(str(p.get("axis") or "both"), 0)
        return "op_sobel(rgb, w, h, %d);" % axis
    if op == "canny":
        return "op_canny(rgb, w, h, %d, %d);" % (
            int(round(_num(p.get("low"), 50))), int(round(_num(p.get("high"), 150))))
    if op == "hist_eq":
        return "op_hist_eq(rgb, w, h);"
    if op == "clahe":
        return "op_clahe(rgb, w, h, %s, %d);" % (
            _cf(_num(p.get("clip"), 2)), int(round(_num(p.get("grid"), 8))))
    if op == "median":
        return "op_median(rgb, w, h, %d);" % int(round(_num(p.get("ksize"), 5)))
    raise ValueError(f"未知图像积木: {op}")


def render_ops_cxx(steps: list[dict]) -> str:
    calls = "\n    ".join(_call(step) for step in steps)
    if not calls:
        calls = "(void)rgb; (void)w; (void)h;"
    text = _RUNTIME.replace("@CALLS@", calls)
    # float literals like 1f are illegal; force a decimal point
    return text
