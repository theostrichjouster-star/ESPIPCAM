
/* 
Generate AVI format for recorded videos

s60sc 2020, 2022
*/

/* AVI file format:
Video and audio chunks are interleaved, one audio chunk following each video frame,
holding the audio captured while that frame was being written.
header:
 310 bytes
per jpeg:
 4 byte 00dc marker
 4 byte jpeg size
 jpeg frame content
 0-3 bytes filler to align on DWORD boundary
per PCM (audio captured since the previous frame)
 4 byte 01wb marker
 4 byte pcm size
 pcm content
 0-3 bytes filler to align on DWORD boundary
footer:
 4 byte idx1 marker
 4 byte index size
 per jpeg:
  4 byte 00dc marker
  4 byte 0000
  4 byte jpeg location
  4 byte jpeg size
 per pcm:
  4 byte 01wb marker
  4 byte 0000
  4 byte pcm location
  4 byte pcm size
*/

#include "appGlobals.h"

// avi header data
const uint8_t dcBuf[4] = {0x30, 0x30, 0x64, 0x63};   // 00dc
const uint8_t wbBuf[4] = {0x30, 0x31, 0x77, 0x62};   // 01wb
static const uint8_t idx1Buf[4] = {0x69, 0x64, 0x78, 0x31}; // idx1
static const uint8_t zeroBuf[4] = {0x00, 0x00, 0x00, 0x00}; // 0000
static uint8_t* idxBuf[2] = {NULL, NULL};

uint8_t aviHeader[AVI_HEADER_LEN] = { // AVI header template
  0x52, 0x49, 0x46, 0x46, 0x00, 0x00, 0x00, 0x00, 0x41, 0x56, 0x49, 0x20, 0x4C, 0x49, 0x53, 0x54,
  0x16, 0x01, 0x00, 0x00, 0x68, 0x64, 0x72, 0x6C, 0x61, 0x76, 0x69, 0x68, 0x38, 0x00, 0x00, 0x00,
  0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x10, 0x00, 0x00, 0x00,
  0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
  0x00, 0x00, 0x00, 0x00, 0xe0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
  0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x4C, 0x49, 0x53, 0x54, 0x6C, 0x00, 0x00, 0x00,
  0x73, 0x74, 0x72, 0x6C, 0x73, 0x74, 0x72, 0x68, 0x30, 0x00, 0x00, 0x00, 0x76, 0x69, 0x64, 0x73,
  0x4D, 0x4A, 0x50, 0x47, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
  0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x0A, 0x00, 0x00, 0x00,
  0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x73, 0x74, 0x72, 0x66,
  0x28, 0x00, 0x00, 0x00, 0x28, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
  0x01, 0x00, 0x18, 0x00, 0x4D, 0x4A, 0x50, 0x47, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
  0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 
  0x4C, 0x49, 0x53, 0x54, 0x56, 0x00, 0x00, 0x00, 
  0x73, 0x74, 0x72, 0x6C, 0x73, 0x74, 0x72, 0x68, 0x30, 0x00, 0x00, 0x00, 0x61, 0x75, 0x64, 0x73,
  0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
  0x01, 0x00, 0x00, 0x00, 0x11, 0x2B, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
  0x11, 0x2B, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x02, 0x00, 0x00, 0x00, 0x73, 0x74, 0x72, 0x66,
  0x12, 0x00, 0x00, 0x00, 0x01, 0x00, 0x01, 0x00, 0x11, 0x2B, 0x00, 0x00, 0x11, 0x2B, 0x00, 0x00,
  0x02, 0x00, 0x10, 0x00, 0x00, 0x00, 
  0x4C, 0x49, 0x53, 0x54, 0x00, 0x00, 0x00, 0x00, 0x6D, 0x6F, 0x76, 0x69,
};

struct frameSizeStruct {
  uint8_t frameWidth[2];
  uint8_t frameHeight[2];
};
// indexed by frame type - needs to be consistent with sensor.h framesize_t enum
static const frameSizeStruct frameSizeData[] = {
  {{0x60, 0x00}, {0x60, 0x00}}, // 96X96
  {{0xA0, 0x00}, {0x78, 0x00}}, // qqvga 
  {{0x80, 0x00}, {0x80, 0x00}}, // 128X128
  {{0xB0, 0x00}, {0x90, 0x00}}, // qcif 
  {{0xF0, 0x00}, {0xB0, 0x00}}, // hqvga 
  {{0xF0, 0x00}, {0xF0, 0x00}}, // 240X240
  {{0x40, 0x01}, {0xF0, 0x00}}, // qvga 
  {{0x40, 0x01}, {0x40, 0x01}}, // 320X320
  {{0x90, 0x01}, {0x28, 0x01}}, // cif 
  {{0xE0, 0x01}, {0x40, 0x01}}, // hvga 
  {{0x80, 0x02}, {0xE0, 0x01}}, // vga 
  {{0x20, 0x03}, {0x58, 0x02}}, // svga 
  {{0x00, 0x04}, {0x00, 0x03}}, // xga 
  {{0x00, 0x05}, {0xD0, 0x02}}, // hd
  {{0x00, 0x05}, {0x00, 0x04}}, // sxga
  {{0x40, 0x06}, {0xB0, 0x04}}, // uxga 
  {{0x98, 0x03}, {0x38, 0x04}}, // FHD
  {{0xD0, 0x02}, {0x00, 0x05}}, // P_HD
  {{0x60, 0x03}, {0x00, 0x06}}, // P_3MP
  {{0x00, 0x08}, {0x00, 0x06}}, // QXGA
  {{0x00, 0x0A}, {0xA0, 0x05}}, // QHD
  {{0x00, 0x0A}, {0x40, 0x06}}, // WQXGA
  {{0x38, 0x04}, {0x80, 0x07}}, // P_FHD
  {{0x00, 0x0A}, {0x80, 0x07}}, // QSXGA
  {{0x20, 0x0A}, {0x98, 0x07}}  // 5MP
};

#define IDX_ENTRY 16 // bytes per index entry

// separate index for motion capture and timelapse
static size_t idxPtr[2];
static size_t idxOffset[2];
static size_t moviSize[2];
static size_t audSize;      // running total of interleaved audio bytes
static size_t audChunkCnt;  // number of 01wb chunks written
static size_t indexLen[2];

static size_t idxBufSize(bool isTL) {
  // motion capture interleaves one audio chunk per video frame, so it needs two index
  // entries per frame, plus one more for the chunk written when the recording closes.
  // Timelapse has no audio and needs one per frame. Both include space for the index
  // header. The spare entries matter - overflowing this reboots the device mid recording
  return (isTL ? maxFrames + 2 : (2 * maxFrames) + 8) * IDX_ENTRY;
}

void prepAviIndex(bool isTL) {
  // prep buffer to store index data, gets appended to end of file
  if (idxBuf[isTL] == NULL) idxBuf[isTL] = (uint8_t*)ps_malloc(idxBufSize(isTL));
  memcpy(idxBuf[isTL], idx1Buf, 4); // index header
  idxPtr[isTL] = CHUNK_HDR;  // leave 4 bytes for index size
  moviSize[isTL] = indexLen[isTL] = 0;
  idxOffset[isTL] = 4; // 4 byte offset
  // audio only applies to motion capture, and timelapse can start midway through
  // a motion recording, so leave the audio totals alone for isTL
  if (!isTL) audSize = audChunkCnt = 0;
}

void buildAviHdr(uint8_t FPS, uint8_t frameType, uint16_t frameCnt, bool isTL) {
  // update AVI header template with file specific details
  size_t audCnt = isTL ? 0 : audChunkCnt; // timelapse never has audio
  size_t chunkCnt = frameCnt + audCnt;    // every chunk has a header and an index entry
  size_t aviSize = moviSize[isTL] + AVI_HEADER_LEN + ((CHUNK_HDR+IDX_ENTRY) * chunkCnt); // AVI content size
  // update aviHeader with relevant stats
  memcpy(aviHeader+4, &aviSize, 4);
  uint32_t usecs = (uint32_t)round(1000000.0f / FPS); // usecs_per_frame
  memcpy(aviHeader+0x20, &usecs, 4);
  memcpy(aviHeader+0x30, &frameCnt, 2);
  memcpy(aviHeader+0x8C, &frameCnt, 2);
  memcpy(aviHeader+0x84, &FPS, 1);
  uint32_t dataSize = moviSize[isTL] + (chunkCnt * CHUNK_HDR) + 4;
  memcpy(aviHeader+0x12E, &dataSize, 4); // data size

  // apply video framesize to avi header
  memcpy(aviHeader+0x40, frameSizeData[frameType].frameWidth, 2);
  memcpy(aviHeader+0xA8, frameSizeData[frameType].frameWidth, 2);
  memcpy(aviHeader+0x44, frameSizeData[frameType].frameHeight, 2);
  memcpy(aviHeader+0xAC, frameSizeData[frameType].frameHeight, 2);

  // aviHeader is a template reused by every recording, so the stream count must be
  // written on each pass - otherwise a silent file that follows one with audio
  // inherits its 2 and declares an audio stream that isn't there
  uint8_t numAviStreams = 1;
#if INCLUDE_AUDIO
  if (audCnt) numAviStreams = 2;
  // dwSampleSize for the audio stream is 2 and dwScale is 1, so dwLength is in samples
  uint32_t audSamples = audCnt ? audSize / 2 : 0;
  memcpy(aviHeader+0x100, &audSamples, 4);
  // apply audio details to avi header
  memcpy(aviHeader+0xF8, &SAMPLE_RATE, 4);
  uint32_t bytesPerSec = SAMPLE_RATE * 2;
  memcpy(aviHeader+0x104, &bytesPerSec, 4); // suggested buffer size
  memcpy(aviHeader+0x11C, &SAMPLE_RATE, 4);
  memcpy(aviHeader+0x120, &bytesPerSec, 4); // bytes per sec
#else
  memcpy(aviHeader+0x100, zeroBuf, 4);
#endif
  memcpy(aviHeader+0x38, &numAviStreams, 1);

  // reset state for next recording
  moviSize[isTL] = idxPtr[isTL] = 0;
  idxOffset[isTL] = 4; // 4 byte offset
}

void buildAviIdx(size_t dataSize, bool isVid, bool isTL) {
  // build AVI video index into buffer - 16 bytes per frame
  // called from saveFrame() for each frame
  moviSize[isTL] += dataSize;
  if (idxPtr[isTL] + IDX_ENTRY > idxBufSize(isTL)) doRestart("Need to reboot if max frames changed");
  if (isVid) memcpy(idxBuf[isTL]+idxPtr[isTL], dcBuf, 4);
  else {
    memcpy(idxBuf[isTL]+idxPtr[isTL], wbBuf, 4);
    if (!isTL) {
      audSize += dataSize;
      audChunkCnt++;
    }
  }
  memcpy(idxBuf[isTL]+idxPtr[isTL]+4, zeroBuf, 4);
  memcpy(idxBuf[isTL]+idxPtr[isTL]+8, &idxOffset[isTL], 4); 
  memcpy(idxBuf[isTL]+idxPtr[isTL]+12, &dataSize, 4); 
  idxOffset[isTL] += dataSize + CHUNK_HDR;
  idxPtr[isTL] += IDX_ENTRY; 
}

size_t writeAviIndex(byte* clientBuf, size_t buffSize, bool isTL) {
  // write completed index to avi file
  // called repeatedly from closeAvi() until return 0
  if (idxPtr[isTL] < indexLen[isTL]) {
    if (indexLen[isTL]-idxPtr[isTL] > buffSize) {
      memcpy(clientBuf, idxBuf[isTL]+idxPtr[isTL], buffSize);
      idxPtr[isTL] += buffSize;
      return buffSize;
    } else {
      // final part of index
      size_t final = indexLen[isTL]-idxPtr[isTL];
      memcpy(clientBuf, idxBuf[isTL]+idxPtr[isTL], final);
      idxPtr[isTL] = indexLen[isTL];
      return final;
    }
  }
  return idxPtr[isTL] = 0;
}
  
void finalizeAviIndex(uint16_t frameCnt, bool isTL) {
  // update index with size
  uint32_t sizeOfIndex = (frameCnt + (isTL ? 0 : audChunkCnt)) * IDX_ENTRY;
  memcpy(idxBuf[isTL]+4, &sizeOfIndex, 4); // size of index 
  indexLen[isTL] = sizeOfIndex + CHUNK_HDR;
  idxPtr[isTL] = 0; // pointer to index buffer
}

