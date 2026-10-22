/* ---- QuickTime stand-in ---------------------------------------------- */
/* KPD3 reaches QuickTime through 161 small thunks (0x4D9910..0x4DB334):
       mov eax, <selector> ; jmp [off_586E2C]        (cdecl, caller cleans)
   With QuickTime absent, off_586E2C is a stub that only sets AX = F7D1, so
   every pointer-returning call hands back garbage like 0x001CF7D1. Pictures
   need QuickTime for one thing: the image inside a KPX ('tBMP'/'tTHM') is a
   plain JFIF JPEG behind a 24-byte header the game writes itself, and the
   game uses QuickTime's Image Compression Manager to turn its 16-bit
   offscreen bitmaps into that JPEG (save) and back (open, and every redraw).
   This stands in for the calls those paths make: Memory Manager handles,
   GWorlds over the game's own DIB sections, RectMatrix, and JPEG
   FCompressImage / FDecompressImage through Windows' WIC. Everything else
   (movies, Flash toons, sound conversion, picture import/export components)
   answers "not available" in a way each caller handles: errors as a
   sign-extended OSErr, NULL for pointer results, and out-parameters cleared.
   Selector meanings and caller behaviour were mapped in IDA. */
#define QT_THUNK_LOADER   ((void **)g_b->qtThunks)       /* 0x586E28 lazy loader; 0 = never try to load QuickTime */
#define QT_THUNK_DISPATCH (QT_THUNK_LOADER + 1)           /* theQuickTimeDispatcher */
#define QT_THUNK_CALLCOMP (QT_THUNK_LOADER + 2)           /* _CallComponent (set, never called) */
#define QT_THUNK_CCFWS    (QT_THUNK_LOADER + 3)           /* _CallComponentFunctionWithStorage (likewise) */
#define QT_INIT_COUNT     (*(volatile LONG *)g_b->qtInitCount)   /* 0x597DD0 InitializeQTML glue: > 0 = already up */
#define QT_LOAD_FN        g_b->qtLoadFn                   /* 0x454A60 LoadLibraryA("quicktime.qts") + InitializeQTML */
#define QT_QFX_ERASER_FN  g_b->qtQfxEraserFn              /* 0x4AD620 QuickTime-effect eraser animation (thiscall, no args) */

enum { noErr = 0, paramErr = -50, memFullErr = -108, fnfErr = -43, noCodecErr = -8961,
       qtNotAvailable = -2093, badComponentInstance = (int)0x80008001, gwFlagErr = (int)0x80000000 };

#pragma pack(push, 2)
typedef struct { short top, left, bottom, right; } QRect;
typedef struct {                    /* QuickDraw PixMap, as the game reads it (native endian) */
    BYTE  *baseAddr;                /* +0  */
    short  rowBytes;                /* +4  bit 15 set = PixMap */
    QRect  bounds;                  /* +6  */
    short  pmVersion, packType;     /* +14 +16 */
    long   packSize;                /* +18 */
    long   hRes, vRes;              /* +22 +26 */
    short  pixelType, pixelSize;    /* +30 +32 */
    short  cmpCount, cmpSize;       /* +34 +36 */
    DWORD  pixelFormat;             /* +38 */
    void  *pmTable, *pmExt;         /* +42 +46 */
} QPixMap;                          /* 50 bytes */
typedef struct {                    /* ImageDescription (86 bytes) */
    long  idSize; DWORD cType; long resvd1; short resvd2, dataRefIndex, version, revisionLevel;
    DWORD vendor; long temporalQuality, spatialQuality; short width, height;
    long  hRes, vRes, dataSize; short frameCount; BYTE name[32]; short depth, clutID;
} QImageDesc;
typedef struct { long m[3][3]; } QMatrix;  /* Fixed, except m[x][2] which are Fract */
#pragma pack(pop)

typedef struct QHandleBlock {       /* a Mac Handle is &block->master */
    void  *master;
    DWORD  size, magic;
} QHandleBlock;
#define QT_HANDLE_MAGIC 0x51544844  /* 'QTHD' */

typedef struct QGWorld {
    DWORD    magic;
    QPixMap  pm;
    QPixMap *pmMaster;              /* GetGWorldPixMap returns &pmMaster */
    HDC      dc;                    /* NewGWorldFromHBITMAP: the game's memory DC (freed here) */
    HBITMAP  bm;                    /* ... and its DIB section */
    void    *owned;                 /* NewGWorld/QTNewGWorld: our own pixel buffer */
} QGWorld;
#define QT_GWORLD_MAGIC 0x51544757  /* 'QTGW' */

static CRITICAL_SECTION g_qtLock;
static void  **g_qtObj;             /* live handles and GWorlds we made */
static int     g_qtObjN, g_qtObjCap;
static short   g_qtMemErr;
static void   *g_qtCurPort, *g_qtCurDev;
static BYTE    g_qtSeen[0x2600];    /* first-call logging, per (manager,index) */
static IWICImagingFactory *g_wic;

static const GUID qt_CLSID_WICFactory = {0xcacaf262,0x9370,0x4615,{0xa1,0x3b,0x9f,0x55,0x39,0xda,0x4c,0x0a}};
static const GUID qt_IID_WICFactory   = {0xec5ec8a9,0xc395,0x4314,{0x9c,0x77,0x54,0xd7,0xa9,0x35,0xff,0x70}};
static const GUID qt_GUID_Jpeg        = {0x19e4a5aa,0x5662,0x4fc5,{0xa0,0xc0,0x17,0x58,0x02,0x8e,0x10,0x57}};
static const GUID qt_GUID_24bppBGR    = {0x6fddc324,0x4e03,0x4bfe,{0xb1,0x85,0x3d,0x77,0x76,0x8d,0xc9,0x0c}};

static void qt_note_first(DWORD sel, const char *name, const char *fmt, ...)
{
    DWORD k = ((sel >> 16) & 0x3F) * 0x100 + (sel & 0xFF);
    char m[300];
    va_list va;
    if (g_log < 2 || (k < sizeof(g_qtSeen) && g_qtSeen[k] >= 2)) return;
    if (k < sizeof(g_qtSeen)) g_qtSeen[k]++;
    va_start(va, fmt);
    _vsnprintf(m, sizeof(m), fmt, va);
    va_end(va);
    m[sizeof(m) - 1] = 0;
    tracef("QT", 1, "%06lX %-26s %s", sel, name, m);
}

/* ---- object registry (the game passes stale or uninitialised values back) */
static int qt_reg_add(void *p)
{
    int ok;
    EnterCriticalSection(&g_qtLock);
    if (g_qtObjN == g_qtObjCap) {
        int cap = g_qtObjCap ? g_qtObjCap * 2 : 64;
        void **n = (void **)realloc(g_qtObj, cap * sizeof(void *));
        if (n) { g_qtObj = n; g_qtObjCap = cap; }
    }
    ok = g_qtObjN < g_qtObjCap;
    if (ok) g_qtObj[g_qtObjN++] = p;
    LeaveCriticalSection(&g_qtLock);
    return ok;
}

static int qt_reg_has(void *p)
{
    int i, r = 0;
    if (!p) return 0;
    EnterCriticalSection(&g_qtLock);
    for (i = 0; i < g_qtObjN; i++) if (g_qtObj[i] == p) { r = 1; break; }
    LeaveCriticalSection(&g_qtLock);
    return r;
}

static int qt_reg_del(void *p)
{
    int i, r = 0;
    if (!p) return 0;
    EnterCriticalSection(&g_qtLock);
    for (i = 0; i < g_qtObjN; i++)
        if (g_qtObj[i] == p) { g_qtObj[i] = g_qtObj[--g_qtObjN]; r = 1; break; }
    LeaveCriticalSection(&g_qtLock);
    return r;
}

static QHandleBlock *qt_handle_block(void *h)     /* NULL unless h is one of ours */
{
    QHandleBlock *b = (QHandleBlock *)h;          /* master is the first member */
    return (qt_reg_has(b) && b->magic == QT_HANDLE_MAGIC) ? b : NULL;
}

static QGWorld *qt_gworld(void *gw)
{
    QGWorld *g = (QGWorld *)gw;
    return (qt_reg_has(g) && g->magic == QT_GWORLD_MAGIC) ? g : NULL;
}

static QGWorld *qt_gworld_of_pm(void *pmh)        /* PixMapHandle -> its GWorld */
{
    QGWorld *g = pmh ? (QGWorld *)((BYTE *)pmh - offsetof(QGWorld, pmMaster)) : NULL;
    return (g && qt_reg_has(g) && g->magic == QT_GWORLD_MAGIC) ? g : NULL;
}

static QPixMap *qt_pixmap(void *pmh)              /* any PixMapHandle we can use */
{
    QGWorld *g = qt_gworld_of_pm(pmh);
    if (g) return &g->pm;
    return NULL;
}

/* ---- Memory Manager ---------------------------------------------------- */
static void *qt_new_handle(DWORD size, int clear)
{
    QHandleBlock *b = (QHandleBlock *)calloc(1, sizeof(QHandleBlock));
    void *mem = b ? (clear ? calloc(1, size ? size : 1) : malloc(size ? size : 1)) : NULL;
    if (!mem) { free(b); g_qtMemErr = memFullErr; return NULL; }
    if (!clear) memset(mem, 0, size);             /* the game relies on zeroed descriptors */
    b->master = mem; b->size = size; b->magic = QT_HANDLE_MAGIC;
    if (!qt_reg_add(b)) { free(mem); free(b); g_qtMemErr = memFullErr; return NULL; }
    g_qtMemErr = noErr;
    return &b->master;
}

static int __cdecl qt_NewHandle(int size)       { return (int)qt_new_handle((DWORD)size, 0); }
static int __cdecl qt_NewHandleClear(int size)  { return (int)qt_new_handle((DWORD)size, 1); }
static int __cdecl qt_MemError(void)            { return g_qtMemErr; }
static int __cdecl qt_Nop(void)                 { return 0; }

static int __cdecl qt_DisposeHandle(void *h)
{
    QHandleBlock *b = qt_handle_block(h);
    if (b && qt_reg_del(b)) { free(b->master); b->magic = 0; free(b); }
    g_qtMemErr = noErr;
    return 0;
}

static int __cdecl qt_GetHandleSize(void *h)
{
    QHandleBlock *b = qt_handle_block(h);
    return b ? (int)b->size : 0;
}

static int __cdecl qt_BlockMove(const void *src, void *dst, int n)   /* (src, dst, n) */
{
    if (src && dst && n > 0) memmove(dst, src, (size_t)n);
    return 0;
}

static int __cdecl qt_NewPtr(int size)
{
    void *p = calloc(1, size > 0 ? (size_t)size : 1);
    if (p && !qt_reg_add(p)) { free(p); p = NULL; }
    g_qtMemErr = p ? noErr : memFullErr;
    return (int)p;
}

static int __cdecl qt_DisposePtr(void *p)
{
    if (qt_reg_del(p)) free(p);
    return 0;
}

static int __cdecl qt_PtrToHand(const void *src, void **dst, int size)
{
    if (dst) *dst = NULL;                         /* callers store this unconditionally */
    g_qtMemErr = memFullErr;
    return memFullErr;
}

/* ---- GWorlds --------------------------------------------------------- */
static void qt_fill_pixmap(QPixMap *pm, BYTE *bits, int stride, int w, int h, int depth, QRect *bounds)
{
    memset(pm, 0, sizeof(*pm));
    pm->baseAddr = bits;
    pm->rowBytes = (short)(stride | 0x8000);
    if (bounds) pm->bounds = *bounds;
    else { pm->bounds.top = 0; pm->bounds.left = 0; pm->bounds.bottom = (short)h; pm->bounds.right = (short)w; }
    pm->hRes = pm->vRes = 72 << 16;
    pm->pixelType = 16;                           /* RGBDirect */
    pm->pixelSize = (short)depth;
    pm->cmpCount = 3;
    pm->cmpSize = depth == 32 ? 8 : 5;
    pm->pixelFormat = depth == 32 ? 0x42475241 /* 'BGRA' */ : 0x4C353535 /* 'L555' */;
}

static QGWorld *qt_gworld_new(void)
{
    QGWorld *g = (QGWorld *)calloc(1, sizeof(QGWorld));
    if (!g) return NULL;
    g->magic = QT_GWORLD_MAGIC;
    g->pmMaster = &g->pm;
    if (!qt_reg_add(g)) { free(g); return NULL; }
    return g;
}

/* NewGWorldFromHBITMAP(GWorldPtr*, CTabHandle, GDHandle, GWorldFlags, HBITMAP, HDC).
   The game's 16-bit top-down DIB section, already filled; the PixMap must
   alias it (decodes write into it, and the game copies it back afterwards). */
static int __cdecl qt_NewGWorldFromHBITMAP(void **out, void *ctab, void *gdev, int flags, HBITMAP bm, HDC dc)
{
    DIBSECTION ds;
    QGWorld *g;
    int h;
    if (out) *out = NULL;
    memset(&ds, 0, sizeof(ds));
    if (!out || !bm || GetObjectA(bm, sizeof(ds), &ds) != sizeof(ds) || !ds.dsBm.bmBits ||
        (ds.dsBm.bmBitsPixel != 16 && ds.dsBm.bmBitsPixel != 32)) {
        qt_note_first(0x170001, "NewGWorldFromHBITMAP", "unusable bitmap %p (bpp %d)", bm, ds.dsBm.bmBitsPixel);
        return paramErr;
    }
    /* The game's DIBs are top-down (biHeight = -h in sub_456980), so bmBits is
       the top row. GetObject reports dsBmih.biHeight as positive either way. */
    g = qt_gworld_new();
    if (!g) return memFullErr;
    h = ds.dsBm.bmHeight;
    qt_fill_pixmap(&g->pm, (BYTE *)ds.dsBm.bmBits, ds.dsBm.bmWidthBytes, ds.dsBm.bmWidth, h,
                   ds.dsBm.bmBitsPixel, NULL);
    g->dc = dc; g->bm = bm;
    *out = g;
    qt_note_first(0x170001, "NewGWorldFromHBITMAP", "%dx%d %dbpp stride %d -> %p",
                  ds.dsBm.bmWidth, h, ds.dsBm.bmBitsPixel, ds.dsBm.bmWidthBytes, g);
    return noErr;
}

static int qt_gworld_alloc(void **out, int depth, const QRect *r)
{
    QGWorld *g;
    int w, h, stride;
    BYTE *bits;
    if (out) *out = NULL;
    if (!out || !r) return paramErr;
    w = r->right - r->left; h = r->bottom - r->top;
    if (w <= 0 || h <= 0 || w > 8192 || h > 8192) return paramErr;
    if (depth != 32) depth = 16;
    stride = ((w * depth / 8) + 3) & ~3;          /* the engine's DWORD-aligned rows */
    bits = (BYTE *)calloc(1, (size_t)stride * h);
    g = bits ? qt_gworld_new() : NULL;
    if (!g) { free(bits); return memFullErr; }
    qt_fill_pixmap(&g->pm, bits, stride, w, h, depth, (QRect *)r);
    g->owned = bits;
    *out = g;
    return noErr;
}

/* NewGWorld(GWorldPtr*, short depth, const Rect*, CTabHandle, GDHandle, GWorldFlags) */
static int __cdecl qt_NewGWorld(void **out, int depth, const QRect *r)
{
    int e = qt_gworld_alloc(out, (short)depth == 32 ? 32 : 16, r);
    qt_note_first(0x1C0001, "NewGWorld", "depth %d -> %d", (short)depth, e);
    return e;
}

/* QTNewGWorld(GWorldPtr*, OSType pixelFormat, const Rect*, CTabHandle, GDHandle, GWorldFlags) */
static int __cdecl qt_QTNewGWorld(void **out, DWORD fmt, const QRect *r)
{
    int depth = (fmt == 0x42475241 || fmt == 0x20 || fmt == 0x41524742) ? 32 : 16;
    int e = qt_gworld_alloc(out, depth, r);
    qt_note_first(0x30080, "QTNewGWorld", "format %08lX -> %d", fmt, e);
    return e;
}

static int __cdecl qt_QTNewGWorldFromPtr(void **out)
{
    if (out) *out = NULL;
    return paramErr;
}

static int __cdecl qt_DisposeGWorld(void *gw)
{
    QGWorld *g = qt_gworld(gw);
    if (!g || !qt_reg_del(g)) return 0;
    /* The game never frees the memory DC and DIB it hands to
       NewGWorldFromHBITMAP, and nothing uses them after this call. */
    if (g->dc) DeleteDC(g->dc);
    if (g->bm) DeleteObject(g->bm);
    free(g->owned);
    g->magic = 0;
    free(g);
    return 0;
}

static int __cdecl qt_GetGWorldPixMap(void *gw)
{
    QGWorld *g = qt_gworld(gw);
    return g ? (int)&g->pmMaster : 0;
}

static int __cdecl qt_LockPixels(void *pmh)     { return qt_pixmap(pmh) ? 1 : 0; }
static int __cdecl qt_GetPixBaseAddr(void *pmh) { QPixMap *p = qt_pixmap(pmh); return p ? (int)p->baseAddr : 0; }
static int __cdecl qt_RowBytesOf(void *pmh)     { QPixMap *p = qt_pixmap(pmh); return p ? (p->rowBytes & 0x3FFF) : 0; }

static int __cdecl qt_GetGWorld(void **port, void **dev)
{
    if (port) *port = g_qtCurPort;                /* callers' variables are uninitialised */
    if (dev)  *dev  = g_qtCurDev;
    return 0;
}

static int __cdecl qt_SetGWorld(void *port, void *dev)
{
    g_qtCurPort = port; g_qtCurDev = dev;
    return 0;
}

static int __cdecl qt_GetPortHDC(void *port)
{
    QGWorld *g = qt_gworld(port);
    return g ? (int)g->dc : 0;
}

/* ---- QuickDraw / FixMath / matrices --------------------------------- */
static int __cdecl qt_SetRect(QRect *r, int l, int t, int rt, int b)
{
    if (r) { r->left = (short)l; r->top = (short)t; r->right = (short)rt; r->bottom = (short)b; }
    return 0;
}

static int __cdecl qt_OffsetRect(QRect *r, int dh, int dv)
{
    if (r) { r->left += (short)dh; r->right += (short)dh; r->top += (short)dv; r->bottom += (short)dv; }
    return 0;
}

static long qt_fixdiv(long a, long b)           /* rounded to nearest, like FixDiv */
{
    __int64 n = (__int64)a << 16;
    if (!b) return 0x7FFFFFFF;
    return (long)((n + ((n < 0) == (b < 0) ? b / 2 : -b / 2)) / b);
}
static int __cdecl qt_FixRatio(int n, int d)   { return (int)qt_fixdiv((short)n, (short)d); }
static int __cdecl qt_FixMul(int a, int b)     { return (int)(((__int64)a * b) >> 16); }
static int __cdecl qt_FixRound(int x)          { return (x + 0x8000) >> 16; }
static int __cdecl qt_Long2Fix(int x)          { return x << 16; }
static int __cdecl qt_TickCount(void)          { return (int)MulDiv(GetTickCount(), 60, 1000); }

static void qt_identity(QMatrix *m)
{
    memset(m, 0, sizeof(*m));
    m->m[0][0] = m->m[1][1] = 0x10000;
    m->m[2][2] = 0x40000000;
}

static int __cdecl qt_RectMatrix(QMatrix *m, const QRect *s, const QRect *d)
{
    long sx, sy;
    if (!m) return 0;
    qt_identity(m);
    if (!s || !d) return 0;
    sx = qt_fixdiv(d->right - d->left, s->right - s->left);
    sy = qt_fixdiv(d->bottom - d->top, s->bottom - s->top);
    m->m[0][0] = sx;
    m->m[1][1] = sy;
    m->m[2][0] = ((long)d->left << 16) - (long)(((__int64)s->left * sx));
    m->m[2][1] = ((long)d->top  << 16) - (long)(((__int64)s->top  * sy));
    return 0;
}

static int __cdecl qt_ScaleMatrix(QMatrix *m, int sx, int sy, int ax, int ay)
{
    int i;
    if (!m) return 0;
    for (i = 0; i < 3; i++) {                     /* M = M * S(about ax,ay) */
        m->m[i][0] = (long)(((__int64)(m->m[i][0] - (i == 2 ? ax : 0)) * sx) >> 16) + (i == 2 ? ax : 0);
        m->m[i][1] = (long)(((__int64)(m->m[i][1] - (i == 2 ? ay : 0)) * sy) >> 16) + (i == 2 ? ay : 0);
    }
    return 0;
}

static int __cdecl qt_TranslateMatrix(QMatrix *m, int dx, int dy)
{
    if (m) { m->m[2][0] += dx; m->m[2][1] += dy; }
    return 0;
}

static int __cdecl qt_EmptyRectOut1(void *a0, QRect *r)    { if (r) memset(r, 0, sizeof(*r)); return 0; }

/* FSMakeFSSpec(short vRefNum, long dirID, ConstStr255Param name, FSSpec *spec).
   On Windows an FSSpec is {short vRefNum; long parID; Str255 name = full path}. */
static int __cdecl qt_FSMakeFSSpec(int vref, int dir, const BYTE *pname, BYTE *spec)
{
    char path[260];
    BYTE name[256];
    int n;
    if (!spec || !pname) return paramErr;
    n = pname[0];
    memcpy(name, pname, n + 1);                   /* may live inside spec (the in-place Mac idiom) */
    memset(spec, 0, 262);
    memcpy(spec + 6, name, n + 1);
    memcpy(path, name + 1, n); path[n] = 0;
    return GetFileAttributesA(path) == INVALID_FILE_ATTRIBUTES ? fnfErr : noErr;
}

/* ---- JPEG through WIC ------------------------------------------------ */
static IWICImagingFactory *qt_wic(void)
{
    if (!g_wic) {
        HRESULT hr = CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);   /* S_FALSE / changed-mode are fine */
        hr = CoCreateInstance(&qt_CLSID_WICFactory, NULL, CLSCTX_INPROC_SERVER, &qt_IID_WICFactory, (void **)&g_wic);
        if (FAILED(hr)) notef("QT", 0, "WIC imaging factory -> %08lX", hr);
        else            tracef("QT", 1, "WIC imaging factory -> %08lX", hr);
    }
    return g_wic;
}

/* 16/32-bit PixMap rectangle -> 24-bit BGR rows */
static BYTE *qt_pixmap_to_bgr(QPixMap *pm, const QRect *src, int *pw, int *ph)
{
    int w = src->right - src->left, h = src->bottom - src->top, x, y, stride = pm->rowBytes & 0x3FFF;
    int ox = src->left - pm->bounds.left, oy = src->top - pm->bounds.top;
    BYTE *out, *o;
    if (w <= 0 || h <= 0 || ox < 0 || oy < 0 ||
        ox + w > pm->bounds.right - pm->bounds.left || oy + h > pm->bounds.bottom - pm->bounds.top) return NULL;
    out = (BYTE *)malloc((size_t)w * h * 3);
    if (!out) return NULL;
    o = out;
    for (y = 0; y < h; y++) {
        const BYTE *row = pm->baseAddr + (size_t)(oy + y) * stride;
        for (x = 0; x < w; x++) {
            if (pm->pixelSize == 32) {
                const BYTE *p = row + (ox + x) * 4;
                o[0] = p[0]; o[1] = p[1]; o[2] = p[2];
            } else {
                WORD v = ((const WORD *)row)[ox + x];
                int r = (v >> 10) & 31, g = (v >> 5) & 31, b = v & 31;
                o[0] = (BYTE)((b << 3) | (b >> 2));
                o[1] = (BYTE)((g << 3) | (g >> 2));
                o[2] = (BYTE)((r << 3) | (r >> 2));
            }
            o += 3;
        }
    }
    *pw = w; *ph = h;
    return out;
}

/* Encode BGR rows as a baseline JFIF stream (4:4:4). Returns malloc'd bytes. */
static BYTE *qt_jpeg_encode(const BYTE *bgr, int w, int h, float quality, DWORD *len)
{
    IWICImagingFactory *f = qt_wic();
    IStream *st = NULL;
    IWICBitmapEncoder *enc = NULL;
    IWICBitmapFrameEncode *fr = NULL;
    IPropertyBag2 *props = NULL;
    BYTE *res = NULL;
    HRESULT hr;
    *len = 0;
    if (!f) return NULL;
    hr = CreateStreamOnHGlobal(NULL, TRUE, &st);
    if (SUCCEEDED(hr)) hr = IWICImagingFactory_CreateEncoder(f, &qt_GUID_Jpeg, NULL, &enc);
    if (SUCCEEDED(hr)) hr = IWICBitmapEncoder_Initialize(enc, st, WICBitmapEncoderNoCache);
    if (SUCCEEDED(hr)) hr = IWICBitmapEncoder_CreateNewFrame(enc, &fr, &props);
    if (SUCCEEDED(hr) && props) {
        PROPBAG2 opt;
        VARIANT v;
        memset(&opt, 0, sizeof(opt));
        opt.pstrName = L"ImageQuality";
        VariantInit(&v); v.vt = VT_R4; v.fltVal = quality;
        IPropertyBag2_Write(props, 1, &opt, &v);
        opt.pstrName = L"JpegYCrCbSubsampling";
        VariantInit(&v); v.vt = VT_UI1; v.bVal = 3;   /* WICJpegYCrCbSubsampling444, as QuickTime's Photo-JPEG wrote */
        IPropertyBag2_Write(props, 1, &opt, &v);
    }
    if (SUCCEEDED(hr)) hr = IWICBitmapFrameEncode_Initialize(fr, props);
    if (SUCCEEDED(hr)) hr = IWICBitmapFrameEncode_SetSize(fr, w, h);
    if (SUCCEEDED(hr)) {
        WICPixelFormatGUID fmt = qt_GUID_24bppBGR;
        hr = IWICBitmapFrameEncode_SetPixelFormat(fr, &fmt);
        if (SUCCEEDED(hr) && memcmp(&fmt, &qt_GUID_24bppBGR, sizeof(fmt))) hr = E_FAIL;
    }
    if (SUCCEEDED(hr)) hr = IWICBitmapFrameEncode_WritePixels(fr, h, w * 3, w * 3 * h, (BYTE *)bgr);
    if (SUCCEEDED(hr)) hr = IWICBitmapFrameEncode_Commit(fr);
    if (SUCCEEDED(hr)) hr = IWICBitmapEncoder_Commit(enc);
    if (SUCCEEDED(hr)) {
        HGLOBAL hg = NULL;
        STATSTG ss;
        memset(&ss, 0, sizeof(ss));
        if (SUCCEEDED(GetHGlobalFromStream(st, &hg)) && SUCCEEDED(IStream_Stat(st, &ss, STATFLAG_NONAME))) {
            void *p = GlobalLock(hg);
            DWORD n = ss.cbSize.LowPart;
            if (p && n && (res = (BYTE *)malloc(n)) != NULL) { memcpy(res, p, n); *len = n; }
            if (p) GlobalUnlock(hg);
        }
    }
    if (FAILED(hr)) notef("QT", 0, "JPEG encode %dx%d failed: %08lX", w, h, hr);
    if (props) IPropertyBag2_Release(props);
    if (fr) IWICBitmapFrameEncode_Release(fr);
    if (enc) IWICBitmapEncoder_Release(enc);
    if (st) IStream_Release(st);
    return res;
}

/* Decode a JPEG to 24-bit BGR rows. */
static BYTE *qt_jpeg_decode(const BYTE *data, DWORD len, int *pw, int *ph)
{
    IWICImagingFactory *f = qt_wic();
    IWICStream *st = NULL;
    IWICBitmapDecoder *dec = NULL;
    IWICBitmapFrameDecode *fr = NULL;
    IWICFormatConverter *cv = NULL;
    UINT w = 0, h = 0;
    BYTE *out = NULL;
    HRESULT hr;
    if (!f || !data || !len) return NULL;
    hr = IWICImagingFactory_CreateStream(f, &st);
    if (SUCCEEDED(hr)) hr = IWICStream_InitializeFromMemory(st, (BYTE *)data, len);
    if (SUCCEEDED(hr)) hr = IWICImagingFactory_CreateDecoderFromStream(f, (IStream *)st, NULL,
                                                                      WICDecodeMetadataCacheOnDemand, &dec);
    if (SUCCEEDED(hr)) hr = IWICBitmapDecoder_GetFrame(dec, 0, &fr);
    if (SUCCEEDED(hr)) hr = IWICBitmapFrameDecode_GetSize(fr, &w, &h);
    if (SUCCEEDED(hr) && (!w || !h || w > 8192 || h > 8192)) hr = E_FAIL;
    if (SUCCEEDED(hr)) hr = IWICImagingFactory_CreateFormatConverter(f, &cv);
    if (SUCCEEDED(hr)) hr = IWICFormatConverter_Initialize(cv, (IWICBitmapSource *)fr, &qt_GUID_24bppBGR,
                                                           WICBitmapDitherTypeNone, NULL, 0.0, WICBitmapPaletteTypeCustom);
    if (SUCCEEDED(hr) && (out = (BYTE *)malloc((size_t)w * h * 3)) == NULL) hr = E_OUTOFMEMORY;
    if (SUCCEEDED(hr)) hr = IWICFormatConverter_CopyPixels(cv, NULL, w * 3, w * 3 * h, out);
    if (FAILED(hr)) { notef("QT", 0, "JPEG decode (%lu bytes) failed: %08lX", len, hr); free(out); out = NULL; }
    else { *pw = (int)w; *ph = (int)h; }
    if (cv) IWICFormatConverter_Release(cv);
    if (fr) IWICBitmapFrameDecode_Release(fr);
    if (dec) IWICBitmapDecoder_Release(dec);
    if (st) IWICStream_Release(st);
    return out;
}

/* GetMaxCompressionSize(PixMapHandle, const Rect*, short depth, CodecQ, CodecType, CompressorComponent, long *size)
   The game compresses into a buffer of exactly this size, with no length
   argument, so this bounds the worst case of the encoder. */
static int __cdecl qt_GetMaxCompressionSize(void *pmh, const QRect *r, int depth, int q, DWORD type, int codec, long *size)
{
    int w = r ? r->right - r->left : 0, h = r ? r->bottom - r->top : 0;
    if (size) *size = 0;
    if (!size || w <= 0 || h <= 0) return paramErr;
    *size = w * h * 4 + 65536;
    qt_note_first(0x30004, "GetMaxCompressionSize", "%dx%d '%c%c%c%c' q=%d -> %ld", w, h,
                  (char)(type >> 24), (char)(type >> 16), (char)(type >> 8), (char)type, q, *size);
    return noErr;
}

/* FCompressImage(src, srcRect, depth, quality, cType, codec, ctable, flags, bufferSize,
                  flushProc, progressProc, ImageDescriptionHandle desc, Ptr data) */
static int __cdecl qt_FCompressImage(void *pmh, const QRect *r, int depth, int q, DWORD type, int codec,
                                     void *ctab, int flags, int bufsize, void *flush, void *prog,
                                     void **desc, BYTE *data)
{
    QPixMap *pm = qt_pixmap(pmh);
    QHandleBlock *db = qt_handle_block(desc);
    BYTE *bgr, *jpg = NULL;
    int w = 0, h = 0;
    DWORD len = 0, limit = (DWORD)-1;
    float quality = (q & 0xFFFF) >= 0x400 ? 1.0f : (q & 0xFFFF) >= 0x300 ? 0.95f : (q & 0xFFFF) >= 0x200 ? 0.85f : 0.70f;
    if (!pm || !r || !data || type != 0x6A706567 /* 'jpeg' */) {
        qt_note_first(0x30007, "FCompressImage", "unsupported (pm %p, type %08lX)", pm, type);
        return paramErr;
    }
    if (r) limit = (DWORD)((r->right - r->left) * (r->bottom - r->top) * 4 + 65536);
    bgr = qt_pixmap_to_bgr(pm, r, &w, &h);
    if (!bgr) return paramErr;
    for (;;) {                                    /* the buffer has no length; never overflow it */
        jpg = qt_jpeg_encode(bgr, w, h, quality, &len);
        if (!jpg || len <= limit || quality < 0.3f) break;
        free(jpg); jpg = NULL; quality -= 0.1f;
    }
    free(bgr);
    if (!jpg || len > limit) { free(jpg); return paramErr; }
    memcpy(data, jpg, len);
    free(jpg);
    if (db && db->size >= sizeof(QImageDesc)) {
        QImageDesc *d = (QImageDesc *)db->master;
        memset(d, 0, sizeof(*d));
        d->idSize = sizeof(QImageDesc);
        d->cType = 0x6A706567;
        d->version = 1; d->revisionLevel = 1;
        d->vendor = 0x6170706C;                   /* 'appl' */
        d->spatialQuality = q & 0xFFFF;
        d->width = (short)w; d->height = (short)h;
        d->hRes = d->vRes = 72 << 16;
        d->dataSize = (long)len;
        d->frameCount = 1;
        memcpy(d->name, "\x0CPhoto - JPEG", 13);
        d->depth = 24; d->clutID = -1;
    }
    qt_note_first(0x30007, "FCompressImage", "%dx%d %dbpp -> JPEG %lu bytes (quality %.2f)", w, h, pm->pixelSize, len, quality);
    return noErr;
}

static DWORD qt_jpeg_length(const BYTE *p, long declared)
{
    DWORD i;
    if (declared > 4 && declared < (64 << 20)) return (DWORD)declared;
    __try {                                       /* no usable size: scan to EOI */
        for (i = 2; i < (16u << 20); i++)
            if (p[i] == 0xFF && p[i + 1] == 0xD9) return i + 2;
    } __except (EXCEPTION_EXECUTE_HANDLER) { }
    return 0;
}

/* FDecompressImage(data, desc, dst, srcRect, matrix, mode, mask, matte, matteRect,
                    accuracy, codec, bufferSize, dataProc, progressProc) */
static int __cdecl qt_FDecompressImage(BYTE *data, void **desc, void *dsth, const QRect *srcRect, const QMatrix *mx)
{
    QPixMap *pm = qt_pixmap(dsth);
    QHandleBlock *db = qt_handle_block(desc);
    QMatrix m;
    BYTE *bgr;
    int iw = 0, ih = 0, sx0 = 0, sy0 = 0, sw, sh, x, y, x0, x1, y0, y1, stride;
    long declared = (db && db->size >= sizeof(QImageDesc)) ? ((QImageDesc *)db->master)->dataSize : 0;
    __int64 tx, ty;
    long fx, fy;
    if (!pm || !data || data[0] != 0xFF || data[1] != 0xD8) {
        qt_note_first(0x30009, "FDecompressImage", "unsupported (pm %p, data %p)", pm, data);
        return paramErr;
    }
    bgr = qt_jpeg_decode(data, qt_jpeg_length(data, declared), &iw, &ih);
    if (!bgr) return paramErr;
    if (mx) m = *mx; else qt_identity(&m);
    sw = iw; sh = ih;
    if (srcRect) {                                /* decode only this part of the image */
        sx0 = max(0, srcRect->left); sy0 = max(0, srcRect->top);
        sw = min(iw, srcRect->right) - sx0; sh = min(ih, srcRect->bottom) - sy0;
    }
    fx = m.m[0][0];
    fy = m.m[1][1];
    tx = m.m[2][0]; ty = m.m[2][1];               /* Fixed */
    if (fx <= 0 || fy <= 0 || sw <= 0 || sh <= 0) { free(bgr); return noErr; }   /* empty destination */
    /* destination rectangle in port coordinates, rounded (a scale like 110/600
       is not exact in 16.16, and flooring would drop the last column/row),
       then clipped to the PixMap */
    x0 = (int)((tx + 0x8000) >> 16);       x1 = (int)((tx + (__int64)sw * fx + 0x8000) >> 16);
    y0 = (int)((ty + 0x8000) >> 16);       y1 = (int)((ty + (__int64)sh * fy + 0x8000) >> 16);
    x0 = max(x0, pm->bounds.left);  x1 = min(x1, pm->bounds.right);
    y0 = max(y0, pm->bounds.top);   y1 = min(y1, pm->bounds.bottom);
    stride = pm->rowBytes & 0x3FFF;
    for (y = y0; y < y1; y++) {
        int iy = sy0 + (int)((((__int64)y << 16) - ty) / fy);
        BYTE *row = pm->baseAddr + (size_t)(y - pm->bounds.top) * stride;
        if (iy < sy0) iy = sy0; else if (iy >= sy0 + sh) iy = sy0 + sh - 1;
        for (x = x0; x < x1; x++) {
            int ix = sx0 + (int)((((__int64)x << 16) - tx) / fx);
            const BYTE *s;
            if (ix < sx0) ix = sx0; else if (ix >= sx0 + sw) ix = sx0 + sw - 1;
            s = bgr + ((size_t)iy * iw + ix) * 3;
            if (pm->pixelSize == 32) {
                BYTE *d = row + (x - pm->bounds.left) * 4;
                d[0] = s[0]; d[1] = s[1]; d[2] = s[2]; d[3] = 0xFF;
            } else {
                ((WORD *)row)[x - pm->bounds.left] = (WORD)(((s[2] >> 3) << 10) | ((s[1] >> 3) << 5) | (s[0] >> 3));
            }
        }
    }
    free(bgr);
    qt_note_first(0x30009, "FDecompressImage", "JPEG %dx%d -> %d,%d..%d,%d of a %dbpp PixMap", iw, ih, x0, y0, x1, y1, pm->pixelSize);
    return noErr;
}

/* ---- "not available" answers ------------------------------------------ */
static int __cdecl qt_ErrParam(void)             { return paramErr; }
static int __cdecl qt_ErrNoQT(void)              { return qtNotAvailable; }
static int __cdecl qt_Ret1(void)                 { return 1; }
static int __cdecl qt_Ret600(void)               { return 600; }
static int __cdecl qt_RetBadCI(void)             { return badComponentInstance; }
static int __cdecl qt_RetGwFlagErr(void)         { return gwFlagErr; }
static int __cdecl qt_ClearOut0Err(void **a0)    { if (a0) *a0 = NULL; return paramErr; }
static int __cdecl qt_ClearOut1Err(int a0, void **a1)                 { if (a1) *a1 = NULL; return paramErr; }
static int __cdecl qt_ClearOut2Err(int a0, int a1, void **a2)         { if (a2) *a2 = NULL; return qtNotAvailable; }
static int __cdecl qt_ClearShort1Err(int a0, short *a1)               { if (a1) *a1 = 0; return paramErr; }
static int __cdecl qt_ClearLong1Err(int a0, long *a1)                 { if (a1) *a1 = 0; return paramErr; }
static int __cdecl qt_ClearShort2Err(int a0, int a1, short *a2)       { if (a2) *a2 = 0; return paramErr; }
static int __cdecl qt_GetGraphicsImporterForFile(void *spec, void **gi) { if (gi) *gi = NULL; return noCodecErr; }
static int __cdecl qt_NewMovieFromFile(void **m)  { if (m) *m = NULL; return -2048; /* noMovieFound */ }
static int __cdecl qt_CreateMovieFile(void *spec, int creator, int script, int flags, short *ref, void **m)
{
    if (ref) *ref = 0;
    if (m) *m = NULL;
    return paramErr;
}
static int __cdecl qt_GetMovieTime(void *m, void *tr)          { if (tr) memset(tr, 0, 16); return 0; }
static int __cdecl qt_GetTrackMatrix(void *t, QMatrix *mx)     { if (mx) qt_identity(mx); return 0; }
static int __cdecl qt_GetTrackDimensions(void *t, long *w, long *h) { if (w) *w = 0; if (h) *h = 0; return 0; }
static int __cdecl qt_GetMediaHandlerDescription(void *md, DWORD *type, BYTE *name, DWORD *mfr)
{
    if (type) *type = 0;
    if (name) name[0] = 0;
    if (mfr) *mfr = 0;
    return 0;
}
static int __cdecl qt_GetMovieNextInterestingTime(void *m, int f, int n, void *types, int t, int rate, long *it, long *dur)
{
    if (it) *it = -1;
    if (dur) *dur = 0;
    return 0;
}
static int __cdecl qt_NewTimeBase(void)          { g_qtMemErr = memFullErr; return 0; }
static int __cdecl qt_GetMoviesError(void)       { return paramErr; }

/* ---- selector table --------------------------------------------------- */
typedef struct { DWORD sel; void *fn; const char *name; } QtEntry;
static const QtEntry g_qtTable[] = {
    /* Component Manager */
    { 0x010000, qt_RetBadCI,          "CallComponent" },
    { 0x010008, qt_Nop,               "CloseComponent" },
    { 0x01002E, qt_ClearOut2Err,      "OpenADefaultComponent" },
    /* Movie Toolbox: everything fails, nothing is ever a movie */
    { 0x020001, qt_Nop,               "EnterMovies" },
    { 0x020003, qt_GetMoviesError,    "GetMoviesError" },
    { 0x020005, qt_Nop, "MoviesTask" }, { 0x020009, qt_Nop, "SetMovieActive" },
    { 0x02000B, qt_Nop, "StartMovie" }, { 0x02000C, qt_Nop, "StopMovie" },
    { 0x020016, qt_Nop, "SetMovieGWorld" }, { 0x020023, qt_Nop, "DisposeMovie" },
    { 0x020029, qt_Ret600, "GetMovieTimeScale" }, { 0x02002A, qt_Nop, "SetMovieTimeScale" },
    { 0x02002B, qt_Nop, "GetMovieDuration" }, { 0x020036, qt_Nop, "SetMoviePosterTime" },
    { 0x020038, qt_Nop, "SetMovieSelection" }, { 0x020039, qt_GetMovieTime, "GetMovieTime" },
    { 0x02003D, qt_Nop, "SetMovieTimeValue" }, { 0x02003F, qt_Nop, "GetMovieTrackCount" },
    { 0x020040, qt_Nop, "GetMovieTrack" }, { 0x020048, qt_Nop, "SetTrackUsage" },
    { 0x02004B, qt_Nop, "GetTrackDuration" }, { 0x020050, qt_Nop, "GetTrackLayer" },
    { 0x020051, qt_Nop, "SetTrackLayer" }, { 0x020054, qt_Nop, "GetTrackVolume" },
    { 0x020056, qt_GetTrackMatrix, "GetTrackMatrix" }, { 0x020057, qt_Nop, "SetTrackMatrix" },
    { 0x02005D, qt_GetTrackDimensions, "GetTrackDimensions" }, { 0x02005E, qt_Nop, "SetTrackDimensions" },
    { 0x020062, qt_Nop, "GetTrackMedia" }, { 0x020068, qt_Ret600, "GetMediaTimeScale" },
    { 0x02006A, qt_Nop, "GetMediaDuration" }, { 0x02006F, qt_GetMediaHandlerDescription, "GetMediaHandlerDescription" },
    { 0x020071, qt_Nop, "GetMediaHandler" }, { 0x020072, qt_ErrParam, "BeginMediaEdits" },
    { 0x020073, qt_ErrParam, "EndMediaEdits" }, { 0x02007C, qt_ErrParam, "AddMediaSample" },
    { 0x020085, qt_ErrParam, "InsertTrackSegment" }, { 0x020089, qt_ErrParam, "DeleteTrackSegment" },
    { 0x02008B, qt_ErrParam, "ScaleTrackSegment" }, { 0x0200A5, qt_NewTimeBase, "NewTimeBase" },
    { 0x0200A8, qt_Nop, "SetTimeBaseValue" }, { 0x0200AA, qt_Nop, "SetTimeBaseRate" },
    { 0x0200B6, qt_Nop, "DisposeTimeBase" }, { 0x0200D5, qt_Nop, "CloseMovieFile" },
    { 0x0200DD, qt_Ret1, "IsMovieDone" }, { 0x0200F0, qt_NewMovieFromFile, "NewMovieFromFile" },
    { 0x0200F7, qt_Nop, "GetMoviePosterPict" }, { 0x0200F9, qt_EmptyRectOut1, "GetMovieBox" },
    { 0x0200FA, qt_Nop, "SetMovieBox" }, { 0x020101, qt_Nop, "SetTrackClipRgn" },
    { 0x02010E, qt_GetMovieNextInterestingTime, "GetMovieNextInterestingTime" },
    { 0x020117, qt_Nop, "GetMovieIndTrack" }, { 0x020127, qt_Nop, "GetTrackID" },
    { 0x020152, qt_Nop, "AddMovieSelection" }, { 0x020153, qt_ErrParam, "CopyTrackSettings" },
    { 0x020179, qt_Nop, "SetMovieCoverProcs" }, { 0x020183, qt_ErrParam, "InsertMediaIntoTrack" },
    { 0x020187, qt_Nop, "NewMovie" }, { 0x020188, qt_Nop, "NewMovieTrack" },
    { 0x02018E, qt_Nop, "NewTrackMedia" }, { 0x020191, qt_CreateMovieFile, "CreateMovieFile" },
    { 0x020192, qt_ClearShort1Err, "OpenMovieFile" }, { 0x02019B, qt_Nop, "FlattenMovie" },
    { 0x02019C, qt_Nop, "FlattenMovieData" }, { 0x0201B4, qt_ErrParam, "PutMovieIntoDataFork" },
    { 0x0201C1, qt_ErrParam, "SetMovieDefaultDataRef" }, { 0x0201E3, qt_Nop, "SetTrackLoadSettings" },
    { 0x0201EC, qt_ClearOut0Err, "NewMovieFromUserProc" }, { 0x02020C, qt_ClearOut0Err, "QTNewAtomContainer" },
    { 0x02020D, qt_Nop, "QTDisposeAtomContainer" }, { 0x020216, qt_ErrParam, "QTGetAtomDataPtr" },
    { 0x020218, qt_ErrParam, "QTInsertChild" }, { 0x02021D, qt_Nop, "QTFindChildByIndex" },
    { 0x020220, qt_ClearOut0Err, "NewMovieFromDataRef" }, { 0x02022C, qt_EmptyRectOut1, "GetMovieNaturalBoundsRect" },
    /* Image Compression Manager */
    { 0x030004, qt_GetMaxCompressionSize, "GetMaxCompressionSize" },
    { 0x030007, qt_FCompressImage,        "FCompressImage" },
    { 0x030009, qt_FDecompressImage,      "FDecompressImage" },
    { 0x03000A, qt_ClearOut0Err, "CompressSequenceBegin" }, { 0x03000B, qt_ErrParam, "CompressSequenceFrame" },
    { 0x03001B, qt_Nop, "CDSequenceEnd" }, { 0x03005D, qt_ClearOut0Err, "DecompressSequenceBeginS" },
    { 0x03005E, qt_ErrParam, "DecompressSequenceFrameWhen" }, { 0x030068, qt_ErrParam, "CDSequenceNewDataSource" },
    { 0x03006A, qt_ErrParam, "CDSequenceSetSourceData" },
    { 0x03006E, qt_GetGraphicsImporterForFile, "GetGraphicsImporterForFile" },
    { 0x030079, qt_ErrParam, "CDSequenceSetTimeBase" }, { 0x03007A, qt_ErrParam, "MakeImageDescriptionForPixMap" },
    { 0x030080, qt_QTNewGWorld, "QTNewGWorld" }, { 0x03008B, qt_QTNewGWorldFromPtr, "QTNewGWorldFromPtr" },
    { 0x03008E, qt_RowBytesOf, "QTGetPixMapHandleRowBytes" },
    /* matrices */
    { 0x040017, qt_ScaleMatrix, "ScaleMatrix" }, { 0x040019, qt_TranslateMatrix, "TranslateMatrix" },
    { 0x04001E, qt_RectMatrix, "RectMatrix" },
    /* Alias Manager */
    { 0x080002, qt_ClearOut1Err, "NewAliasMinimal" },
    /* OS utilities */
    { 0x0E0005, qt_TickCount, "TickCount" },
    /* File Manager */
    { 0x0F0046, qt_Nop, "FSClose" }, { 0x0F0047, qt_ClearLong1Err, "FSRead" },
    { 0x0F0048, qt_ErrParam, "FSWrite" }, { 0x0F0054, qt_ClearLong1Err, "GetEOF" },
    { 0x0F0055, qt_ErrParam, "SetEOF" }, { 0x0F0056, qt_ClearLong1Err, "GetFPos" },
    { 0x0F0057, qt_ErrParam, "SetFPos" }, { 0x0F00E2, qt_FSMakeFSSpec, "FSMakeFSSpec" },
    { 0x0F00E3, qt_ClearShort2Err, "FSpOpenDF" }, { 0x0F00E5, qt_ErrParam, "FSpCreate" },
    { 0x0F00E7, qt_Nop, "FSpDelete" },
    /* FixMath */
    { 0x100001, qt_FixRatio, "FixRatio" }, { 0x100002, qt_FixMul, "FixMul" },
    { 0x100005, qt_FixRound, "FixRound" }, { 0x100006, qt_Long2Fix, "Long2Fix" },
    /* Memory Manager */
    { 0x150006, qt_MemError, "MemError" }, { 0x150008, qt_NewHandle, "NewHandle" },
    { 0x15000A, qt_NewHandleClear, "NewHandleClear" }, { 0x150011, qt_NewPtr, "NewPtr" },
    { 0x150019, qt_Nop, "HLock" }, { 0x15001A, qt_Nop, "HUnlock" }, { 0x15001D, qt_Nop, "HLockHi" },
    { 0x15002F, qt_DisposePtr, "DisposePtr" }, { 0x150032, qt_DisposeHandle, "DisposeHandle" },
    { 0x150034, qt_GetHandleSize, "GetHandleSize" }, { 0x15003E, qt_BlockMove, "BlockMove" },
    { 0x15003F, qt_BlockMove, "BlockMoveData" }, { 0x15005D, qt_PtrToHand, "PtrToHand" },
    /* QTML (Windows) */
    { 0x170001, qt_NewGWorldFromHBITMAP, "NewGWorldFromHBITMAP" }, { 0x170007, qt_GetPortHDC, "GetPortHDC" },
    /* QDOffscreen */
    { 0x1C0001, qt_NewGWorld, "NewGWorld" }, { 0x1C0002, qt_LockPixels, "LockPixels" },
    { 0x1C0003, qt_Nop, "UnlockPixels" }, { 0x1C0004, qt_RetGwFlagErr, "UpdateGWorld" },
    { 0x1C0005, qt_DisposeGWorld, "DisposeGWorld" }, { 0x1C0006, qt_GetGWorld, "GetGWorld" },
    { 0x1C0007, qt_SetGWorld, "SetGWorld" }, { 0x1C000E, qt_Nop, "GetPixelsState" },
    { 0x1C000F, qt_Nop, "SetPixelsState" }, { 0x1C0010, qt_GetPixBaseAddr, "GetPixBaseAddr" },
    { 0x1C0013, qt_Nop, "GetGWorldDevice" }, { 0x1C0018, qt_GetGWorldPixMap, "GetGWorldPixMap" },
    /* QTML ports */
    { 0x1D0001, qt_Nop, "CreatePortAssociation" }, { 0x1D0002, qt_Nop, "DestroyPortAssociation" },
    { 0x1D0008, qt_Nop, "InitializeQTML" },
    /* QuickDraw */
    { 0x1E0025, qt_SetRect, "SetRect" }, { 0x1E0026, qt_OffsetRect, "OffsetRect" },
    { 0x1E0040, qt_Nop, "NewRgn" }, { 0x1E0044, qt_Nop, "DisposeRgn" }, { 0x1E0048, qt_Nop, "RectRgn" },
    { 0x1E0058, qt_Nop, "CopyBits" }, { 0x1E005F, qt_Nop, "DrawPicture" }, { 0x1E0060, qt_Nop, "KillPicture" },
    { 0x1E009B, qt_Nop, "RGBForeColor" }, { 0x1E009C, qt_Nop, "RGBBackColor" },
    /* AIFF and Sound Manager */
    { 0x24000E, qt_ErrParam, "SetupAIFFHeader" }, { 0x24000F, qt_ErrParam, "ParseAIFFHeader" },
    { 0x250003, qt_Nop, "SysBeep" }, { 0x250027, qt_ClearOut2Err, "SoundConverterOpen" },
    { 0x250028, qt_Nop, "SoundConverterClose" }, { 0x250029, qt_ErrParam, "SoundConverterGetBufferSizes" },
    { 0x25002A, qt_ErrParam, "SoundConverterBeginConversion" }, { 0x25002B, qt_ErrParam, "SoundConverterConvertBuffer" },
    { 0x25002C, qt_ErrParam, "SoundConverterEndConversion" },
};

static void *__cdecl qt_lookup(DWORD sel)
{
    int i;
    for (i = 0; i < (int)(sizeof(g_qtTable) / sizeof(g_qtTable[0])); i++)
        if (g_qtTable[i].sel == sel) {
            if (g_qtTable[i].fn != (void *)qt_FCompressImage && g_qtTable[i].fn != (void *)qt_FDecompressImage &&
                g_qtTable[i].fn != (void *)qt_NewGWorldFromHBITMAP && g_qtTable[i].fn != (void *)qt_GetMaxCompressionSize &&
                g_qtTable[i].fn != (void *)qt_NewGWorld && g_qtTable[i].fn != (void *)qt_QTNewGWorld)
                qt_note_first(sel, g_qtTable[i].name, "");
            return g_qtTable[i].fn;
        }
    qt_note_first(sel, "?", "unknown selector -> paramErr");
    return (void *)qt_ErrParam;
}

/* Entered from a game thunk: EAX = selector, [esp] = the game's return
   address, arguments from [esp+4] - exactly a cdecl call to the handler. */
__declspec(naked) static void qt_dispatch(void)
{
    __asm {
        push eax
        call qt_lookup
        add  esp, 4
        jmp  eax
    }
}

static void patch_bytes(DWORD at, const BYTE *expect, int ne, const BYTE *with, int nw, const char *what)
{
    BYTE *p = (BYTE *)at;
    DWORD old;
    if (memcmp(p, expect, ne)) { notef("proxy", 0, "%s: bytes at %08lX not as expected - not patched", what, at); return; }
    VirtualProtect(p, nw, PAGE_EXECUTE_READWRITE, &old);
    memcpy(p, with, nw);
    VirtualProtect(p, nw, old, &old);
    FlushInstructionCache(GetCurrentProcess(), p, nw);
}

static void qt_install(void)                    /* DllMain, before any game code */
{
    static const BYTE loadOrig[] = { 0x56, 0x68 };                    /* push esi; push "quicktime.qts" */
    static const BYTE loadNew[]  = { 0x33, 0xC0, 0xC3 };              /* xor eax,eax; ret */
    static const BYTE qfxOrig[]  = { 0x6A, 0xFF, 0x68 };              /* push -1; push SEH_4AD620 */
    static const BYTE qfxNew[]   = { 0xC3 };                          /* ret (thiscall, no stack args) */
    DWORD old;
    InitializeCriticalSection(&g_qtLock);
    if (!g_b) return;
    /* Never load QuickTime.qts from the search path (DLL planting), and keep
       the InitializeQTML glue from putting the "unavailable" stub back. */
    patch_bytes(QT_LOAD_FN, loadOrig, sizeof(loadOrig), loadNew, sizeof(loadNew), "QuickTime loader");
    QT_INIT_COUNT = 1;
    /* The QuickTime-effect eraser drives a QuickTime transition with no error
       checks; without QuickTime the eraser just erases. */
    patch_bytes(QT_QFX_ERASER_FN, qfxOrig, sizeof(qfxOrig), qfxNew, sizeof(qfxNew), "QuickTime eraser effect");
    VirtualProtect(QT_THUNK_LOADER, 16, PAGE_READWRITE, &old);
    *QT_THUNK_LOADER   = NULL;
    *QT_THUNK_DISPATCH = (void *)qt_dispatch;
    *QT_THUNK_CALLCOMP = (void *)qt_dispatch;
    *QT_THUNK_CCFWS    = (void *)qt_dispatch;
    VirtualProtect(QT_THUNK_LOADER, 16, old, &old);
    notef("proxy", 1, "QuickTime stand-in installed (%d selectors; JPEG via WIC)",
          (int)(sizeof(g_qtTable) / sizeof(g_qtTable[0])));
}
