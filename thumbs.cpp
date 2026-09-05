// Thumbnails for the Gallery: a small JPEG per recording or saved still, generated once and
// cached on the card beside its file as <basename>.thm
//
// Three things shape this file:
//
// - MEMORY. A QSXGA frame is up to ~1MB and the measured PSRAM low-water mark on these boards is
//   ~984KB, so the source frame cannot be held in RAM. esp_jpg_decode() pulls its input through a
//   reader callback, so the JPEG streams off the card and the only buffer is the DECODED output at
//   1/8 in each axis: 160x90x3 at HD, 320x240x3 at QSXGA.
//
// - THE MIDDLE FRAME, not the first. The first frame of a motion clip is usually the empty scene
//   that triggered it. The middle is found with one seek to the byte midpoint and a forward scan
//   for the next valid chunk, not by walking every chunk header.
//
// - CONTENTION. Generating one competes with the recorder for the same card and the same CPU, so
//   it is refused outright while capturing or streaming rather than queued.
//
// s60sc fork, 5 Sep 2026

#include "appGlobals.h"
#include "esp_jpg_decode.h"
#include "img_converters.h"

#define THM_SCAN_WINDOW 8192   // bytes read at a time when hunting the middle frame
#define THM_SCAN_TRIES 8       // windows to try before falling back to the first frame

// the '00dc' chunk marker, as bytes rather than the packed uint32 the playback task uses
static const uint8_t dcMarker[4] = {0x30, 0x30, 0x64, 0x63};

struct thumbCtx {
  File* src;        // open source file
  size_t jpegStart; // offset of the JPEG within it
  size_t jpegLen;
  uint8_t* out;     // decoded RGB888, allocated on the decoder's start callback
  uint16_t outW;
  uint16_t outH;
};

static size_t thumbReader(void* arg, size_t index, uint8_t* buf, size_t len) {
  // index is relative to the start of the JPEG. A NULL buf means the decoder wants to skip
  // forward, which is why this seeks on every call rather than relying on the file position
  thumbCtx* ctx = (thumbCtx*)arg;
  if (index + len > ctx->jpegLen) len = ctx->jpegLen > index ? ctx->jpegLen - index : 0;
  if (!len) return 0;
  if (buf == NULL) return len; // skip
  if (!ctx->src->seek(ctx->jpegStart + index, SeekSet)) return 0;
  return ctx->src->read(buf, len);
}

static bool thumbWriter(void* arg, uint16_t x, uint16_t y, uint16_t w, uint16_t h, uint8_t* data) {
  thumbCtx* ctx = (thumbCtx*)arg;
  if (data == NULL) {
    if (x == 0 && y == 0) {
      // start: w and h are the SCALED output size, which is the only place it is known
      ctx->outW = w;
      ctx->outH = h;
      size_t need = (size_t)w * h * 3;
      ctx->out = (uint8_t*)(psramFound() ? ps_malloc(need) : malloc(need));
      if (ctx->out == NULL) {
        LOG_WRN("No memory for a %ux%u thumbnail (%s)", w, h, fmtSize(need));
        return false;
      }
    }
    return true; // start with a buffer, or end
  }
  if (ctx->out == NULL) return false;
  // decoded blocks arrive as RGB888 rows; copy each into place. RGB888 rather than RGB565 keeps
  // this a memcpy instead of a per-pixel conversion, at 320x240 costing 77KB more
  size_t rowBytes = (size_t)w * 3;
  size_t outRow = (size_t)ctx->outW * 3;
  for (uint16_t i = 0; i < h; i++) {
    if ((size_t)(y + i) >= ctx->outH) break;
    memcpy(ctx->out + ((size_t)(y + i) * outRow) + ((size_t)x * 3), data + (i * rowBytes), rowBytes);
  }
  return true;
}

static bool findMiddleFrame(File& f, size_t fileSize, size_t* start, size_t* len) {
  // the AVI is a fixed 310 byte header then '00dc' + 4 byte length + JPEG, repeated, so a frame
  // can be reached by seeking rather than parsing RIFF. Validity is checked the way
  // recoverInterrupted() checks it: a plausible length is not enough, the payload must also
  // start with a JPEG SOI, or the tail of a buffer reads as a frame
  static uint8_t win[THM_SCAN_WINDOW];
  size_t from = AVI_HEADER_LEN + ((fileSize - AVI_HEADER_LEN) / 2);
  for (int tries = 0; tries < THM_SCAN_TRIES && from + CHUNK_HDR + 2 < fileSize; tries++) {
    if (!f.seek(from, SeekSet)) break;
    size_t got = f.read(win, THM_SCAN_WINDOW);
    if (got < CHUNK_HDR + 2) break;
    for (size_t i = 0; i + CHUNK_HDR + 2 <= got; i++) {
      if (memcmp(win + i, dcMarker, 4)) continue;
      uint32_t clen = 0;
      memcpy(&clen, win + i + 4, 4);
      size_t payload = from + i + CHUNK_HDR;
      if (!clen || clen > maxFrameBuffSize || payload + clen > fileSize) continue;
      if (win[i + CHUNK_HDR] != 0xFF || win[i + CHUNK_HDR + 1] != 0xD8) continue; // not a JPEG
      *start = payload;
      *len = clen;
      return true;
    }
    from += got - (CHUNK_HDR + 2); // overlap, in case a marker straddles the window edge
  }
  // fall back to the first frame, which is always at the fixed header length
  uint8_t hdr[CHUNK_HDR];
  if (f.seek(AVI_HEADER_LEN, SeekSet) && f.read(hdr, CHUNK_HDR) == CHUNK_HDR
      && !memcmp(hdr, dcMarker, 4)) {
    uint32_t clen = 0;
    memcpy(&clen, hdr + 4, 4);
    if (clen && clen <= maxFrameBuffSize && AVI_HEADER_LEN + CHUNK_HDR + clen <= fileSize) {
      *start = AVI_HEADER_LEN + CHUNK_HDR;
      *len = clen;
      return true;
    }
  }
  return false;
}

bool makeThumb(const char* srcPath, char* thmPath, size_t thmPathLen) {
  // generate <basename>.thm for srcPath. Returns true if the cache file now exists
  strncpy(thmPath, srcPath, thmPathLen - 1);
  thmPath[thmPathLen - 1] = 0;
  if (!changeExtension(thmPath, THM_EXT)) return false;
  if (STORAGE.exists(thmPath)) return true;

  // refused, not queued: this reads a whole frame off the card and decodes it, and the recorder
  // needs both. A tile without a thumbnail shows a placeholder, so browsing still works
  if (isCapturing || forceRecord) {
    LOG_VRB("Thumbnail for %s deferred - capture in progress", srcPath);
    return false;
  }

  File f = STORAGE.open(srcPath, FILE_READ);
  if (!f) {
    LOG_WRN("Thumbnail source %s could not be opened", srcPath);
    return false;
  }
  size_t fileSize = f.size();
  thumbCtx ctx = {&f, 0, fileSize, NULL, 0, 0};
  bool isAvi = nameHasExt(srcPath, AVI_EXT);
  if (isAvi) {
    if (fileSize <= AVI_HEADER_LEN + CHUNK_HDR || !findMiddleFrame(f, fileSize, &ctx.jpegStart, &ctx.jpegLen)) {
      LOG_WRN("No usable frame in %s for a thumbnail", srcPath);
      f.close();
      return false;
    }
  }

  uint32_t started = millis();
  esp_err_t res = esp_jpg_decode(ctx.jpegLen, JPG_SCALE_8X, thumbReader, thumbWriter, &ctx);
  f.close();
  if (res != ESP_OK || ctx.out == NULL || !ctx.outW || !ctx.outH) {
    if (ctx.out != NULL) free(ctx.out);
    LOG_WRN("Thumbnail decode failed for %s: %s", srcPath, espErrMsg(res));
    return false;
  }

  uint8_t* jpg = NULL;
  size_t jpgLen = 0;
  bool ok = fmt2jpg(ctx.out, (size_t)ctx.outW * ctx.outH * 3, ctx.outW, ctx.outH,
                    PIXFORMAT_RGB888, THM_QUALITY, &jpg, &jpgLen);
  free(ctx.out);
  if (!ok || jpg == NULL) {
    LOG_WRN("Thumbnail encode failed for %s", srcPath);
    return false;
  }
  File tf = STORAGE.open(thmPath, FILE_WRITE);
  size_t written = 0;
  if (tf) {
    written = tf.write(jpg, jpgLen);
    tf.close();
  }
  free(jpg);
  if (written != jpgLen) {
    LOG_WRN("Thumbnail %s short write: %u of %u", thmPath, written, jpgLen);
    STORAGE.remove(thmPath);
    return false;
  }
  LOG_INF("Thumbnail %s: %ux%u, %s, %lums", thmPath, ctx.outW, ctx.outH, fmtSize(jpgLen),
    (unsigned long)(millis() - started));
  return true;
}
