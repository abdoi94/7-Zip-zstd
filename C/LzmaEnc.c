<<<<<<< HEAD
/* LzmaEnc.c -- LZMA Encoder
2017-06-22 : Igor Pavlov : Public domain */

#include "Precomp.h"

#include <string.h>

/* #define SHOW_STAT */
/* #define SHOW_STAT2 */

#if defined(SHOW_STAT) || defined(SHOW_STAT2)
#include <stdio.h>
#endif

#include "LzmaEnc.h"

#include "LzFind.h"
#ifndef _7ZIP_ST
#include "LzFindMt.h"
#endif

#ifdef SHOW_STAT
static unsigned g_STAT_OFFSET = 0;
#endif

#define kLzmaMaxHistorySize ((UInt32)3 << 29)
/* #define kLzmaMaxHistorySize ((UInt32)7 << 29) */

#define kBlockSizeMax ((1 << LZMA_NUM_BLOCK_SIZE_BITS) - 1)

#define kBlockSize (9 << 10)
#define kUnpackBlockSize (1 << 18)
#define kMatchArraySize (1 << 21)
#define kMatchRecordMaxSize ((LZMA_MATCH_LEN_MAX * 2 + 3) * LZMA_MATCH_LEN_MAX)

#define kNumMaxDirectBits (31)

#define kNumTopBits 24
#define kTopValue ((UInt32)1 << kNumTopBits)

#define kNumBitModelTotalBits 11
#define kBitModelTotal (1 << kNumBitModelTotalBits)
#define kNumMoveBits 5
#define kProbInitValue (kBitModelTotal >> 1)

#define kNumMoveReducingBits 4
#define kNumBitPriceShiftBits 4
#define kBitPrice (1 << kNumBitPriceShiftBits)

void LzmaEncProps_Init(CLzmaEncProps *p)
{
  p->level = 5;
  p->dictSize = p->mc = 0;
  p->reduceSize = (UInt64)(Int64)-1;
  p->lc = p->lp = p->pb = p->algo = p->fb = p->btMode = p->numHashBytes = p->numThreads = -1;
  p->writeEndMark = 0;
}

void LzmaEncProps_Normalize(CLzmaEncProps *p)
{
  int level = p->level;
  if (level < 0) level = 5;
  p->level = level;
  
  if (p->dictSize == 0) p->dictSize = (level <= 5 ? (1 << (level * 2 + 14)) : (level <= 7 ? (1 << 25) : (1 << 26)));
  if (p->dictSize > p->reduceSize)
  {
    unsigned i;
    UInt32 reduceSize = (UInt32)p->reduceSize;
    for (i = 11; i <= 30; i++)
    {
      if (reduceSize <= ((UInt32)2 << i)) { p->dictSize = ((UInt32)2 << i); break; }
      if (reduceSize <= ((UInt32)3 << i)) { p->dictSize = ((UInt32)3 << i); break; }
    }
  }

  if (p->lc < 0) p->lc = 3;
  if (p->lp < 0) p->lp = 0;
  if (p->pb < 0) p->pb = 2;

  if (p->algo < 0) p->algo = (level < 5 ? 0 : 1);
  if (p->fb < 0) p->fb = (level < 7 ? 32 : 64);
  if (p->btMode < 0) p->btMode = (p->algo == 0 ? 0 : 1);
  if (p->numHashBytes < 0) p->numHashBytes = 4;
  if (p->mc == 0) p->mc = (16 + (p->fb >> 1)) >> (p->btMode ? 0 : 1);
  
  if (p->numThreads < 0)
    p->numThreads =
      #ifndef _7ZIP_ST
      ((p->btMode && p->algo) ? 2 : 1);
      #else
      1;
      #endif
}

UInt32 LzmaEncProps_GetDictSize(const CLzmaEncProps *props2)
{
  CLzmaEncProps props = *props2;
  LzmaEncProps_Normalize(&props);
  return props.dictSize;
}

#if (_MSC_VER >= 1400)
/* BSR code is fast for some new CPUs */
/* #define LZMA_LOG_BSR */
#endif

#ifdef LZMA_LOG_BSR

#define kDicLogSizeMaxCompress 32

#define BSR2_RET(pos, res) { unsigned long zz; _BitScanReverse(&zz, (pos)); res = (zz + zz) + ((pos >> (zz - 1)) & 1); }

static UInt32 GetPosSlot1(UInt32 pos)
{
  UInt32 res;
  BSR2_RET(pos, res);
  return res;
}
#define GetPosSlot2(pos, res) { BSR2_RET(pos, res); }
#define GetPosSlot(pos, res) { if (pos < 2) res = pos; else BSR2_RET(pos, res); }

#else

#define kNumLogBits (9 + sizeof(size_t) / 2)
/* #define kNumLogBits (11 + sizeof(size_t) / 8 * 3) */

#define kDicLogSizeMaxCompress ((kNumLogBits - 1) * 2 + 7)

static void LzmaEnc_FastPosInit(Byte *g_FastPos)
{
  unsigned slot;
  g_FastPos[0] = 0;
  g_FastPos[1] = 1;
  g_FastPos += 2;
  
  for (slot = 2; slot < kNumLogBits * 2; slot++)
  {
    size_t k = ((size_t)1 << ((slot >> 1) - 1));
    size_t j;
    for (j = 0; j < k; j++)
      g_FastPos[j] = (Byte)slot;
    g_FastPos += k;
  }
}

/* we can use ((limit - pos) >> 31) only if (pos < ((UInt32)1 << 31)) */
/*
#define BSR2_RET(pos, res) { UInt32 zz = 6 + ((kNumLogBits - 1) & \
  (0 - (((((UInt32)1 << (kNumLogBits + 6)) - 1) - pos) >> 31))); \
  res = p->g_FastPos[pos >> zz] + (zz * 2); }
*/

/*
#define BSR2_RET(pos, res) { UInt32 zz = 6 + ((kNumLogBits - 1) & \
  (0 - (((((UInt32)1 << (kNumLogBits)) - 1) - (pos >> 6)) >> 31))); \
  res = p->g_FastPos[pos >> zz] + (zz * 2); }
*/

#define BSR2_RET(pos, res) { UInt32 zz = (pos < (1 << (kNumLogBits + 6))) ? 6 : 6 + kNumLogBits - 1; \
  res = p->g_FastPos[pos >> zz] + (zz * 2); }

/*
#define BSR2_RET(pos, res) { res = (pos < (1 << (kNumLogBits + 6))) ? \
  p->g_FastPos[pos >> 6] + 12 : \
  p->g_FastPos[pos >> (6 + kNumLogBits - 1)] + (6 + (kNumLogBits - 1)) * 2; }
*/

#define GetPosSlot1(pos) p->g_FastPos[pos]
#define GetPosSlot2(pos, res) { BSR2_RET(pos, res); }
#define GetPosSlot(pos, res) { if (pos < kNumFullDistances) res = p->g_FastPos[pos]; else BSR2_RET(pos, res); }

#endif


#define LZMA_NUM_REPS 4

typedef unsigned CState;

typedef struct
{
  UInt32 price;

  CState state;
  int prev1IsChar;
  int prev2;

  UInt32 posPrev2;
  UInt32 backPrev2;

  UInt32 posPrev;
  UInt32 backPrev;
  UInt32 backs[LZMA_NUM_REPS];
} COptimal;

#define kNumOpts (1 << 12)

#define kNumLenToPosStates 4
#define kNumPosSlotBits 6
#define kDicLogSizeMin 0
#define kDicLogSizeMax 32
#define kDistTableSizeMax (kDicLogSizeMax * 2)


#define kNumAlignBits 4
#define kAlignTableSize (1 << kNumAlignBits)
#define kAlignMask (kAlignTableSize - 1)

#define kStartPosModelIndex 4
#define kEndPosModelIndex 14
#define kNumPosModels (kEndPosModelIndex - kStartPosModelIndex)

#define kNumFullDistances (1 << (kEndPosModelIndex >> 1))

#ifdef _LZMA_PROB32
#define CLzmaProb UInt32
#else
#define CLzmaProb UInt16
#endif

#define LZMA_PB_MAX 4
#define LZMA_LC_MAX 8
#define LZMA_LP_MAX 4

#define LZMA_NUM_PB_STATES_MAX (1 << LZMA_PB_MAX)


#define kLenNumLowBits 3
#define kLenNumLowSymbols (1 << kLenNumLowBits)
#define kLenNumMidBits 3
#define kLenNumMidSymbols (1 << kLenNumMidBits)
#define kLenNumHighBits 8
#define kLenNumHighSymbols (1 << kLenNumHighBits)

#define kLenNumSymbolsTotal (kLenNumLowSymbols + kLenNumMidSymbols + kLenNumHighSymbols)

#define LZMA_MATCH_LEN_MIN 2
#define LZMA_MATCH_LEN_MAX (LZMA_MATCH_LEN_MIN + kLenNumSymbolsTotal - 1)

#define kNumStates 12


typedef struct
{
  CLzmaProb choice;
  CLzmaProb choice2;
  CLzmaProb low[LZMA_NUM_PB_STATES_MAX << kLenNumLowBits];
  CLzmaProb mid[LZMA_NUM_PB_STATES_MAX << kLenNumMidBits];
  CLzmaProb high[kLenNumHighSymbols];
} CLenEnc;


typedef struct
{
  CLenEnc p;
  UInt32 tableSize;
  UInt32 prices[LZMA_NUM_PB_STATES_MAX][kLenNumSymbolsTotal];
  UInt32 counters[LZMA_NUM_PB_STATES_MAX];
} CLenPriceEnc;


typedef struct
{
  UInt32 range;
  Byte cache;
  UInt64 low;
  UInt64 cacheSize;
  Byte *buf;
  Byte *bufLim;
  Byte *bufBase;
  ISeqOutStream *outStream;
  UInt64 processed;
  SRes res;
} CRangeEnc;


typedef struct
{
  CLzmaProb *litProbs;

  UInt32 state;
  UInt32 reps[LZMA_NUM_REPS];

  CLzmaProb isMatch[kNumStates][LZMA_NUM_PB_STATES_MAX];
  CLzmaProb isRep[kNumStates];
  CLzmaProb isRepG0[kNumStates];
  CLzmaProb isRepG1[kNumStates];
  CLzmaProb isRepG2[kNumStates];
  CLzmaProb isRep0Long[kNumStates][LZMA_NUM_PB_STATES_MAX];

  CLzmaProb posSlotEncoder[kNumLenToPosStates][1 << kNumPosSlotBits];
  CLzmaProb posEncoders[kNumFullDistances - kEndPosModelIndex];
  CLzmaProb posAlignEncoder[1 << kNumAlignBits];
  
  CLenPriceEnc lenEnc;
  CLenPriceEnc repLenEnc;
} CSaveState;


typedef struct
{
  void *matchFinderObj;
  IMatchFinder matchFinder;

  UInt32 optimumEndIndex;
  UInt32 optimumCurrentIndex;

  UInt32 longestMatchLength;
  UInt32 numPairs;
  UInt32 numAvail;

  UInt32 numFastBytes;
  UInt32 additionalOffset;
  UInt32 reps[LZMA_NUM_REPS];
  UInt32 state;

  unsigned lc, lp, pb;
  unsigned lpMask, pbMask;
  unsigned lclp;

  CLzmaProb *litProbs;

  Bool fastMode;
  Bool writeEndMark;
  Bool finished;
  Bool multiThread;
  Bool needInit;

  UInt64 nowPos64;
  
  UInt32 matchPriceCount;
  UInt32 alignPriceCount;

  UInt32 distTableSize;

  UInt32 dictSize;
  SRes result;

  CRangeEnc rc;

  #ifndef _7ZIP_ST
  Bool mtMode;
  CMatchFinderMt matchFinderMt;
  #endif

  CMatchFinder matchFinderBase;

  #ifndef _7ZIP_ST
  Byte pad[128];
  #endif
  
  COptimal opt[kNumOpts];
  
  #ifndef LZMA_LOG_BSR
  Byte g_FastPos[1 << kNumLogBits];
  #endif

  UInt32 ProbPrices[kBitModelTotal >> kNumMoveReducingBits];
  UInt32 matches[LZMA_MATCH_LEN_MAX * 2 + 2 + 1];

  UInt32 posSlotPrices[kNumLenToPosStates][kDistTableSizeMax];
  UInt32 distancesPrices[kNumLenToPosStates][kNumFullDistances];
  UInt32 alignPrices[kAlignTableSize];

  CLzmaProb isMatch[kNumStates][LZMA_NUM_PB_STATES_MAX];
  CLzmaProb isRep[kNumStates];
  CLzmaProb isRepG0[kNumStates];
  CLzmaProb isRepG1[kNumStates];
  CLzmaProb isRepG2[kNumStates];
  CLzmaProb isRep0Long[kNumStates][LZMA_NUM_PB_STATES_MAX];

  CLzmaProb posSlotEncoder[kNumLenToPosStates][1 << kNumPosSlotBits];
  CLzmaProb posEncoders[kNumFullDistances - kEndPosModelIndex];
  CLzmaProb posAlignEncoder[1 << kNumAlignBits];
  
  CLenPriceEnc lenEnc;
  CLenPriceEnc repLenEnc;

  CSaveState saveState;

  #ifndef _7ZIP_ST
  Byte pad2[128];
  #endif
} CLzmaEnc;


void LzmaEnc_SaveState(CLzmaEncHandle pp)
{
  CLzmaEnc *p = (CLzmaEnc *)pp;
  CSaveState *dest = &p->saveState;
  int i;
  dest->lenEnc = p->lenEnc;
  dest->repLenEnc = p->repLenEnc;
  dest->state = p->state;

  for (i = 0; i < kNumStates; i++)
  {
    memcpy(dest->isMatch[i], p->isMatch[i], sizeof(p->isMatch[i]));
    memcpy(dest->isRep0Long[i], p->isRep0Long[i], sizeof(p->isRep0Long[i]));
  }
  for (i = 0; i < kNumLenToPosStates; i++)
    memcpy(dest->posSlotEncoder[i], p->posSlotEncoder[i], sizeof(p->posSlotEncoder[i]));
  memcpy(dest->isRep, p->isRep, sizeof(p->isRep));
  memcpy(dest->isRepG0, p->isRepG0, sizeof(p->isRepG0));
  memcpy(dest->isRepG1, p->isRepG1, sizeof(p->isRepG1));
  memcpy(dest->isRepG2, p->isRepG2, sizeof(p->isRepG2));
  memcpy(dest->posEncoders, p->posEncoders, sizeof(p->posEncoders));
  memcpy(dest->posAlignEncoder, p->posAlignEncoder, sizeof(p->posAlignEncoder));
  memcpy(dest->reps, p->reps, sizeof(p->reps));
  memcpy(dest->litProbs, p->litProbs, ((UInt32)0x300 << p->lclp) * sizeof(CLzmaProb));
}

void LzmaEnc_RestoreState(CLzmaEncHandle pp)
{
  CLzmaEnc *dest = (CLzmaEnc *)pp;
  const CSaveState *p = &dest->saveState;
  int i;
  dest->lenEnc = p->lenEnc;
  dest->repLenEnc = p->repLenEnc;
  dest->state = p->state;

  for (i = 0; i < kNumStates; i++)
  {
    memcpy(dest->isMatch[i], p->isMatch[i], sizeof(p->isMatch[i]));
    memcpy(dest->isRep0Long[i], p->isRep0Long[i], sizeof(p->isRep0Long[i]));
  }
  for (i = 0; i < kNumLenToPosStates; i++)
    memcpy(dest->posSlotEncoder[i], p->posSlotEncoder[i], sizeof(p->posSlotEncoder[i]));
  memcpy(dest->isRep, p->isRep, sizeof(p->isRep));
  memcpy(dest->isRepG0, p->isRepG0, sizeof(p->isRepG0));
  memcpy(dest->isRepG1, p->isRepG1, sizeof(p->isRepG1));
  memcpy(dest->isRepG2, p->isRepG2, sizeof(p->isRepG2));
  memcpy(dest->posEncoders, p->posEncoders, sizeof(p->posEncoders));
  memcpy(dest->posAlignEncoder, p->posAlignEncoder, sizeof(p->posAlignEncoder));
  memcpy(dest->reps, p->reps, sizeof(p->reps));
  memcpy(dest->litProbs, p->litProbs, ((UInt32)0x300 << dest->lclp) * sizeof(CLzmaProb));
}

SRes LzmaEnc_SetProps(CLzmaEncHandle pp, const CLzmaEncProps *props2)
{
  CLzmaEnc *p = (CLzmaEnc *)pp;
  CLzmaEncProps props = *props2;
  LzmaEncProps_Normalize(&props);

  if (props.lc > LZMA_LC_MAX
      || props.lp > LZMA_LP_MAX
      || props.pb > LZMA_PB_MAX
      || props.dictSize > ((UInt64)1 << kDicLogSizeMaxCompress)
      || props.dictSize > kLzmaMaxHistorySize)
    return SZ_ERROR_PARAM;

  p->dictSize = props.dictSize;
  {
    unsigned fb = props.fb;
    if (fb < 5)
      fb = 5;
    if (fb > LZMA_MATCH_LEN_MAX)
      fb = LZMA_MATCH_LEN_MAX;
    p->numFastBytes = fb;
  }
  p->lc = props.lc;
  p->lp = props.lp;
  p->pb = props.pb;
  p->fastMode = (props.algo == 0);
  p->matchFinderBase.btMode = (Byte)(props.btMode ? 1 : 0);
  {
    UInt32 numHashBytes = 4;
    if (props.btMode)
    {
      if (props.numHashBytes < 2)
        numHashBytes = 2;
      else if (props.numHashBytes < 4)
        numHashBytes = props.numHashBytes;
    }
    p->matchFinderBase.numHashBytes = numHashBytes;
  }

  p->matchFinderBase.cutValue = props.mc;

  p->writeEndMark = props.writeEndMark;

  #ifndef _7ZIP_ST
  /*
  if (newMultiThread != _multiThread)
  {
    ReleaseMatchFinder();
    _multiThread = newMultiThread;
  }
  */
  p->multiThread = (props.numThreads > 1);
  #endif

  return SZ_OK;
}


void LzmaEnc_SetDataSize(CLzmaEncHandle pp, UInt64 expectedDataSiize)
{
  CLzmaEnc *p = (CLzmaEnc *)pp;
  p->matchFinderBase.expectedDataSize = expectedDataSiize;
}



static const int kLiteralNextStates[kNumStates] = {0, 0, 0, 0, 1, 2, 3, 4,  5,  6,   4, 5};
static const int kMatchNextStates[kNumStates]   = {7, 7, 7, 7, 7, 7, 7, 10, 10, 10, 10, 10};
static const int kRepNextStates[kNumStates]     = {8, 8, 8, 8, 8, 8, 8, 11, 11, 11, 11, 11};
static const int kShortRepNextStates[kNumStates]= {9, 9, 9, 9, 9, 9, 9, 11, 11, 11, 11, 11};

#define IsCharState(s) ((s) < 7)

#define GetLenToPosState(len) (((len) < kNumLenToPosStates + 1) ? (len) - 2 : kNumLenToPosStates - 1)

#define kInfinityPrice (1 << 30)

static void RangeEnc_Construct(CRangeEnc *p)
{
  p->outStream = NULL;
  p->bufBase = NULL;
}

#define RangeEnc_GetProcessed(p) ((p)->processed + ((p)->buf - (p)->bufBase) + (p)->cacheSize)

#define RC_BUF_SIZE (1 << 16)
static int RangeEnc_Alloc(CRangeEnc *p, ISzAllocPtr alloc)
{
  if (!p->bufBase)
  {
    p->bufBase = (Byte *)ISzAlloc_Alloc(alloc, RC_BUF_SIZE);
    if (!p->bufBase)
      return 0;
    p->bufLim = p->bufBase + RC_BUF_SIZE;
  }
  return 1;
}

static void RangeEnc_Free(CRangeEnc *p, ISzAllocPtr alloc)
{
  ISzAlloc_Free(alloc, p->bufBase);
  p->bufBase = 0;
}

static void RangeEnc_Init(CRangeEnc *p)
{
  /* Stream.Init(); */
  p->low = 0;
  p->range = 0xFFFFFFFF;
  p->cacheSize = 1;
  p->cache = 0;

  p->buf = p->bufBase;

  p->processed = 0;
  p->res = SZ_OK;
}

static void RangeEnc_FlushStream(CRangeEnc *p)
{
  size_t num;
  if (p->res != SZ_OK)
    return;
  num = p->buf - p->bufBase;
  if (num != ISeqOutStream_Write(p->outStream, p->bufBase, num))
    p->res = SZ_ERROR_WRITE;
  p->processed += num;
  p->buf = p->bufBase;
}

static void MY_FAST_CALL RangeEnc_ShiftLow(CRangeEnc *p)
{
  if ((UInt32)p->low < (UInt32)0xFF000000 || (unsigned)(p->low >> 32) != 0)
  {
    Byte temp = p->cache;
    do
    {
      Byte *buf = p->buf;
      *buf++ = (Byte)(temp + (Byte)(p->low >> 32));
      p->buf = buf;
      if (buf == p->bufLim)
        RangeEnc_FlushStream(p);
      temp = 0xFF;
    }
    while (--p->cacheSize != 0);
    p->cache = (Byte)((UInt32)p->low >> 24);
  }
  p->cacheSize++;
  p->low = (UInt32)p->low << 8;
}

static void RangeEnc_FlushData(CRangeEnc *p)
{
  int i;
  for (i = 0; i < 5; i++)
    RangeEnc_ShiftLow(p);
}

static void RangeEnc_EncodeDirectBits(CRangeEnc *p, UInt32 value, unsigned numBits)
{
  do
  {
    p->range >>= 1;
    p->low += p->range & (0 - ((value >> --numBits) & 1));
    if (p->range < kTopValue)
    {
      p->range <<= 8;
      RangeEnc_ShiftLow(p);
    }
  }
  while (numBits != 0);
}

static void RangeEnc_EncodeBit(CRangeEnc *p, CLzmaProb *prob, UInt32 symbol)
{
  UInt32 ttt = *prob;
  UInt32 newBound = (p->range >> kNumBitModelTotalBits) * ttt;
  if (symbol == 0)
  {
    p->range = newBound;
    ttt += (kBitModelTotal - ttt) >> kNumMoveBits;
  }
  else
  {
    p->low += newBound;
    p->range -= newBound;
    ttt -= ttt >> kNumMoveBits;
  }
  *prob = (CLzmaProb)ttt;
  if (p->range < kTopValue)
  {
    p->range <<= 8;
    RangeEnc_ShiftLow(p);
  }
}

static void LitEnc_Encode(CRangeEnc *p, CLzmaProb *probs, UInt32 symbol)
{
  symbol |= 0x100;
  do
  {
    RangeEnc_EncodeBit(p, probs + (symbol >> 8), (symbol >> 7) & 1);
    symbol <<= 1;
  }
  while (symbol < 0x10000);
}

static void LitEnc_EncodeMatched(CRangeEnc *p, CLzmaProb *probs, UInt32 symbol, UInt32 matchByte)
{
  UInt32 offs = 0x100;
  symbol |= 0x100;
  do
  {
    matchByte <<= 1;
    RangeEnc_EncodeBit(p, probs + (offs + (matchByte & offs) + (symbol >> 8)), (symbol >> 7) & 1);
    symbol <<= 1;
    offs &= ~(matchByte ^ symbol);
  }
  while (symbol < 0x10000);
}

static void LzmaEnc_InitPriceTables(UInt32 *ProbPrices)
{
  UInt32 i;
  for (i = (1 << kNumMoveReducingBits) / 2; i < kBitModelTotal; i += (1 << kNumMoveReducingBits))
  {
    const int kCyclesBits = kNumBitPriceShiftBits;
    UInt32 w = i;
    UInt32 bitCount = 0;
    int j;
    for (j = 0; j < kCyclesBits; j++)
    {
      w = w * w;
      bitCount <<= 1;
      while (w >= ((UInt32)1 << 16))
      {
        w >>= 1;
        bitCount++;
      }
    }
    ProbPrices[i >> kNumMoveReducingBits] = ((kNumBitModelTotalBits << kCyclesBits) - 15 - bitCount);
  }
}


#define GET_PRICE(prob, symbol) \
  p->ProbPrices[((prob) ^ (((-(int)(symbol))) & (kBitModelTotal - 1))) >> kNumMoveReducingBits];

#define GET_PRICEa(prob, symbol) \
  ProbPrices[((prob) ^ ((-((int)(symbol))) & (kBitModelTotal - 1))) >> kNumMoveReducingBits];

#define GET_PRICE_0(prob) p->ProbPrices[(prob) >> kNumMoveReducingBits]
#define GET_PRICE_1(prob) p->ProbPrices[((prob) ^ (kBitModelTotal - 1)) >> kNumMoveReducingBits]

#define GET_PRICE_0a(prob) ProbPrices[(prob) >> kNumMoveReducingBits]
#define GET_PRICE_1a(prob) ProbPrices[((prob) ^ (kBitModelTotal - 1)) >> kNumMoveReducingBits]

static UInt32 LitEnc_GetPrice(const CLzmaProb *probs, UInt32 symbol, const UInt32 *ProbPrices)
{
  UInt32 price = 0;
  symbol |= 0x100;
  do
  {
    price += GET_PRICEa(probs[symbol >> 8], (symbol >> 7) & 1);
    symbol <<= 1;
  }
  while (symbol < 0x10000);
  return price;
}

static UInt32 LitEnc_GetPriceMatched(const CLzmaProb *probs, UInt32 symbol, UInt32 matchByte, const UInt32 *ProbPrices)
{
  UInt32 price = 0;
  UInt32 offs = 0x100;
  symbol |= 0x100;
  do
  {
    matchByte <<= 1;
    price += GET_PRICEa(probs[offs + (matchByte & offs) + (symbol >> 8)], (symbol >> 7) & 1);
    symbol <<= 1;
    offs &= ~(matchByte ^ symbol);
  }
  while (symbol < 0x10000);
  return price;
}


static void RcTree_Encode(CRangeEnc *rc, CLzmaProb *probs, int numBitLevels, UInt32 symbol)
{
  UInt32 m = 1;
  int i;
  for (i = numBitLevels; i != 0;)
  {
    UInt32 bit;
    i--;
    bit = (symbol >> i) & 1;
    RangeEnc_EncodeBit(rc, probs + m, bit);
    m = (m << 1) | bit;
  }
}

static void RcTree_ReverseEncode(CRangeEnc *rc, CLzmaProb *probs, int numBitLevels, UInt32 symbol)
{
  UInt32 m = 1;
  int i;
  for (i = 0; i < numBitLevels; i++)
  {
    UInt32 bit = symbol & 1;
    RangeEnc_EncodeBit(rc, probs + m, bit);
    m = (m << 1) | bit;
    symbol >>= 1;
  }
}

static UInt32 RcTree_GetPrice(const CLzmaProb *probs, int numBitLevels, UInt32 symbol, const UInt32 *ProbPrices)
{
  UInt32 price = 0;
  symbol |= (1 << numBitLevels);
  while (symbol != 1)
  {
    price += GET_PRICEa(probs[symbol >> 1], symbol & 1);
    symbol >>= 1;
  }
  return price;
}

static UInt32 RcTree_ReverseGetPrice(const CLzmaProb *probs, int numBitLevels, UInt32 symbol, const UInt32 *ProbPrices)
{
  UInt32 price = 0;
  UInt32 m = 1;
  int i;
  for (i = numBitLevels; i != 0; i--)
  {
    UInt32 bit = symbol & 1;
    symbol >>= 1;
    price += GET_PRICEa(probs[m], bit);
    m = (m << 1) | bit;
  }
  return price;
}


static void LenEnc_Init(CLenEnc *p)
{
  unsigned i;
  p->choice = p->choice2 = kProbInitValue;
  for (i = 0; i < (LZMA_NUM_PB_STATES_MAX << kLenNumLowBits); i++)
    p->low[i] = kProbInitValue;
  for (i = 0; i < (LZMA_NUM_PB_STATES_MAX << kLenNumMidBits); i++)
    p->mid[i] = kProbInitValue;
  for (i = 0; i < kLenNumHighSymbols; i++)
    p->high[i] = kProbInitValue;
}

static void LenEnc_Encode(CLenEnc *p, CRangeEnc *rc, UInt32 symbol, UInt32 posState)
{
  if (symbol < kLenNumLowSymbols)
  {
    RangeEnc_EncodeBit(rc, &p->choice, 0);
    RcTree_Encode(rc, p->low + (posState << kLenNumLowBits), kLenNumLowBits, symbol);
  }
  else
  {
    RangeEnc_EncodeBit(rc, &p->choice, 1);
    if (symbol < kLenNumLowSymbols + kLenNumMidSymbols)
    {
      RangeEnc_EncodeBit(rc, &p->choice2, 0);
      RcTree_Encode(rc, p->mid + (posState << kLenNumMidBits), kLenNumMidBits, symbol - kLenNumLowSymbols);
    }
    else
    {
      RangeEnc_EncodeBit(rc, &p->choice2, 1);
      RcTree_Encode(rc, p->high, kLenNumHighBits, symbol - kLenNumLowSymbols - kLenNumMidSymbols);
    }
  }
}

static void LenEnc_SetPrices(CLenEnc *p, UInt32 posState, UInt32 numSymbols, UInt32 *prices, const UInt32 *ProbPrices)
{
  UInt32 a0 = GET_PRICE_0a(p->choice);
  UInt32 a1 = GET_PRICE_1a(p->choice);
  UInt32 b0 = a1 + GET_PRICE_0a(p->choice2);
  UInt32 b1 = a1 + GET_PRICE_1a(p->choice2);
  UInt32 i = 0;
  for (i = 0; i < kLenNumLowSymbols; i++)
  {
    if (i >= numSymbols)
      return;
    prices[i] = a0 + RcTree_GetPrice(p->low + (posState << kLenNumLowBits), kLenNumLowBits, i, ProbPrices);
  }
  for (; i < kLenNumLowSymbols + kLenNumMidSymbols; i++)
  {
    if (i >= numSymbols)
      return;
    prices[i] = b0 + RcTree_GetPrice(p->mid + (posState << kLenNumMidBits), kLenNumMidBits, i - kLenNumLowSymbols, ProbPrices);
  }
  for (; i < numSymbols; i++)
    prices[i] = b1 + RcTree_GetPrice(p->high, kLenNumHighBits, i - kLenNumLowSymbols - kLenNumMidSymbols, ProbPrices);
}

static void MY_FAST_CALL LenPriceEnc_UpdateTable(CLenPriceEnc *p, UInt32 posState, const UInt32 *ProbPrices)
{
  LenEnc_SetPrices(&p->p, posState, p->tableSize, p->prices[posState], ProbPrices);
  p->counters[posState] = p->tableSize;
}

static void LenPriceEnc_UpdateTables(CLenPriceEnc *p, UInt32 numPosStates, const UInt32 *ProbPrices)
{
  UInt32 posState;
  for (posState = 0; posState < numPosStates; posState++)
    LenPriceEnc_UpdateTable(p, posState, ProbPrices);
}

static void LenEnc_Encode2(CLenPriceEnc *p, CRangeEnc *rc, UInt32 symbol, UInt32 posState, Bool updatePrice, const UInt32 *ProbPrices)
{
  LenEnc_Encode(&p->p, rc, symbol, posState);
  if (updatePrice)
    if (--p->counters[posState] == 0)
      LenPriceEnc_UpdateTable(p, posState, ProbPrices);
}




static void MovePos(CLzmaEnc *p, UInt32 num)
{
  #ifdef SHOW_STAT
  g_STAT_OFFSET += num;
  printf("\n MovePos %u", num);
  #endif
  
  if (num != 0)
  {
    p->additionalOffset += num;
    p->matchFinder.Skip(p->matchFinderObj, num);
  }
}

static UInt32 ReadMatchDistances(CLzmaEnc *p, UInt32 *numDistancePairsRes)
{
  UInt32 lenRes = 0, numPairs;
  p->numAvail = p->matchFinder.GetNumAvailableBytes(p->matchFinderObj);
  numPairs = p->matchFinder.GetMatches(p->matchFinderObj, p->matches);
  
  #ifdef SHOW_STAT
  printf("\n i = %u numPairs = %u    ", g_STAT_OFFSET, numPairs / 2);
  g_STAT_OFFSET++;
  {
    UInt32 i;
    for (i = 0; i < numPairs; i += 2)
      printf("%2u %6u   | ", p->matches[i], p->matches[i + 1]);
  }
  #endif
  
  if (numPairs > 0)
  {
    lenRes = p->matches[(size_t)numPairs - 2];
    if (lenRes == p->numFastBytes)
    {
      UInt32 numAvail = p->numAvail;
      if (numAvail > LZMA_MATCH_LEN_MAX)
        numAvail = LZMA_MATCH_LEN_MAX;
      {
        const Byte *pbyCur = p->matchFinder.GetPointerToCurrentPos(p->matchFinderObj) - 1;
        const Byte *pby = pbyCur + lenRes;
        ptrdiff_t dif = (ptrdiff_t)-1 - p->matches[(size_t)numPairs - 1];
        const Byte *pbyLim = pbyCur + numAvail;
        for (; pby != pbyLim && *pby == pby[dif]; pby++);
        lenRes = (UInt32)(pby - pbyCur);
      }
    }
  }
  p->additionalOffset++;
  *numDistancePairsRes = numPairs;
  return lenRes;
}


#define MakeAsChar(p) (p)->backPrev = (UInt32)(-1); (p)->prev1IsChar = False;
#define MakeAsShortRep(p) (p)->backPrev = 0; (p)->prev1IsChar = False;
#define IsShortRep(p) ((p)->backPrev == 0)

static UInt32 GetRepLen1Price(CLzmaEnc *p, UInt32 state, UInt32 posState)
{
  return
    GET_PRICE_0(p->isRepG0[state]) +
    GET_PRICE_0(p->isRep0Long[state][posState]);
}

static UInt32 GetPureRepPrice(CLzmaEnc *p, UInt32 repIndex, UInt32 state, UInt32 posState)
{
  UInt32 price;
  if (repIndex == 0)
  {
    price = GET_PRICE_0(p->isRepG0[state]);
    price += GET_PRICE_1(p->isRep0Long[state][posState]);
  }
  else
  {
    price = GET_PRICE_1(p->isRepG0[state]);
    if (repIndex == 1)
      price += GET_PRICE_0(p->isRepG1[state]);
    else
    {
      price += GET_PRICE_1(p->isRepG1[state]);
      price += GET_PRICE(p->isRepG2[state], repIndex - 2);
    }
  }
  return price;
}

static UInt32 GetRepPrice(CLzmaEnc *p, UInt32 repIndex, UInt32 len, UInt32 state, UInt32 posState)
{
  return p->repLenEnc.prices[posState][(size_t)len - LZMA_MATCH_LEN_MIN] +
    GetPureRepPrice(p, repIndex, state, posState);
}

static UInt32 Backward(CLzmaEnc *p, UInt32 *backRes, UInt32 cur)
{
  UInt32 posMem = p->opt[cur].posPrev;
  UInt32 backMem = p->opt[cur].backPrev;
  p->optimumEndIndex = cur;
  do
  {
    if (p->opt[cur].prev1IsChar)
    {
      MakeAsChar(&p->opt[posMem])
      p->opt[posMem].posPrev = posMem - 1;
      if (p->opt[cur].prev2)
      {
        p->opt[(size_t)posMem - 1].prev1IsChar = False;
        p->opt[(size_t)posMem - 1].posPrev = p->opt[cur].posPrev2;
        p->opt[(size_t)posMem - 1].backPrev = p->opt[cur].backPrev2;
      }
    }
    {
      UInt32 posPrev = posMem;
      UInt32 backCur = backMem;
      
      backMem = p->opt[posPrev].backPrev;
      posMem = p->opt[posPrev].posPrev;
      
      p->opt[posPrev].backPrev = backCur;
      p->opt[posPrev].posPrev = cur;
      cur = posPrev;
    }
  }
  while (cur != 0);
  *backRes = p->opt[0].backPrev;
  p->optimumCurrentIndex  = p->opt[0].posPrev;
  return p->optimumCurrentIndex;
}

#define LIT_PROBS(pos, prevByte) (p->litProbs + ((((pos) & p->lpMask) << p->lc) + ((prevByte) >> (8 - p->lc))) * (UInt32)0x300)

static UInt32 GetOptimum(CLzmaEnc *p, UInt32 position, UInt32 *backRes)
{
  UInt32 lenEnd, cur;
  UInt32 reps[LZMA_NUM_REPS], repLens[LZMA_NUM_REPS];
  UInt32 *matches;

  {

  UInt32 numAvail, mainLen, numPairs, repMaxIndex, i, posState, len;
  UInt32 matchPrice, repMatchPrice, normalMatchPrice;
  const Byte *data;
  Byte curByte, matchByte;

  if (p->optimumEndIndex != p->optimumCurrentIndex)
  {
    const COptimal *opt = &p->opt[p->optimumCurrentIndex];
    UInt32 lenRes = opt->posPrev - p->optimumCurrentIndex;
    *backRes = opt->backPrev;
    p->optimumCurrentIndex = opt->posPrev;
    return lenRes;
  }
  p->optimumCurrentIndex = p->optimumEndIndex = 0;
  
  if (p->additionalOffset == 0)
    mainLen = ReadMatchDistances(p, &numPairs);
  else
  {
    mainLen = p->longestMatchLength;
    numPairs = p->numPairs;
  }

  numAvail = p->numAvail;
  if (numAvail < 2)
  {
    *backRes = (UInt32)(-1);
    return 1;
  }
  if (numAvail > LZMA_MATCH_LEN_MAX)
    numAvail = LZMA_MATCH_LEN_MAX;

  data = p->matchFinder.GetPointerToCurrentPos(p->matchFinderObj) - 1;
  repMaxIndex = 0;
  for (i = 0; i < LZMA_NUM_REPS; i++)
  {
    UInt32 lenTest;
    const Byte *data2;
    reps[i] = p->reps[i];
    data2 = data - reps[i] - 1;
    if (data[0] != data2[0] || data[1] != data2[1])
    {
      repLens[i] = 0;
      continue;
    }
    for (lenTest = 2; lenTest < numAvail && data[lenTest] == data2[lenTest]; lenTest++);
    repLens[i] = lenTest;
    if (lenTest > repLens[repMaxIndex])
      repMaxIndex = i;
  }
  if (repLens[repMaxIndex] >= p->numFastBytes)
  {
    UInt32 lenRes;
    *backRes = repMaxIndex;
    lenRes = repLens[repMaxIndex];
    MovePos(p, lenRes - 1);
    return lenRes;
  }

  matches = p->matches;
  if (mainLen >= p->numFastBytes)
  {
    *backRes = matches[(size_t)numPairs - 1] + LZMA_NUM_REPS;
    MovePos(p, mainLen - 1);
    return mainLen;
  }
  curByte = *data;
  matchByte = *(data - (reps[0] + 1));

  if (mainLen < 2 && curByte != matchByte && repLens[repMaxIndex] < 2)
  {
    *backRes = (UInt32)-1;
    return 1;
  }

  p->opt[0].state = (CState)p->state;

  posState = (position & p->pbMask);

  {
    const CLzmaProb *probs = LIT_PROBS(position, *(data - 1));
    p->opt[1].price = GET_PRICE_0(p->isMatch[p->state][posState]) +
        (!IsCharState(p->state) ?
          LitEnc_GetPriceMatched(probs, curByte, matchByte, p->ProbPrices) :
          LitEnc_GetPrice(probs, curByte, p->ProbPrices));
  }

  MakeAsChar(&p->opt[1]);

  matchPrice = GET_PRICE_1(p->isMatch[p->state][posState]);
  repMatchPrice = matchPrice + GET_PRICE_1(p->isRep[p->state]);

  if (matchByte == curByte)
  {
    UInt32 shortRepPrice = repMatchPrice + GetRepLen1Price(p, p->state, posState);
    if (shortRepPrice < p->opt[1].price)
    {
      p->opt[1].price = shortRepPrice;
      MakeAsShortRep(&p->opt[1]);
    }
  }
  lenEnd = ((mainLen >= repLens[repMaxIndex]) ? mainLen : repLens[repMaxIndex]);

  if (lenEnd < 2)
  {
    *backRes = p->opt[1].backPrev;
    return 1;
  }

  p->opt[1].posPrev = 0;
  for (i = 0; i < LZMA_NUM_REPS; i++)
    p->opt[0].backs[i] = reps[i];

  len = lenEnd;
  do
    p->opt[len--].price = kInfinityPrice;
  while (len >= 2);

  for (i = 0; i < LZMA_NUM_REPS; i++)
  {
    UInt32 repLen = repLens[i];
    UInt32 price;
    if (repLen < 2)
      continue;
    price = repMatchPrice + GetPureRepPrice(p, i, p->state, posState);
    do
    {
      UInt32 curAndLenPrice = price + p->repLenEnc.prices[posState][(size_t)repLen - 2];
      COptimal *opt = &p->opt[repLen];
      if (curAndLenPrice < opt->price)
      {
        opt->price = curAndLenPrice;
        opt->posPrev = 0;
        opt->backPrev = i;
        opt->prev1IsChar = False;
      }
    }
    while (--repLen >= 2);
  }

  normalMatchPrice = matchPrice + GET_PRICE_0(p->isRep[p->state]);

  len = ((repLens[0] >= 2) ? repLens[0] + 1 : 2);
  if (len <= mainLen)
  {
    UInt32 offs = 0;
    while (len > matches[offs])
      offs += 2;
    for (; ; len++)
    {
      COptimal *opt;
      UInt32 distance = matches[(size_t)offs + 1];

      UInt32 curAndLenPrice = normalMatchPrice + p->lenEnc.prices[posState][(size_t)len - LZMA_MATCH_LEN_MIN];
      UInt32 lenToPosState = GetLenToPosState(len);
      if (distance < kNumFullDistances)
        curAndLenPrice += p->distancesPrices[lenToPosState][distance];
      else
      {
        UInt32 slot;
        GetPosSlot2(distance, slot);
        curAndLenPrice += p->alignPrices[distance & kAlignMask] + p->posSlotPrices[lenToPosState][slot];
      }
      opt = &p->opt[len];
      if (curAndLenPrice < opt->price)
      {
        opt->price = curAndLenPrice;
        opt->posPrev = 0;
        opt->backPrev = distance + LZMA_NUM_REPS;
        opt->prev1IsChar = False;
      }
      if (len == matches[offs])
      {
        offs += 2;
        if (offs == numPairs)
          break;
      }
    }
  }

  cur = 0;

    #ifdef SHOW_STAT2
    /* if (position >= 0) */
    {
      unsigned i;
      printf("\n pos = %4X", position);
      for (i = cur; i <= lenEnd; i++)
      printf("\nprice[%4X] = %u", position - cur + i, p->opt[i].price);
    }
    #endif

  }

  for (;;)
  {
    UInt32 numAvail;
    UInt32 numAvailFull, newLen, numPairs, posPrev, state, posState, startLen;
    UInt32 curPrice, curAnd1Price, matchPrice, repMatchPrice;
    Bool nextIsChar;
    Byte curByte, matchByte;
    const Byte *data;
    COptimal *curOpt;
    COptimal *nextOpt;

    cur++;
    if (cur == lenEnd)
      return Backward(p, backRes, cur);

    newLen = ReadMatchDistances(p, &numPairs);
    if (newLen >= p->numFastBytes)
    {
      p->numPairs = numPairs;
      p->longestMatchLength = newLen;
      return Backward(p, backRes, cur);
    }
    position++;
    curOpt = &p->opt[cur];
    posPrev = curOpt->posPrev;
    if (curOpt->prev1IsChar)
    {
      posPrev--;
      if (curOpt->prev2)
      {
        state = p->opt[curOpt->posPrev2].state;
        if (curOpt->backPrev2 < LZMA_NUM_REPS)
          state = kRepNextStates[state];
        else
          state = kMatchNextStates[state];
      }
      else
        state = p->opt[posPrev].state;
      state = kLiteralNextStates[state];
    }
    else
      state = p->opt[posPrev].state;
    if (posPrev == cur - 1)
    {
      if (IsShortRep(curOpt))
        state = kShortRepNextStates[state];
      else
        state = kLiteralNextStates[state];
    }
    else
    {
      UInt32 pos;
      const COptimal *prevOpt;
      if (curOpt->prev1IsChar && curOpt->prev2)
      {
        posPrev = curOpt->posPrev2;
        pos = curOpt->backPrev2;
        state = kRepNextStates[state];
      }
      else
      {
        pos = curOpt->backPrev;
        if (pos < LZMA_NUM_REPS)
          state = kRepNextStates[state];
        else
          state = kMatchNextStates[state];
      }
      prevOpt = &p->opt[posPrev];
      if (pos < LZMA_NUM_REPS)
      {
        UInt32 i;
        reps[0] = prevOpt->backs[pos];
        for (i = 1; i <= pos; i++)
          reps[i] = prevOpt->backs[(size_t)i - 1];
        for (; i < LZMA_NUM_REPS; i++)
          reps[i] = prevOpt->backs[i];
      }
      else
      {
        UInt32 i;
        reps[0] = (pos - LZMA_NUM_REPS);
        for (i = 1; i < LZMA_NUM_REPS; i++)
          reps[i] = prevOpt->backs[(size_t)i - 1];
      }
    }
    curOpt->state = (CState)state;

    curOpt->backs[0] = reps[0];
    curOpt->backs[1] = reps[1];
    curOpt->backs[2] = reps[2];
    curOpt->backs[3] = reps[3];

    curPrice = curOpt->price;
    nextIsChar = False;
    data = p->matchFinder.GetPointerToCurrentPos(p->matchFinderObj) - 1;
    curByte = *data;
    matchByte = *(data - (reps[0] + 1));

    posState = (position & p->pbMask);

    curAnd1Price = curPrice + GET_PRICE_0(p->isMatch[state][posState]);
    {
      const CLzmaProb *probs = LIT_PROBS(position, *(data - 1));
      curAnd1Price +=
        (!IsCharState(state) ?
          LitEnc_GetPriceMatched(probs, curByte, matchByte, p->ProbPrices) :
          LitEnc_GetPrice(probs, curByte, p->ProbPrices));
    }

    nextOpt = &p->opt[(size_t)cur + 1];

    if (curAnd1Price < nextOpt->price)
    {
      nextOpt->price = curAnd1Price;
      nextOpt->posPrev = cur;
      MakeAsChar(nextOpt);
      nextIsChar = True;
    }

    matchPrice = curPrice + GET_PRICE_1(p->isMatch[state][posState]);
    repMatchPrice = matchPrice + GET_PRICE_1(p->isRep[state]);
    
    if (matchByte == curByte && !(nextOpt->posPrev < cur && nextOpt->backPrev == 0))
    {
      UInt32 shortRepPrice = repMatchPrice + GetRepLen1Price(p, state, posState);
      if (shortRepPrice <= nextOpt->price)
      {
        nextOpt->price = shortRepPrice;
        nextOpt->posPrev = cur;
        MakeAsShortRep(nextOpt);
        nextIsChar = True;
      }
    }
    numAvailFull = p->numAvail;
    {
      UInt32 temp = kNumOpts - 1 - cur;
      if (temp < numAvailFull)
        numAvailFull = temp;
    }

    if (numAvailFull < 2)
      continue;
    numAvail = (numAvailFull <= p->numFastBytes ? numAvailFull : p->numFastBytes);

    if (!nextIsChar && matchByte != curByte) /* speed optimization */
    {
      /* try Literal + rep0 */
      UInt32 temp;
      UInt32 lenTest2;
      const Byte *data2 = data - reps[0] - 1;
      UInt32 limit = p->numFastBytes + 1;
      if (limit > numAvailFull)
        limit = numAvailFull;

      for (temp = 1; temp < limit && data[temp] == data2[temp]; temp++);
      lenTest2 = temp - 1;
      if (lenTest2 >= 2)
      {
        UInt32 state2 = kLiteralNextStates[state];
        UInt32 posStateNext = (position + 1) & p->pbMask;
        UInt32 nextRepMatchPrice = curAnd1Price +
            GET_PRICE_1(p->isMatch[state2][posStateNext]) +
            GET_PRICE_1(p->isRep[state2]);
        /* for (; lenTest2 >= 2; lenTest2--) */
        {
          UInt32 curAndLenPrice;
          COptimal *opt;
          UInt32 offset = cur + 1 + lenTest2;
          while (lenEnd < offset)
            p->opt[++lenEnd].price = kInfinityPrice;
          curAndLenPrice = nextRepMatchPrice + GetRepPrice(p, 0, lenTest2, state2, posStateNext);
          opt = &p->opt[offset];
          if (curAndLenPrice < opt->price)
          {
            opt->price = curAndLenPrice;
            opt->posPrev = cur + 1;
            opt->backPrev = 0;
            opt->prev1IsChar = True;
            opt->prev2 = False;
          }
        }
      }
    }
    
    startLen = 2; /* speed optimization */
    {
    UInt32 repIndex;
    for (repIndex = 0; repIndex < LZMA_NUM_REPS; repIndex++)
    {
      UInt32 lenTest;
      UInt32 lenTestTemp;
      UInt32 price;
      const Byte *data2 = data - reps[repIndex] - 1;
      if (data[0] != data2[0] || data[1] != data2[1])
        continue;
      for (lenTest = 2; lenTest < numAvail && data[lenTest] == data2[lenTest]; lenTest++);
      while (lenEnd < cur + lenTest)
        p->opt[++lenEnd].price = kInfinityPrice;
      lenTestTemp = lenTest;
      price = repMatchPrice + GetPureRepPrice(p, repIndex, state, posState);
      do
      {
        UInt32 curAndLenPrice = price + p->repLenEnc.prices[posState][(size_t)lenTest - 2];
        COptimal *opt = &p->opt[cur + lenTest];
        if (curAndLenPrice < opt->price)
        {
          opt->price = curAndLenPrice;
          opt->posPrev = cur;
          opt->backPrev = repIndex;
          opt->prev1IsChar = False;
        }
      }
      while (--lenTest >= 2);
      lenTest = lenTestTemp;
      
      if (repIndex == 0)
        startLen = lenTest + 1;
        
      /* if (_maxMode) */
        {
          UInt32 lenTest2 = lenTest + 1;
          UInt32 limit = lenTest2 + p->numFastBytes;
          if (limit > numAvailFull)
            limit = numAvailFull;
          for (; lenTest2 < limit && data[lenTest2] == data2[lenTest2]; lenTest2++);
          lenTest2 -= lenTest + 1;
          if (lenTest2 >= 2)
          {
            UInt32 nextRepMatchPrice;
            UInt32 state2 = kRepNextStates[state];
            UInt32 posStateNext = (position + lenTest) & p->pbMask;
            UInt32 curAndLenCharPrice =
                price + p->repLenEnc.prices[posState][(size_t)lenTest - 2] +
                GET_PRICE_0(p->isMatch[state2][posStateNext]) +
                LitEnc_GetPriceMatched(LIT_PROBS(position + lenTest, data[(size_t)lenTest - 1]),
                    data[lenTest], data2[lenTest], p->ProbPrices);
            state2 = kLiteralNextStates[state2];
            posStateNext = (position + lenTest + 1) & p->pbMask;
            nextRepMatchPrice = curAndLenCharPrice +
                GET_PRICE_1(p->isMatch[state2][posStateNext]) +
                GET_PRICE_1(p->isRep[state2]);
            
            /* for (; lenTest2 >= 2; lenTest2--) */
            {
              UInt32 curAndLenPrice;
              COptimal *opt;
              UInt32 offset = cur + lenTest + 1 + lenTest2;
              while (lenEnd < offset)
                p->opt[++lenEnd].price = kInfinityPrice;
              curAndLenPrice = nextRepMatchPrice + GetRepPrice(p, 0, lenTest2, state2, posStateNext);
              opt = &p->opt[offset];
              if (curAndLenPrice < opt->price)
              {
                opt->price = curAndLenPrice;
                opt->posPrev = cur + lenTest + 1;
                opt->backPrev = 0;
                opt->prev1IsChar = True;
                opt->prev2 = True;
                opt->posPrev2 = cur;
                opt->backPrev2 = repIndex;
              }
            }
          }
        }
    }
    }
    /* for (UInt32 lenTest = 2; lenTest <= newLen; lenTest++) */
    if (newLen > numAvail)
    {
      newLen = numAvail;
      for (numPairs = 0; newLen > matches[numPairs]; numPairs += 2);
      matches[numPairs] = newLen;
      numPairs += 2;
    }
    if (newLen >= startLen)
    {
      UInt32 normalMatchPrice = matchPrice + GET_PRICE_0(p->isRep[state]);
      UInt32 offs, curBack, posSlot;
      UInt32 lenTest;
      while (lenEnd < cur + newLen)
        p->opt[++lenEnd].price = kInfinityPrice;

      offs = 0;
      while (startLen > matches[offs])
        offs += 2;
      curBack = matches[(size_t)offs + 1];
      GetPosSlot2(curBack, posSlot);
      for (lenTest = /*2*/ startLen; ; lenTest++)
      {
        UInt32 curAndLenPrice = normalMatchPrice + p->lenEnc.prices[posState][(size_t)lenTest - LZMA_MATCH_LEN_MIN];
        {
        UInt32 lenToPosState = GetLenToPosState(lenTest);
        COptimal *opt;
        if (curBack < kNumFullDistances)
          curAndLenPrice += p->distancesPrices[lenToPosState][curBack];
        else
          curAndLenPrice += p->posSlotPrices[lenToPosState][posSlot] + p->alignPrices[curBack & kAlignMask];
        
        opt = &p->opt[cur + lenTest];
        if (curAndLenPrice < opt->price)
        {
          opt->price = curAndLenPrice;
          opt->posPrev = cur;
          opt->backPrev = curBack + LZMA_NUM_REPS;
          opt->prev1IsChar = False;
        }
        }

        if (/*_maxMode && */lenTest == matches[offs])
        {
          /* Try Match + Literal + Rep0 */
          const Byte *data2 = data - curBack - 1;
          UInt32 lenTest2 = lenTest + 1;
          UInt32 limit = lenTest2 + p->numFastBytes;
          if (limit > numAvailFull)
            limit = numAvailFull;
          for (; lenTest2 < limit && data[lenTest2] == data2[lenTest2]; lenTest2++);
          lenTest2 -= lenTest + 1;
          if (lenTest2 >= 2)
          {
            UInt32 nextRepMatchPrice;
            UInt32 state2 = kMatchNextStates[state];
            UInt32 posStateNext = (position + lenTest) & p->pbMask;
            UInt32 curAndLenCharPrice = curAndLenPrice +
                GET_PRICE_0(p->isMatch[state2][posStateNext]) +
                LitEnc_GetPriceMatched(LIT_PROBS(position + lenTest, data[(size_t)lenTest - 1]),
                    data[lenTest], data2[lenTest], p->ProbPrices);
            state2 = kLiteralNextStates[state2];
            posStateNext = (posStateNext + 1) & p->pbMask;
            nextRepMatchPrice = curAndLenCharPrice +
                GET_PRICE_1(p->isMatch[state2][posStateNext]) +
                GET_PRICE_1(p->isRep[state2]);
            
            /* for (; lenTest2 >= 2; lenTest2--) */
            {
              UInt32 offset = cur + lenTest + 1 + lenTest2;
              UInt32 curAndLenPrice2;
              COptimal *opt;
              while (lenEnd < offset)
                p->opt[++lenEnd].price = kInfinityPrice;
              curAndLenPrice2 = nextRepMatchPrice + GetRepPrice(p, 0, lenTest2, state2, posStateNext);
              opt = &p->opt[offset];
              if (curAndLenPrice2 < opt->price)
              {
                opt->price = curAndLenPrice2;
                opt->posPrev = cur + lenTest + 1;
                opt->backPrev = 0;
                opt->prev1IsChar = True;
                opt->prev2 = True;
                opt->posPrev2 = cur;
                opt->backPrev2 = curBack + LZMA_NUM_REPS;
              }
            }
          }
          offs += 2;
          if (offs == numPairs)
            break;
          curBack = matches[(size_t)offs + 1];
          if (curBack >= kNumFullDistances)
            GetPosSlot2(curBack, posSlot);
        }
      }
    }
  }
}

#define ChangePair(smallDist, bigDist) (((bigDist) >> 7) > (smallDist))

static UInt32 GetOptimumFast(CLzmaEnc *p, UInt32 *backRes)
{
  UInt32 numAvail, mainLen, mainDist, numPairs, repIndex, repLen, i;
  const Byte *data;
  const UInt32 *matches;

  if (p->additionalOffset == 0)
    mainLen = ReadMatchDistances(p, &numPairs);
  else
  {
    mainLen = p->longestMatchLength;
    numPairs = p->numPairs;
  }

  numAvail = p->numAvail;
  *backRes = (UInt32)-1;
  if (numAvail < 2)
    return 1;
  if (numAvail > LZMA_MATCH_LEN_MAX)
    numAvail = LZMA_MATCH_LEN_MAX;
  data = p->matchFinder.GetPointerToCurrentPos(p->matchFinderObj) - 1;

  repLen = repIndex = 0;
  for (i = 0; i < LZMA_NUM_REPS; i++)
  {
    UInt32 len;
    const Byte *data2 = data - p->reps[i] - 1;
    if (data[0] != data2[0] || data[1] != data2[1])
      continue;
    for (len = 2; len < numAvail && data[len] == data2[len]; len++);
    if (len >= p->numFastBytes)
    {
      *backRes = i;
      MovePos(p, len - 1);
      return len;
    }
    if (len > repLen)
    {
      repIndex = i;
      repLen = len;
    }
  }

  matches = p->matches;
  if (mainLen >= p->numFastBytes)
  {
    *backRes = matches[(size_t)numPairs - 1] + LZMA_NUM_REPS;
    MovePos(p, mainLen - 1);
    return mainLen;
  }

  mainDist = 0; /* for GCC */
  if (mainLen >= 2)
  {
    mainDist = matches[(size_t)numPairs - 1];
    while (numPairs > 2 && mainLen == matches[(size_t)numPairs - 4] + 1)
    {
      if (!ChangePair(matches[(size_t)numPairs - 3], mainDist))
        break;
      numPairs -= 2;
      mainLen = matches[(size_t)numPairs - 2];
      mainDist = matches[(size_t)numPairs - 1];
    }
    if (mainLen == 2 && mainDist >= 0x80)
      mainLen = 1;
  }

  if (repLen >= 2 && (
        (repLen + 1 >= mainLen) ||
        (repLen + 2 >= mainLen && mainDist >= (1 << 9)) ||
        (repLen + 3 >= mainLen && mainDist >= (1 << 15))))
  {
    *backRes = repIndex;
    MovePos(p, repLen - 1);
    return repLen;
  }
  
  if (mainLen < 2 || numAvail <= 2)
    return 1;

  p->longestMatchLength = ReadMatchDistances(p, &p->numPairs);
  if (p->longestMatchLength >= 2)
  {
    UInt32 newDistance = matches[(size_t)p->numPairs - 1];
    if ((p->longestMatchLength >= mainLen && newDistance < mainDist) ||
        (p->longestMatchLength == mainLen + 1 && !ChangePair(mainDist, newDistance)) ||
        (p->longestMatchLength > mainLen + 1) ||
        (p->longestMatchLength + 1 >= mainLen && mainLen >= 3 && ChangePair(newDistance, mainDist)))
      return 1;
  }
  
  data = p->matchFinder.GetPointerToCurrentPos(p->matchFinderObj) - 1;
  for (i = 0; i < LZMA_NUM_REPS; i++)
  {
    UInt32 len, limit;
    const Byte *data2 = data - p->reps[i] - 1;
    if (data[0] != data2[0] || data[1] != data2[1])
      continue;
    limit = mainLen - 1;
    for (len = 2; len < limit && data[len] == data2[len]; len++);
    if (len >= limit)
      return 1;
  }
  *backRes = mainDist + LZMA_NUM_REPS;
  MovePos(p, mainLen - 2);
  return mainLen;
}

static void WriteEndMarker(CLzmaEnc *p, UInt32 posState)
{
  UInt32 len;
  RangeEnc_EncodeBit(&p->rc, &p->isMatch[p->state][posState], 1);
  RangeEnc_EncodeBit(&p->rc, &p->isRep[p->state], 0);
  p->state = kMatchNextStates[p->state];
  len = LZMA_MATCH_LEN_MIN;
  LenEnc_Encode2(&p->lenEnc, &p->rc, len - LZMA_MATCH_LEN_MIN, posState, !p->fastMode, p->ProbPrices);
  RcTree_Encode(&p->rc, p->posSlotEncoder[GetLenToPosState(len)], kNumPosSlotBits, (1 << kNumPosSlotBits) - 1);
  RangeEnc_EncodeDirectBits(&p->rc, (((UInt32)1 << 30) - 1) >> kNumAlignBits, 30 - kNumAlignBits);
  RcTree_ReverseEncode(&p->rc, p->posAlignEncoder, kNumAlignBits, kAlignMask);
}

static SRes CheckErrors(CLzmaEnc *p)
{
  if (p->result != SZ_OK)
    return p->result;
  if (p->rc.res != SZ_OK)
    p->result = SZ_ERROR_WRITE;
  if (p->matchFinderBase.result != SZ_OK)
    p->result = SZ_ERROR_READ;
  if (p->result != SZ_OK)
    p->finished = True;
  return p->result;
}

static SRes Flush(CLzmaEnc *p, UInt32 nowPos)
{
  /* ReleaseMFStream(); */
  p->finished = True;
  if (p->writeEndMark)
    WriteEndMarker(p, nowPos & p->pbMask);
  RangeEnc_FlushData(&p->rc);
  RangeEnc_FlushStream(&p->rc);
  return CheckErrors(p);
}

static void FillAlignPrices(CLzmaEnc *p)
{
  UInt32 i;
  for (i = 0; i < kAlignTableSize; i++)
    p->alignPrices[i] = RcTree_ReverseGetPrice(p->posAlignEncoder, kNumAlignBits, i, p->ProbPrices);
  p->alignPriceCount = 0;
}

static void FillDistancesPrices(CLzmaEnc *p)
{
  UInt32 tempPrices[kNumFullDistances];
  UInt32 i, lenToPosState;
  for (i = kStartPosModelIndex; i < kNumFullDistances; i++)
  {
    UInt32 posSlot = GetPosSlot1(i);
    UInt32 footerBits = ((posSlot >> 1) - 1);
    UInt32 base = ((2 | (posSlot & 1)) << footerBits);
    tempPrices[i] = RcTree_ReverseGetPrice(p->posEncoders + base - posSlot - 1, footerBits, i - base, p->ProbPrices);
  }

  for (lenToPosState = 0; lenToPosState < kNumLenToPosStates; lenToPosState++)
  {
    UInt32 posSlot;
    const CLzmaProb *encoder = p->posSlotEncoder[lenToPosState];
    UInt32 *posSlotPrices = p->posSlotPrices[lenToPosState];
    for (posSlot = 0; posSlot < p->distTableSize; posSlot++)
      posSlotPrices[posSlot] = RcTree_GetPrice(encoder, kNumPosSlotBits, posSlot, p->ProbPrices);
    for (posSlot = kEndPosModelIndex; posSlot < p->distTableSize; posSlot++)
      posSlotPrices[posSlot] += ((((posSlot >> 1) - 1) - kNumAlignBits) << kNumBitPriceShiftBits);

    {
      UInt32 *distancesPrices = p->distancesPrices[lenToPosState];
      for (i = 0; i < kStartPosModelIndex; i++)
        distancesPrices[i] = posSlotPrices[i];
      for (; i < kNumFullDistances; i++)
        distancesPrices[i] = posSlotPrices[GetPosSlot1(i)] + tempPrices[i];
    }
  }
  p->matchPriceCount = 0;
}

void LzmaEnc_Construct(CLzmaEnc *p)
{
  RangeEnc_Construct(&p->rc);
  MatchFinder_Construct(&p->matchFinderBase);
  
  #ifndef _7ZIP_ST
  MatchFinderMt_Construct(&p->matchFinderMt);
  p->matchFinderMt.MatchFinder = &p->matchFinderBase;
  #endif

  {
    CLzmaEncProps props;
    LzmaEncProps_Init(&props);
    LzmaEnc_SetProps(p, &props);
  }

  #ifndef LZMA_LOG_BSR
  LzmaEnc_FastPosInit(p->g_FastPos);
  #endif

  LzmaEnc_InitPriceTables(p->ProbPrices);
  p->litProbs = NULL;
  p->saveState.litProbs = NULL;
}

CLzmaEncHandle LzmaEnc_Create(ISzAllocPtr alloc)
{
  void *p;
  p = ISzAlloc_Alloc(alloc, sizeof(CLzmaEnc));
  if (p)
    LzmaEnc_Construct((CLzmaEnc *)p);
  return p;
}

void LzmaEnc_FreeLits(CLzmaEnc *p, ISzAllocPtr alloc)
{
  ISzAlloc_Free(alloc, p->litProbs);
  ISzAlloc_Free(alloc, p->saveState.litProbs);
  p->litProbs = NULL;
  p->saveState.litProbs = NULL;
}

void LzmaEnc_Destruct(CLzmaEnc *p, ISzAllocPtr alloc, ISzAllocPtr allocBig)
{
  #ifndef _7ZIP_ST
  MatchFinderMt_Destruct(&p->matchFinderMt, allocBig);
  #endif
  
  MatchFinder_Free(&p->matchFinderBase, allocBig);
  LzmaEnc_FreeLits(p, alloc);
  RangeEnc_Free(&p->rc, alloc);
}

void LzmaEnc_Destroy(CLzmaEncHandle p, ISzAllocPtr alloc, ISzAllocPtr allocBig)
{
  LzmaEnc_Destruct((CLzmaEnc *)p, alloc, allocBig);
  ISzAlloc_Free(alloc, p);
}

static SRes LzmaEnc_CodeOneBlock(CLzmaEnc *p, Bool useLimits, UInt32 maxPackSize, UInt32 maxUnpackSize)
{
  UInt32 nowPos32, startPos32;
  if (p->needInit)
  {
    p->matchFinder.Init(p->matchFinderObj);
    p->needInit = 0;
  }

  if (p->finished)
    return p->result;
  RINOK(CheckErrors(p));

  nowPos32 = (UInt32)p->nowPos64;
  startPos32 = nowPos32;

  if (p->nowPos64 == 0)
  {
    UInt32 numPairs;
    Byte curByte;
    if (p->matchFinder.GetNumAvailableBytes(p->matchFinderObj) == 0)
      return Flush(p, nowPos32);
    ReadMatchDistances(p, &numPairs);
    RangeEnc_EncodeBit(&p->rc, &p->isMatch[p->state][0], 0);
    p->state = kLiteralNextStates[p->state];
    curByte = *(p->matchFinder.GetPointerToCurrentPos(p->matchFinderObj) - p->additionalOffset);
    LitEnc_Encode(&p->rc, p->litProbs, curByte);
    p->additionalOffset--;
    nowPos32++;
  }

  if (p->matchFinder.GetNumAvailableBytes(p->matchFinderObj) != 0)
  for (;;)
  {
    UInt32 pos, len, posState;

    if (p->fastMode)
      len = GetOptimumFast(p, &pos);
    else
      len = GetOptimum(p, nowPos32, &pos);

    #ifdef SHOW_STAT2
    printf("\n pos = %4X,   len = %u   pos = %u", nowPos32, len, pos);
    #endif

    posState = nowPos32 & p->pbMask;
    if (len == 1 && pos == (UInt32)-1)
    {
      Byte curByte;
      CLzmaProb *probs;
      const Byte *data;

      RangeEnc_EncodeBit(&p->rc, &p->isMatch[p->state][posState], 0);
      data = p->matchFinder.GetPointerToCurrentPos(p->matchFinderObj) - p->additionalOffset;
      curByte = *data;
      probs = LIT_PROBS(nowPos32, *(data - 1));
      if (IsCharState(p->state))
        LitEnc_Encode(&p->rc, probs, curByte);
      else
        LitEnc_EncodeMatched(&p->rc, probs, curByte, *(data - p->reps[0] - 1));
      p->state = kLiteralNextStates[p->state];
    }
    else
    {
      RangeEnc_EncodeBit(&p->rc, &p->isMatch[p->state][posState], 1);
      if (pos < LZMA_NUM_REPS)
      {
        RangeEnc_EncodeBit(&p->rc, &p->isRep[p->state], 1);
        if (pos == 0)
        {
          RangeEnc_EncodeBit(&p->rc, &p->isRepG0[p->state], 0);
          RangeEnc_EncodeBit(&p->rc, &p->isRep0Long[p->state][posState], ((len == 1) ? 0 : 1));
        }
        else
        {
          UInt32 distance = p->reps[pos];
          RangeEnc_EncodeBit(&p->rc, &p->isRepG0[p->state], 1);
          if (pos == 1)
            RangeEnc_EncodeBit(&p->rc, &p->isRepG1[p->state], 0);
          else
          {
            RangeEnc_EncodeBit(&p->rc, &p->isRepG1[p->state], 1);
            RangeEnc_EncodeBit(&p->rc, &p->isRepG2[p->state], pos - 2);
            if (pos == 3)
              p->reps[3] = p->reps[2];
            p->reps[2] = p->reps[1];
          }
          p->reps[1] = p->reps[0];
          p->reps[0] = distance;
        }
        if (len == 1)
          p->state = kShortRepNextStates[p->state];
        else
        {
          LenEnc_Encode2(&p->repLenEnc, &p->rc, len - LZMA_MATCH_LEN_MIN, posState, !p->fastMode, p->ProbPrices);
          p->state = kRepNextStates[p->state];
        }
      }
      else
      {
        UInt32 posSlot;
        RangeEnc_EncodeBit(&p->rc, &p->isRep[p->state], 0);
        p->state = kMatchNextStates[p->state];
        LenEnc_Encode2(&p->lenEnc, &p->rc, len - LZMA_MATCH_LEN_MIN, posState, !p->fastMode, p->ProbPrices);
        pos -= LZMA_NUM_REPS;
        GetPosSlot(pos, posSlot);
        RcTree_Encode(&p->rc, p->posSlotEncoder[GetLenToPosState(len)], kNumPosSlotBits, posSlot);
        
        if (posSlot >= kStartPosModelIndex)
        {
          UInt32 footerBits = ((posSlot >> 1) - 1);
          UInt32 base = ((2 | (posSlot & 1)) << footerBits);
          UInt32 posReduced = pos - base;

          if (posSlot < kEndPosModelIndex)
            RcTree_ReverseEncode(&p->rc, p->posEncoders + base - posSlot - 1, footerBits, posReduced);
          else
          {
            RangeEnc_EncodeDirectBits(&p->rc, posReduced >> kNumAlignBits, footerBits - kNumAlignBits);
            RcTree_ReverseEncode(&p->rc, p->posAlignEncoder, kNumAlignBits, posReduced & kAlignMask);
            p->alignPriceCount++;
          }
        }
        p->reps[3] = p->reps[2];
        p->reps[2] = p->reps[1];
        p->reps[1] = p->reps[0];
        p->reps[0] = pos;
        p->matchPriceCount++;
      }
    }
    p->additionalOffset -= len;
    nowPos32 += len;
    if (p->additionalOffset == 0)
    {
      UInt32 processed;
      if (!p->fastMode)
      {
        if (p->matchPriceCount >= (1 << 7))
          FillDistancesPrices(p);
        if (p->alignPriceCount >= kAlignTableSize)
          FillAlignPrices(p);
      }
      if (p->matchFinder.GetNumAvailableBytes(p->matchFinderObj) == 0)
        break;
      processed = nowPos32 - startPos32;
      if (useLimits)
      {
        if (processed + kNumOpts + 300 >= maxUnpackSize ||
            RangeEnc_GetProcessed(&p->rc) + kNumOpts * 2 >= maxPackSize)
          break;
      }
      else if (processed >= (1 << 17))
      {
        p->nowPos64 += nowPos32 - startPos32;
        return CheckErrors(p);
      }
    }
  }
  p->nowPos64 += nowPos32 - startPos32;
  return Flush(p, nowPos32);
}

#define kBigHashDicLimit ((UInt32)1 << 24)

static SRes LzmaEnc_Alloc(CLzmaEnc *p, UInt32 keepWindowSize, ISzAllocPtr alloc, ISzAllocPtr allocBig)
{
  UInt32 beforeSize = kNumOpts;
  if (!RangeEnc_Alloc(&p->rc, alloc))
    return SZ_ERROR_MEM;

  #ifndef _7ZIP_ST
  p->mtMode = (p->multiThread && !p->fastMode && (p->matchFinderBase.btMode != 0));
  #endif

  {
    unsigned lclp = p->lc + p->lp;
    if (!p->litProbs || !p->saveState.litProbs || p->lclp != lclp)
    {
      LzmaEnc_FreeLits(p, alloc);
      p->litProbs = (CLzmaProb *)ISzAlloc_Alloc(alloc, ((UInt32)0x300 << lclp) * sizeof(CLzmaProb));
      p->saveState.litProbs = (CLzmaProb *)ISzAlloc_Alloc(alloc, ((UInt32)0x300 << lclp) * sizeof(CLzmaProb));
      if (!p->litProbs || !p->saveState.litProbs)
      {
        LzmaEnc_FreeLits(p, alloc);
        return SZ_ERROR_MEM;
      }
      p->lclp = lclp;
    }
  }

  p->matchFinderBase.bigHash = (Byte)(p->dictSize > kBigHashDicLimit ? 1 : 0);

  if (beforeSize + p->dictSize < keepWindowSize)
    beforeSize = keepWindowSize - p->dictSize;

  #ifndef _7ZIP_ST
  if (p->mtMode)
  {
    RINOK(MatchFinderMt_Create(&p->matchFinderMt, p->dictSize, beforeSize, p->numFastBytes, LZMA_MATCH_LEN_MAX, allocBig));
    p->matchFinderObj = &p->matchFinderMt;
    p->matchFinderBase.bigHash = (Byte)(
        (p->dictSize > kBigHashDicLimit && p->matchFinderBase.hashMask >= 0xFFFFFF) ? 1 : 0);
    MatchFinderMt_CreateVTable(&p->matchFinderMt, &p->matchFinder);
  }
  else
  #endif
  {
    if (!MatchFinder_Create(&p->matchFinderBase, p->dictSize, beforeSize, p->numFastBytes, LZMA_MATCH_LEN_MAX, allocBig))
      return SZ_ERROR_MEM;
    p->matchFinderObj = &p->matchFinderBase;
    MatchFinder_CreateVTable(&p->matchFinderBase, &p->matchFinder);
  }
  
  return SZ_OK;
}

void LzmaEnc_Init(CLzmaEnc *p)
{
  UInt32 i;
  p->state = 0;
  for (i = 0 ; i < LZMA_NUM_REPS; i++)
    p->reps[i] = 0;

  RangeEnc_Init(&p->rc);


  for (i = 0; i < kNumStates; i++)
  {
    UInt32 j;
    for (j = 0; j < LZMA_NUM_PB_STATES_MAX; j++)
    {
      p->isMatch[i][j] = kProbInitValue;
      p->isRep0Long[i][j] = kProbInitValue;
    }
    p->isRep[i] = kProbInitValue;
    p->isRepG0[i] = kProbInitValue;
    p->isRepG1[i] = kProbInitValue;
    p->isRepG2[i] = kProbInitValue;
  }

  {
    UInt32 num = (UInt32)0x300 << (p->lp + p->lc);
    CLzmaProb *probs = p->litProbs;
    for (i = 0; i < num; i++)
      probs[i] = kProbInitValue;
  }

  {
    for (i = 0; i < kNumLenToPosStates; i++)
    {
      CLzmaProb *probs = p->posSlotEncoder[i];
      UInt32 j;
      for (j = 0; j < (1 << kNumPosSlotBits); j++)
        probs[j] = kProbInitValue;
    }
  }
  {
    for (i = 0; i < kNumFullDistances - kEndPosModelIndex; i++)
      p->posEncoders[i] = kProbInitValue;
  }

  LenEnc_Init(&p->lenEnc.p);
  LenEnc_Init(&p->repLenEnc.p);

  for (i = 0; i < (1 << kNumAlignBits); i++)
    p->posAlignEncoder[i] = kProbInitValue;

  p->optimumEndIndex = 0;
  p->optimumCurrentIndex = 0;
  p->additionalOffset = 0;

  p->pbMask = (1 << p->pb) - 1;
  p->lpMask = (1 << p->lp) - 1;
}

void LzmaEnc_InitPrices(CLzmaEnc *p)
{
  if (!p->fastMode)
  {
    FillDistancesPrices(p);
    FillAlignPrices(p);
  }

  p->lenEnc.tableSize =
  p->repLenEnc.tableSize =
      p->numFastBytes + 1 - LZMA_MATCH_LEN_MIN;
  LenPriceEnc_UpdateTables(&p->lenEnc, 1 << p->pb, p->ProbPrices);
  LenPriceEnc_UpdateTables(&p->repLenEnc, 1 << p->pb, p->ProbPrices);
}

static SRes LzmaEnc_AllocAndInit(CLzmaEnc *p, UInt32 keepWindowSize, ISzAllocPtr alloc, ISzAllocPtr allocBig)
{
  UInt32 i;
  for (i = 0; i < (UInt32)kDicLogSizeMaxCompress; i++)
    if (p->dictSize <= ((UInt32)1 << i))
      break;
  p->distTableSize = i * 2;

  p->finished = False;
  p->result = SZ_OK;
  RINOK(LzmaEnc_Alloc(p, keepWindowSize, alloc, allocBig));
  LzmaEnc_Init(p);
  LzmaEnc_InitPrices(p);
  p->nowPos64 = 0;
  return SZ_OK;
}

static SRes LzmaEnc_Prepare(CLzmaEncHandle pp, ISeqOutStream *outStream, ISeqInStream *inStream,
    ISzAllocPtr alloc, ISzAllocPtr allocBig)
{
  CLzmaEnc *p = (CLzmaEnc *)pp;
  p->matchFinderBase.stream = inStream;
  p->needInit = 1;
  p->rc.outStream = outStream;
  return LzmaEnc_AllocAndInit(p, 0, alloc, allocBig);
}

SRes LzmaEnc_PrepareForLzma2(CLzmaEncHandle pp,
    ISeqInStream *inStream, UInt32 keepWindowSize,
    ISzAllocPtr alloc, ISzAllocPtr allocBig)
{
  CLzmaEnc *p = (CLzmaEnc *)pp;
  p->matchFinderBase.stream = inStream;
  p->needInit = 1;
  return LzmaEnc_AllocAndInit(p, keepWindowSize, alloc, allocBig);
}

static void LzmaEnc_SetInputBuf(CLzmaEnc *p, const Byte *src, SizeT srcLen)
{
  p->matchFinderBase.directInput = 1;
  p->matchFinderBase.bufferBase = (Byte *)src;
  p->matchFinderBase.directInputRem = srcLen;
}

SRes LzmaEnc_MemPrepare(CLzmaEncHandle pp, const Byte *src, SizeT srcLen,
    UInt32 keepWindowSize, ISzAllocPtr alloc, ISzAllocPtr allocBig)
{
  CLzmaEnc *p = (CLzmaEnc *)pp;
  LzmaEnc_SetInputBuf(p, src, srcLen);
  p->needInit = 1;

  LzmaEnc_SetDataSize(pp, srcLen);
  return LzmaEnc_AllocAndInit(p, keepWindowSize, alloc, allocBig);
}

void LzmaEnc_Finish(CLzmaEncHandle pp)
{
  #ifndef _7ZIP_ST
  CLzmaEnc *p = (CLzmaEnc *)pp;
  if (p->mtMode)
    MatchFinderMt_ReleaseStream(&p->matchFinderMt);
  #else
  UNUSED_VAR(pp);
  #endif
}


typedef struct
{
  ISeqOutStream vt;
  Byte *data;
  SizeT rem;
  Bool overflow;
} CLzmaEnc_SeqOutStreamBuf;

static size_t SeqOutStreamBuf_Write(const ISeqOutStream *pp, const void *data, size_t size)
{
  CLzmaEnc_SeqOutStreamBuf *p = CONTAINER_FROM_VTBL(pp, CLzmaEnc_SeqOutStreamBuf, vt);
  if (p->rem < size)
  {
    size = p->rem;
    p->overflow = True;
  }
  memcpy(p->data, data, size);
  p->rem -= size;
  p->data += size;
  return size;
}


UInt32 LzmaEnc_GetNumAvailableBytes(CLzmaEncHandle pp)
{
  const CLzmaEnc *p = (CLzmaEnc *)pp;
  return p->matchFinder.GetNumAvailableBytes(p->matchFinderObj);
}


const Byte *LzmaEnc_GetCurBuf(CLzmaEncHandle pp)
{
  const CLzmaEnc *p = (CLzmaEnc *)pp;
  return p->matchFinder.GetPointerToCurrentPos(p->matchFinderObj) - p->additionalOffset;
}


SRes LzmaEnc_CodeOneMemBlock(CLzmaEncHandle pp, Bool reInit,
    Byte *dest, size_t *destLen, UInt32 desiredPackSize, UInt32 *unpackSize)
{
  CLzmaEnc *p = (CLzmaEnc *)pp;
  UInt64 nowPos64;
  SRes res;
  CLzmaEnc_SeqOutStreamBuf outStream;

  outStream.vt.Write = SeqOutStreamBuf_Write;
  outStream.data = dest;
  outStream.rem = *destLen;
  outStream.overflow = False;

  p->writeEndMark = False;
  p->finished = False;
  p->result = SZ_OK;

  if (reInit)
    LzmaEnc_Init(p);
  LzmaEnc_InitPrices(p);
  nowPos64 = p->nowPos64;
  RangeEnc_Init(&p->rc);
  p->rc.outStream = &outStream.vt;

  res = LzmaEnc_CodeOneBlock(p, True, desiredPackSize, *unpackSize);
  
  *unpackSize = (UInt32)(p->nowPos64 - nowPos64);
  *destLen -= outStream.rem;
  if (outStream.overflow)
    return SZ_ERROR_OUTPUT_EOF;

  return res;
}


static SRes LzmaEnc_Encode2(CLzmaEnc *p, ICompressProgress *progress)
{
  SRes res = SZ_OK;

  #ifndef _7ZIP_ST
  Byte allocaDummy[0x300];
  allocaDummy[0] = 0;
  allocaDummy[1] = allocaDummy[0];
  #endif

  for (;;)
  {
    res = LzmaEnc_CodeOneBlock(p, False, 0, 0);
    if (res != SZ_OK || p->finished)
      break;
    if (progress)
    {
      res = ICompressProgress_Progress(progress, p->nowPos64, RangeEnc_GetProcessed(&p->rc));
      if (res != SZ_OK)
      {
        res = SZ_ERROR_PROGRESS;
        break;
      }
    }
  }
  
  LzmaEnc_Finish(p);

  /*
  if (res == SZ_OK && !Inline_MatchFinder_IsFinishedOK(&p->matchFinderBase))
    res = SZ_ERROR_FAIL;
  }
  */

  return res;
}


SRes LzmaEnc_Encode(CLzmaEncHandle pp, ISeqOutStream *outStream, ISeqInStream *inStream, ICompressProgress *progress,
    ISzAllocPtr alloc, ISzAllocPtr allocBig)
{
  RINOK(LzmaEnc_Prepare(pp, outStream, inStream, alloc, allocBig));
  return LzmaEnc_Encode2((CLzmaEnc *)pp, progress);
}


SRes LzmaEnc_WriteProperties(CLzmaEncHandle pp, Byte *props, SizeT *size)
{
  CLzmaEnc *p = (CLzmaEnc *)pp;
  unsigned i;
  UInt32 dictSize = p->dictSize;
  if (*size < LZMA_PROPS_SIZE)
    return SZ_ERROR_PARAM;
  *size = LZMA_PROPS_SIZE;
  props[0] = (Byte)((p->pb * 5 + p->lp) * 9 + p->lc);

  if (dictSize >= ((UInt32)1 << 22))
  {
    UInt32 kDictMask = ((UInt32)1 << 20) - 1;
    if (dictSize < (UInt32)0xFFFFFFFF - kDictMask)
      dictSize = (dictSize + kDictMask) & ~kDictMask;
  }
  else for (i = 11; i <= 30; i++)
  {
    if (dictSize <= ((UInt32)2 << i)) { dictSize = (2 << i); break; }
    if (dictSize <= ((UInt32)3 << i)) { dictSize = (3 << i); break; }
  }

  for (i = 0; i < 4; i++)
    props[1 + i] = (Byte)(dictSize >> (8 * i));
  return SZ_OK;
}


unsigned LzmaEnc_IsWriteEndMark(CLzmaEncHandle pp)
{
  return ((CLzmaEnc *)pp)->writeEndMark;
}


SRes LzmaEnc_MemEncode(CLzmaEncHandle pp, Byte *dest, SizeT *destLen, const Byte *src, SizeT srcLen,
    int writeEndMark, ICompressProgress *progress, ISzAllocPtr alloc, ISzAllocPtr allocBig)
{
  SRes res;
  CLzmaEnc *p = (CLzmaEnc *)pp;

  CLzmaEnc_SeqOutStreamBuf outStream;

  outStream.vt.Write = SeqOutStreamBuf_Write;
  outStream.data = dest;
  outStream.rem = *destLen;
  outStream.overflow = False;

  p->writeEndMark = writeEndMark;
  p->rc.outStream = &outStream.vt;

  res = LzmaEnc_MemPrepare(pp, src, srcLen, 0, alloc, allocBig);
  
  if (res == SZ_OK)
  {
    res = LzmaEnc_Encode2(p, progress);
    if (res == SZ_OK && p->nowPos64 != srcLen)
      res = SZ_ERROR_FAIL;
  }

  *destLen -= outStream.rem;
  if (outStream.overflow)
    return SZ_ERROR_OUTPUT_EOF;
  return res;
}


SRes LzmaEncode(Byte *dest, SizeT *destLen, const Byte *src, SizeT srcLen,
    const CLzmaEncProps *props, Byte *propsEncoded, SizeT *propsSize, int writeEndMark,
    ICompressProgress *progress, ISzAllocPtr alloc, ISzAllocPtr allocBig)
{
  CLzmaEnc *p = (CLzmaEnc *)LzmaEnc_Create(alloc);
  SRes res;
  if (!p)
    return SZ_ERROR_MEM;

  res = LzmaEnc_SetProps(p, props);
  if (res == SZ_OK)
  {
    res = LzmaEnc_WriteProperties(p, propsEncoded, propsSize);
    if (res == SZ_OK)
      res = LzmaEnc_MemEncode(p, dest, destLen, src, srcLen,
          writeEndMark, progress, alloc, allocBig);
  }

  LzmaEnc_Destroy(p, alloc, allocBig);
  return res;
}
=======
/* LzmaEnc.c -- LZMA Encoder
2018-04-29 : Igor Pavlov : Public domain */

#include "Precomp.h"

#include <string.h>

/* #define SHOW_STAT */
/* #define SHOW_STAT2 */

#if defined(SHOW_STAT) || defined(SHOW_STAT2)
#include <stdio.h>
#endif

#include "LzmaEnc.h"

#include "LzFind.h"
#ifndef _7ZIP_ST
#include "LzFindMt.h"
#endif

#ifdef SHOW_STAT
static unsigned g_STAT_OFFSET = 0;
#endif

#define kLzmaMaxHistorySize ((UInt32)3 << 29)
/* #define kLzmaMaxHistorySize ((UInt32)7 << 29) */

#define kNumTopBits 24
#define kTopValue ((UInt32)1 << kNumTopBits)

#define kNumBitModelTotalBits 11
#define kBitModelTotal (1 << kNumBitModelTotalBits)
#define kNumMoveBits 5
#define kProbInitValue (kBitModelTotal >> 1)

#define kNumMoveReducingBits 4
#define kNumBitPriceShiftBits 4
#define kBitPrice (1 << kNumBitPriceShiftBits)

void LzmaEncProps_Init(CLzmaEncProps *p)
{
  p->level = 5;
  p->dictSize = p->mc = 0;
  p->reduceSize = (UInt64)(Int64)-1;
  p->lc = p->lp = p->pb = p->algo = p->fb = p->btMode = p->numHashBytes = p->numThreads = -1;
  p->writeEndMark = 0;
}

void LzmaEncProps_Normalize(CLzmaEncProps *p)
{
  int level = p->level;
  if (level < 0) level = 5;
  p->level = level;
  
  if (p->dictSize == 0) p->dictSize = (level <= 5 ? (1 << (level * 2 + 14)) : (level <= 7 ? (1 << 25) : (1 << 26)));
  if (p->dictSize > p->reduceSize)
  {
    unsigned i;
    UInt32 reduceSize = (UInt32)p->reduceSize;
    for (i = 11; i <= 30; i++)
    {
      if (reduceSize <= ((UInt32)2 << i)) { p->dictSize = ((UInt32)2 << i); break; }
      if (reduceSize <= ((UInt32)3 << i)) { p->dictSize = ((UInt32)3 << i); break; }
    }
  }

  if (p->lc < 0) p->lc = 3;
  if (p->lp < 0) p->lp = 0;
  if (p->pb < 0) p->pb = 2;

  if (p->algo < 0) p->algo = (level < 5 ? 0 : 1);
  if (p->fb < 0) p->fb = (level < 7 ? 32 : 64);
  if (p->btMode < 0) p->btMode = (p->algo == 0 ? 0 : 1);
  if (p->numHashBytes < 0) p->numHashBytes = 4;
  if (p->mc == 0) p->mc = (16 + (p->fb >> 1)) >> (p->btMode ? 0 : 1);
  
  if (p->numThreads < 0)
    p->numThreads =
      #ifndef _7ZIP_ST
      ((p->btMode && p->algo) ? 2 : 1);
      #else
      1;
      #endif
}

UInt32 LzmaEncProps_GetDictSize(const CLzmaEncProps *props2)
{
  CLzmaEncProps props = *props2;
  LzmaEncProps_Normalize(&props);
  return props.dictSize;
}

#if (_MSC_VER >= 1400)
/* BSR code is fast for some new CPUs */
/* #define LZMA_LOG_BSR */
#endif

#ifdef LZMA_LOG_BSR

#define kDicLogSizeMaxCompress 32

#define BSR2_RET(pos, res) { unsigned long zz; _BitScanReverse(&zz, (pos)); res = (zz + zz) + ((pos >> (zz - 1)) & 1); }

static unsigned GetPosSlot1(UInt32 pos)
{
  unsigned res;
  BSR2_RET(pos, res);
  return res;
}
#define GetPosSlot2(pos, res) { BSR2_RET(pos, res); }
#define GetPosSlot(pos, res) { if (pos < 2) res = pos; else BSR2_RET(pos, res); }

#else

#define kNumLogBits (9 + sizeof(size_t) / 2)
/* #define kNumLogBits (11 + sizeof(size_t) / 8 * 3) */

#define kDicLogSizeMaxCompress ((kNumLogBits - 1) * 2 + 7)

static void LzmaEnc_FastPosInit(Byte *g_FastPos)
{
  unsigned slot;
  g_FastPos[0] = 0;
  g_FastPos[1] = 1;
  g_FastPos += 2;
  
  for (slot = 2; slot < kNumLogBits * 2; slot++)
  {
    size_t k = ((size_t)1 << ((slot >> 1) - 1));
    size_t j;
    for (j = 0; j < k; j++)
      g_FastPos[j] = (Byte)slot;
    g_FastPos += k;
  }
}

/* we can use ((limit - pos) >> 31) only if (pos < ((UInt32)1 << 31)) */
/*
#define BSR2_RET(pos, res) { unsigned zz = 6 + ((kNumLogBits - 1) & \
  (0 - (((((UInt32)1 << (kNumLogBits + 6)) - 1) - pos) >> 31))); \
  res = p->g_FastPos[pos >> zz] + (zz * 2); }
*/

/*
#define BSR2_RET(pos, res) { unsigned zz = 6 + ((kNumLogBits - 1) & \
  (0 - (((((UInt32)1 << (kNumLogBits)) - 1) - (pos >> 6)) >> 31))); \
  res = p->g_FastPos[pos >> zz] + (zz * 2); }
*/

#define BSR2_RET(pos, res) { unsigned zz = (pos < (1 << (kNumLogBits + 6))) ? 6 : 6 + kNumLogBits - 1; \
  res = p->g_FastPos[pos >> zz] + (zz * 2); }

/*
#define BSR2_RET(pos, res) { res = (pos < (1 << (kNumLogBits + 6))) ? \
  p->g_FastPos[pos >> 6] + 12 : \
  p->g_FastPos[pos >> (6 + kNumLogBits - 1)] + (6 + (kNumLogBits - 1)) * 2; }
*/

#define GetPosSlot1(pos) p->g_FastPos[pos]
#define GetPosSlot2(pos, res) { BSR2_RET(pos, res); }
#define GetPosSlot(pos, res) { if (pos < kNumFullDistances) res = p->g_FastPos[pos & (kNumFullDistances - 1)]; else BSR2_RET(pos, res); }

#endif


#define LZMA_NUM_REPS 4

typedef UInt16 CState;
typedef UInt16 CExtra;

typedef struct
{
  UInt32 price;
  CState state;
  CExtra extra;
      // 0   : normal
      // 1   : LIT : MATCH
      // > 1 : MATCH (extra-1) : LIT : REP0 (len)
  UInt32 len;
  UInt32 dist;
  UInt32 reps[LZMA_NUM_REPS];
} COptimal;


#define kNumOpts (1 << 12)
#define kPackReserve (1 + kNumOpts * 2)

#define kNumLenToPosStates 4
#define kNumPosSlotBits 6
#define kDicLogSizeMin 0
#define kDicLogSizeMax 32
#define kDistTableSizeMax (kDicLogSizeMax * 2)

#define kNumAlignBits 4
#define kAlignTableSize (1 << kNumAlignBits)
#define kAlignMask (kAlignTableSize - 1)

#define kStartPosModelIndex 4
#define kEndPosModelIndex 14
#define kNumFullDistances (1 << (kEndPosModelIndex >> 1))

typedef
#ifdef _LZMA_PROB32
  UInt32
#else
  UInt16
#endif
  CLzmaProb;

#define LZMA_PB_MAX 4
#define LZMA_LC_MAX 8
#define LZMA_LP_MAX 4

#define LZMA_NUM_PB_STATES_MAX (1 << LZMA_PB_MAX)

#define kLenNumLowBits 3
#define kLenNumLowSymbols (1 << kLenNumLowBits)
#define kLenNumHighBits 8
#define kLenNumHighSymbols (1 << kLenNumHighBits)
#define kLenNumSymbolsTotal (kLenNumLowSymbols * 2 + kLenNumHighSymbols)

#define LZMA_MATCH_LEN_MIN 2
#define LZMA_MATCH_LEN_MAX (LZMA_MATCH_LEN_MIN + kLenNumSymbolsTotal - 1)

#define kNumStates 12


typedef struct
{
  CLzmaProb low[LZMA_NUM_PB_STATES_MAX << (kLenNumLowBits + 1)];
  CLzmaProb high[kLenNumHighSymbols];
} CLenEnc;


typedef struct
{
  unsigned tableSize;
  unsigned counters[LZMA_NUM_PB_STATES_MAX];
  UInt32 prices[LZMA_NUM_PB_STATES_MAX][kLenNumSymbolsTotal];
} CLenPriceEnc;


typedef struct
{
  UInt32 range;
  unsigned cache;
  UInt64 low;
  UInt64 cacheSize;
  Byte *buf;
  Byte *bufLim;
  Byte *bufBase;
  ISeqOutStream *outStream;
  UInt64 processed;
  SRes res;
} CRangeEnc;


typedef struct
{
  CLzmaProb *litProbs;

  unsigned state;
  UInt32 reps[LZMA_NUM_REPS];

  CLzmaProb posAlignEncoder[1 << kNumAlignBits];
  CLzmaProb isRep[kNumStates];
  CLzmaProb isRepG0[kNumStates];
  CLzmaProb isRepG1[kNumStates];
  CLzmaProb isRepG2[kNumStates];
  CLzmaProb isMatch[kNumStates][LZMA_NUM_PB_STATES_MAX];
  CLzmaProb isRep0Long[kNumStates][LZMA_NUM_PB_STATES_MAX];

  CLzmaProb posSlotEncoder[kNumLenToPosStates][1 << kNumPosSlotBits];
  CLzmaProb posEncoders[kNumFullDistances];
  
  CLenEnc lenProbs;
  CLenEnc repLenProbs;

} CSaveState;


typedef UInt32 CProbPrice;


typedef struct
{
  void *matchFinderObj;
  IMatchFinder matchFinder;

  unsigned optCur;
  unsigned optEnd;

  unsigned longestMatchLen;
  unsigned numPairs;
  UInt32 numAvail;

  unsigned state;
  unsigned numFastBytes;
  unsigned additionalOffset;
  UInt32 reps[LZMA_NUM_REPS];
  unsigned lpMask, pbMask;
  CLzmaProb *litProbs;
  CRangeEnc rc;

  UInt32 backRes;

  unsigned lc, lp, pb;
  unsigned lclp;

  Bool fastMode;
  Bool writeEndMark;
  Bool finished;
  Bool multiThread;
  Bool needInit;

  UInt64 nowPos64;
  
  unsigned matchPriceCount;
  unsigned alignPriceCount;

  unsigned distTableSize;

  UInt32 dictSize;
  SRes result;

  #ifndef _7ZIP_ST
  Bool mtMode;
  // begin of CMatchFinderMt is used in LZ thread
  CMatchFinderMt matchFinderMt;
  // end of CMatchFinderMt is used in BT and HASH threads
  #endif

  CMatchFinder matchFinderBase;

  #ifndef _7ZIP_ST
  Byte pad[128];
  #endif
  
  // LZ thread
  CProbPrice ProbPrices[kBitModelTotal >> kNumMoveReducingBits];

  UInt32 matches[LZMA_MATCH_LEN_MAX * 2 + 2 + 1];

  UInt32 alignPrices[kAlignTableSize];
  UInt32 posSlotPrices[kNumLenToPosStates][kDistTableSizeMax];
  UInt32 distancesPrices[kNumLenToPosStates][kNumFullDistances];

  CLzmaProb posAlignEncoder[1 << kNumAlignBits];
  CLzmaProb isRep[kNumStates];
  CLzmaProb isRepG0[kNumStates];
  CLzmaProb isRepG1[kNumStates];
  CLzmaProb isRepG2[kNumStates];
  CLzmaProb isMatch[kNumStates][LZMA_NUM_PB_STATES_MAX];
  CLzmaProb isRep0Long[kNumStates][LZMA_NUM_PB_STATES_MAX];
  CLzmaProb posSlotEncoder[kNumLenToPosStates][1 << kNumPosSlotBits];
  CLzmaProb posEncoders[kNumFullDistances];
  
  CLenEnc lenProbs;
  CLenEnc repLenProbs;

  #ifndef LZMA_LOG_BSR
  Byte g_FastPos[1 << kNumLogBits];
  #endif

  CLenPriceEnc lenEnc;
  CLenPriceEnc repLenEnc;

  COptimal opt[kNumOpts];

  CSaveState saveState;

  #ifndef _7ZIP_ST
  Byte pad2[128];
  #endif
} CLzmaEnc;



#define COPY_ARR(dest, src, arr) memcpy(dest->arr, src->arr, sizeof(src->arr));

void LzmaEnc_SaveState(CLzmaEncHandle pp)
{
  CLzmaEnc *p = (CLzmaEnc *)pp;
  CSaveState *dest = &p->saveState;
  
  dest->state = p->state;
  
  dest->lenProbs = p->lenProbs;
  dest->repLenProbs = p->repLenProbs;

  COPY_ARR(dest, p, reps);

  COPY_ARR(dest, p, posAlignEncoder);
  COPY_ARR(dest, p, isRep);
  COPY_ARR(dest, p, isRepG0);
  COPY_ARR(dest, p, isRepG1);
  COPY_ARR(dest, p, isRepG2);
  COPY_ARR(dest, p, isMatch);
  COPY_ARR(dest, p, isRep0Long);
  COPY_ARR(dest, p, posSlotEncoder);
  COPY_ARR(dest, p, posEncoders);

  memcpy(dest->litProbs, p->litProbs, ((UInt32)0x300 << p->lclp) * sizeof(CLzmaProb));
}


void LzmaEnc_RestoreState(CLzmaEncHandle pp)
{
  CLzmaEnc *dest = (CLzmaEnc *)pp;
  const CSaveState *p = &dest->saveState;

  dest->state = p->state;

  dest->lenProbs = p->lenProbs;
  dest->repLenProbs = p->repLenProbs;
  
  COPY_ARR(dest, p, reps);
  
  COPY_ARR(dest, p, posAlignEncoder);
  COPY_ARR(dest, p, isRep);
  COPY_ARR(dest, p, isRepG0);
  COPY_ARR(dest, p, isRepG1);
  COPY_ARR(dest, p, isRepG2);
  COPY_ARR(dest, p, isMatch);
  COPY_ARR(dest, p, isRep0Long);
  COPY_ARR(dest, p, posSlotEncoder);
  COPY_ARR(dest, p, posEncoders);

  memcpy(dest->litProbs, p->litProbs, ((UInt32)0x300 << dest->lclp) * sizeof(CLzmaProb));
}



SRes LzmaEnc_SetProps(CLzmaEncHandle pp, const CLzmaEncProps *props2)
{
  CLzmaEnc *p = (CLzmaEnc *)pp;
  CLzmaEncProps props = *props2;
  LzmaEncProps_Normalize(&props);

  if (props.lc > LZMA_LC_MAX
      || props.lp > LZMA_LP_MAX
      || props.pb > LZMA_PB_MAX
      || props.dictSize > ((UInt64)1 << kDicLogSizeMaxCompress)
      || props.dictSize > kLzmaMaxHistorySize)
    return SZ_ERROR_PARAM;

  p->dictSize = props.dictSize;
  {
    unsigned fb = props.fb;
    if (fb < 5)
      fb = 5;
    if (fb > LZMA_MATCH_LEN_MAX)
      fb = LZMA_MATCH_LEN_MAX;
    p->numFastBytes = fb;
  }
  p->lc = props.lc;
  p->lp = props.lp;
  p->pb = props.pb;
  p->fastMode = (props.algo == 0);
  p->matchFinderBase.btMode = (Byte)(props.btMode ? 1 : 0);
  {
    unsigned numHashBytes = 4;
    if (props.btMode)
    {
      if (props.numHashBytes < 2)
        numHashBytes = 2;
      else if (props.numHashBytes < 4)
        numHashBytes = props.numHashBytes;
    }
    p->matchFinderBase.numHashBytes = numHashBytes;
  }

  p->matchFinderBase.cutValue = props.mc;

  p->writeEndMark = props.writeEndMark;

  #ifndef _7ZIP_ST
  /*
  if (newMultiThread != _multiThread)
  {
    ReleaseMatchFinder();
    _multiThread = newMultiThread;
  }
  */
  p->multiThread = (props.numThreads > 1);
  #endif

  return SZ_OK;
}


void LzmaEnc_SetDataSize(CLzmaEncHandle pp, UInt64 expectedDataSiize)
{
  CLzmaEnc *p = (CLzmaEnc *)pp;
  p->matchFinderBase.expectedDataSize = expectedDataSiize;
}


#define kState_Start 0
#define kState_LitAfterMatch 4
#define kState_LitAfterRep   5
#define kState_MatchAfterLit 7
#define kState_RepAfterLit   8

static const Byte kLiteralNextStates[kNumStates] = {0, 0, 0, 0, 1, 2, 3, 4,  5,  6,   4, 5};
static const Byte kMatchNextStates[kNumStates]   = {7, 7, 7, 7, 7, 7, 7, 10, 10, 10, 10, 10};
static const Byte kRepNextStates[kNumStates]     = {8, 8, 8, 8, 8, 8, 8, 11, 11, 11, 11, 11};
static const Byte kShortRepNextStates[kNumStates]= {9, 9, 9, 9, 9, 9, 9, 11, 11, 11, 11, 11};

#define IsLitState(s) ((s) < 7)
#define GetLenToPosState2(len) (((len) < kNumLenToPosStates - 1) ? (len) : kNumLenToPosStates - 1)
#define GetLenToPosState(len) (((len) < kNumLenToPosStates + 1) ? (len) - 2 : kNumLenToPosStates - 1)

#define kInfinityPrice (1 << 30)

static void RangeEnc_Construct(CRangeEnc *p)
{
  p->outStream = NULL;
  p->bufBase = NULL;
}

#define RangeEnc_GetProcessed(p)       ((p)->processed + ((p)->buf - (p)->bufBase) + (p)->cacheSize)
#define RangeEnc_GetProcessed_sizet(p) ((size_t)(p)->processed + ((p)->buf - (p)->bufBase) + (size_t)(p)->cacheSize)

#define RC_BUF_SIZE (1 << 16)

static int RangeEnc_Alloc(CRangeEnc *p, ISzAllocPtr alloc)
{
  if (!p->bufBase)
  {
    p->bufBase = (Byte *)ISzAlloc_Alloc(alloc, RC_BUF_SIZE);
    if (!p->bufBase)
      return 0;
    p->bufLim = p->bufBase + RC_BUF_SIZE;
  }
  return 1;
}

static void RangeEnc_Free(CRangeEnc *p, ISzAllocPtr alloc)
{
  ISzAlloc_Free(alloc, p->bufBase);
  p->bufBase = 0;
}

static void RangeEnc_Init(CRangeEnc *p)
{
  /* Stream.Init(); */
  p->range = 0xFFFFFFFF;
  p->cache = 0;
  p->low = 0;
  p->cacheSize = 0;

  p->buf = p->bufBase;

  p->processed = 0;
  p->res = SZ_OK;
}

MY_NO_INLINE static void RangeEnc_FlushStream(CRangeEnc *p)
{
  size_t num;
  if (p->res != SZ_OK)
    return;
  num = p->buf - p->bufBase;
  if (num != ISeqOutStream_Write(p->outStream, p->bufBase, num))
    p->res = SZ_ERROR_WRITE;
  p->processed += num;
  p->buf = p->bufBase;
}

MY_NO_INLINE static void MY_FAST_CALL RangeEnc_ShiftLow(CRangeEnc *p)
{
  UInt32 low = (UInt32)p->low;
  unsigned high = (unsigned)(p->low >> 32);
  p->low = (UInt32)(low << 8);
  if (low < (UInt32)0xFF000000 || high != 0)
  {
    {
      Byte *buf = p->buf;
      *buf++ = (Byte)(p->cache + high);
      p->cache = (unsigned)(low >> 24);
      p->buf = buf;
      if (buf == p->bufLim)
        RangeEnc_FlushStream(p);
      if (p->cacheSize == 0)
        return;
    }
    high += 0xFF;
    for (;;)
    {
      Byte *buf = p->buf;
      *buf++ = (Byte)(high);
      p->buf = buf;
      if (buf == p->bufLim)
        RangeEnc_FlushStream(p);
      if (--p->cacheSize == 0)
        return;
    }
  }
  p->cacheSize++;
}

static void RangeEnc_FlushData(CRangeEnc *p)
{
  int i;
  for (i = 0; i < 5; i++)
    RangeEnc_ShiftLow(p);
}

#define RC_NORM(p) if (range < kTopValue) { range <<= 8; RangeEnc_ShiftLow(p); }

#define RC_BIT_PRE(p, prob) \
  ttt = *(prob); \
  newBound = (range >> kNumBitModelTotalBits) * ttt;

// #define _LZMA_ENC_USE_BRANCH

#ifdef _LZMA_ENC_USE_BRANCH

#define RC_BIT(p, prob, symbol) { \
  RC_BIT_PRE(p, prob) \
  if (symbol == 0) { range = newBound; ttt += (kBitModelTotal - ttt) >> kNumMoveBits; } \
  else { (p)->low += newBound; range -= newBound; ttt -= ttt >> kNumMoveBits; } \
  *(prob) = (CLzmaProb)ttt; \
  RC_NORM(p) \
  }

#else

#define RC_BIT(p, prob, symbol) { \
  UInt32 mask; \
  RC_BIT_PRE(p, prob) \
  mask = 0 - (UInt32)symbol; \
  range &= mask; \
  mask &= newBound; \
  range -= mask; \
  (p)->low += mask; \
  mask = (UInt32)symbol - 1; \
  range += newBound & mask; \
  mask &= (kBitModelTotal - ((1 << kNumMoveBits) - 1)); \
  mask += ((1 << kNumMoveBits) - 1); \
  ttt += (Int32)(mask - ttt) >> kNumMoveBits; \
  *(prob) = (CLzmaProb)ttt; \
  RC_NORM(p) \
  }

#endif




#define RC_BIT_0_BASE(p, prob) \
  range = newBound; *(prob) = (CLzmaProb)(ttt + ((kBitModelTotal - ttt) >> kNumMoveBits));

#define RC_BIT_1_BASE(p, prob) \
  range -= newBound; (p)->low += newBound; *(prob) = (CLzmaProb)(ttt - (ttt >> kNumMoveBits)); \

#define RC_BIT_0(p, prob) \
  RC_BIT_0_BASE(p, prob) \
  RC_NORM(p)

#define RC_BIT_1(p, prob) \
  RC_BIT_1_BASE(p, prob) \
  RC_NORM(p)

static void RangeEnc_EncodeBit_0(CRangeEnc *p, CLzmaProb *prob)
{
  UInt32 range, ttt, newBound;
  range = p->range;
  RC_BIT_PRE(p, prob)
  RC_BIT_0(p, prob)
  p->range = range;
}

static void LitEnc_Encode(CRangeEnc *p, CLzmaProb *probs, UInt32 symbol)
{
  UInt32 range = p->range;
  symbol |= 0x100;
  do
  {
    UInt32 ttt, newBound;
    // RangeEnc_EncodeBit(p, probs + (symbol >> 8), (symbol >> 7) & 1);
    CLzmaProb *prob = probs + (symbol >> 8);
    UInt32 bit = (symbol >> 7) & 1;
    symbol <<= 1;
    RC_BIT(p, prob, bit);
  }
  while (symbol < 0x10000);
  p->range = range;
}

static void LitEnc_EncodeMatched(CRangeEnc *p, CLzmaProb *probs, UInt32 symbol, UInt32 matchByte)
{
  UInt32 range = p->range;
  UInt32 offs = 0x100;
  symbol |= 0x100;
  do
  {
    UInt32 ttt, newBound;
    CLzmaProb *prob;
    UInt32 bit;
    matchByte <<= 1;
    // RangeEnc_EncodeBit(p, probs + (offs + (matchByte & offs) + (symbol >> 8)), (symbol >> 7) & 1);
    prob = probs + (offs + (matchByte & offs) + (symbol >> 8));
    bit = (symbol >> 7) & 1;
    symbol <<= 1;
    offs &= ~(matchByte ^ symbol);
    RC_BIT(p, prob, bit);
  }
  while (symbol < 0x10000);
  p->range = range;
}



static void LzmaEnc_InitPriceTables(CProbPrice *ProbPrices)
{
  UInt32 i;
  for (i = 0; i < (kBitModelTotal >> kNumMoveReducingBits); i++)
  {
    const unsigned kCyclesBits = kNumBitPriceShiftBits;
    UInt32 w = (i << kNumMoveReducingBits) + (1 << (kNumMoveReducingBits - 1));
    unsigned bitCount = 0;
    unsigned j;
    for (j = 0; j < kCyclesBits; j++)
    {
      w = w * w;
      bitCount <<= 1;
      while (w >= ((UInt32)1 << 16))
      {
        w >>= 1;
        bitCount++;
      }
    }
    ProbPrices[i] = (CProbPrice)((kNumBitModelTotalBits << kCyclesBits) - 15 - bitCount);
    // printf("\n%3d: %5d", i, ProbPrices[i]);
  }
}


#define GET_PRICE(prob, symbol) \
  p->ProbPrices[((prob) ^ (unsigned)(((-(int)(symbol))) & (kBitModelTotal - 1))) >> kNumMoveReducingBits];

#define GET_PRICEa(prob, symbol) \
     ProbPrices[((prob) ^ (unsigned)((-((int)(symbol))) & (kBitModelTotal - 1))) >> kNumMoveReducingBits];

#define GET_PRICE_0(prob) p->ProbPrices[(prob) >> kNumMoveReducingBits]
#define GET_PRICE_1(prob) p->ProbPrices[((prob) ^ (kBitModelTotal - 1)) >> kNumMoveReducingBits]

#define GET_PRICEa_0(prob) ProbPrices[(prob) >> kNumMoveReducingBits]
#define GET_PRICEa_1(prob) ProbPrices[((prob) ^ (kBitModelTotal - 1)) >> kNumMoveReducingBits]


static UInt32 LitEnc_GetPrice(const CLzmaProb *probs, UInt32 symbol, const CProbPrice *ProbPrices)
{
  UInt32 price = 0;
  symbol |= 0x100;
  do
  {
    unsigned bit = symbol & 1;
    symbol >>= 1;
    price += GET_PRICEa(probs[symbol], bit);
  }
  while (symbol >= 2);
  return price;
}


static UInt32 LitEnc_Matched_GetPrice(const CLzmaProb *probs, UInt32 symbol, UInt32 matchByte, const CProbPrice *ProbPrices)
{
  UInt32 price = 0;
  UInt32 offs = 0x100;
  symbol |= 0x100;
  do
  {
    matchByte <<= 1;
    price += GET_PRICEa(probs[offs + (matchByte & offs) + (symbol >> 8)], (symbol >> 7) & 1);
    symbol <<= 1;
    offs &= ~(matchByte ^ symbol);
  }
  while (symbol < 0x10000);
  return price;
}


static void RcTree_ReverseEncode(CRangeEnc *rc, CLzmaProb *probs, unsigned numBits, UInt32 symbol)
{
  UInt32 range = rc->range;
  unsigned m = 1;
  do
  {
    UInt32 ttt, newBound;
    unsigned bit = symbol & 1;
    // RangeEnc_EncodeBit(rc, probs + m, bit);
    symbol >>= 1;
    RC_BIT(rc, probs + m, bit);
    m = (m << 1) | bit;
  }
  while (--numBits);
  rc->range = range;
}



static void LenEnc_Init(CLenEnc *p)
{
  unsigned i;
  for (i = 0; i < (LZMA_NUM_PB_STATES_MAX << (kLenNumLowBits + 1)); i++)
    p->low[i] = kProbInitValue;
  for (i = 0; i < kLenNumHighSymbols; i++)
    p->high[i] = kProbInitValue;
}

static void LenEnc_Encode(CLenEnc *p, CRangeEnc *rc, unsigned symbol, unsigned posState)
{
  UInt32 range, ttt, newBound;
  CLzmaProb *probs = p->low;
  range = rc->range;
  RC_BIT_PRE(rc, probs);
  if (symbol >= kLenNumLowSymbols)
  {
    RC_BIT_1(rc, probs);
    probs += kLenNumLowSymbols;
    RC_BIT_PRE(rc, probs);
    if (symbol >= kLenNumLowSymbols * 2)
    {
      RC_BIT_1(rc, probs);
      rc->range = range;
      // RcTree_Encode(rc, p->high, kLenNumHighBits, symbol - kLenNumLowSymbols * 2);
      LitEnc_Encode(rc, p->high, symbol - kLenNumLowSymbols * 2);
      return;
    }
    symbol -= kLenNumLowSymbols;
  }

  // RcTree_Encode(rc, probs + (posState << kLenNumLowBits), kLenNumLowBits, symbol);
  {
    unsigned m;
    unsigned bit;
    RC_BIT_0(rc, probs);
    probs += (posState << (1 + kLenNumLowBits));
    bit = (symbol >> 2)    ; RC_BIT(rc, probs + 1, bit); m = (1 << 1) + bit;
    bit = (symbol >> 1) & 1; RC_BIT(rc, probs + m, bit); m = (m << 1) + bit;
    bit =  symbol       & 1; RC_BIT(rc, probs + m, bit);
    rc->range = range;
  }
}

static void SetPrices_3(const CLzmaProb *probs, UInt32 startPrice, UInt32 *prices, const CProbPrice *ProbPrices)
{
  unsigned i;
  for (i = 0; i < 8; i += 2)
  {
    UInt32 price = startPrice;
    UInt32 prob;
    price += GET_PRICEa(probs[1           ], (i >> 2));
    price += GET_PRICEa(probs[2 + (i >> 2)], (i >> 1) & 1);
    prob = probs[4 + (i >> 1)];
    prices[i    ] = price + GET_PRICEa_0(prob);
    prices[i + 1] = price + GET_PRICEa_1(prob);
  }
}


MY_NO_INLINE static void MY_FAST_CALL LenPriceEnc_UpdateTable(
    CLenPriceEnc *p, unsigned posState,
    const CLenEnc *enc,
    const CProbPrice *ProbPrices)
{
  // int y; for (y = 0; y < 100; y++) {
  UInt32 a;
  unsigned i, numSymbols;

  UInt32 *prices = p->prices[posState];
  {
    const CLzmaProb *probs = enc->low + (posState << (1 + kLenNumLowBits));
    SetPrices_3(probs, GET_PRICEa_0(enc->low[0]), prices, ProbPrices);
    a = GET_PRICEa_1(enc->low[0]);
    SetPrices_3(probs + kLenNumLowSymbols, a + GET_PRICEa_0(enc->low[kLenNumLowSymbols]), prices + kLenNumLowSymbols, ProbPrices);
    a += GET_PRICEa_1(enc->low[kLenNumLowSymbols]);
  }
  numSymbols = p->tableSize;
  p->counters[posState] = numSymbols;
  for (i = kLenNumLowSymbols * 2; i < numSymbols; i += 1)
  {
    prices[i] = a +
       // RcTree_GetPrice(enc->high, kLenNumHighBits, i - kLenNumLowSymbols * 2, ProbPrices);
       LitEnc_GetPrice(enc->high, i - kLenNumLowSymbols * 2, ProbPrices);
    /*
    unsigned sym = (i - kLenNumLowSymbols * 2) >> 1;
    UInt32 price = a + RcTree_GetPrice(enc->high, kLenNumHighBits - 1, sym, ProbPrices);
    UInt32 prob = enc->high[(1 << 7) + sym];
    prices[i    ] = price + GET_PRICEa_0(prob);
    prices[i + 1] = price + GET_PRICEa_1(prob);
    */
  }
  // }
}

static void LenPriceEnc_UpdateTables(CLenPriceEnc *p, unsigned numPosStates,
    const CLenEnc *enc,
    const CProbPrice *ProbPrices)
{
  unsigned posState;
  for (posState = 0; posState < numPosStates; posState++)
    LenPriceEnc_UpdateTable(p, posState, enc, ProbPrices);
}


/*
  #ifdef SHOW_STAT
  g_STAT_OFFSET += num;
  printf("\n MovePos %u", num);
  #endif
*/
  
#define MOVE_POS(p, num) { \
    p->additionalOffset += (num); \
    p->matchFinder.Skip(p->matchFinderObj, (num)); }


static unsigned ReadMatchDistances(CLzmaEnc *p, unsigned *numPairsRes)
{
  unsigned numPairs;
  
  p->additionalOffset++;
  p->numAvail = p->matchFinder.GetNumAvailableBytes(p->matchFinderObj);
  numPairs = p->matchFinder.GetMatches(p->matchFinderObj, p->matches);
  *numPairsRes = numPairs;
  
  #ifdef SHOW_STAT
  printf("\n i = %u numPairs = %u    ", g_STAT_OFFSET, numPairs / 2);
  g_STAT_OFFSET++;
  {
    unsigned i;
    for (i = 0; i < numPairs; i += 2)
      printf("%2u %6u   | ", p->matches[i], p->matches[i + 1]);
  }
  #endif
  
  if (numPairs == 0)
    return 0;
  {
    unsigned len = p->matches[(size_t)numPairs - 2];
    if (len != p->numFastBytes)
      return len;
    {
      UInt32 numAvail = p->numAvail;
      if (numAvail > LZMA_MATCH_LEN_MAX)
        numAvail = LZMA_MATCH_LEN_MAX;
      {
        const Byte *p1 = p->matchFinder.GetPointerToCurrentPos(p->matchFinderObj) - 1;
        const Byte *p2 = p1 + len;
        ptrdiff_t dif = (ptrdiff_t)-1 - p->matches[(size_t)numPairs - 1];
        const Byte *lim = p1 + numAvail;
        for (; p2 != lim && *p2 == p2[dif]; p2++);
        return (unsigned)(p2 - p1);
      }
    }
  }
}

#define MARK_LIT ((UInt32)(Int32)-1)

#define MakeAs_Lit(p)       { (p)->dist = MARK_LIT; (p)->extra = 0; }
#define MakeAs_ShortRep(p)  { (p)->dist = 0; (p)->extra = 0; }
#define IsShortRep(p)       ((p)->dist == 0)


#define GetPrice_ShortRep(p, state, posState) \
  ( GET_PRICE_0(p->isRepG0[state]) + GET_PRICE_0(p->isRep0Long[state][posState]))

#define GetPrice_Rep_0(p, state, posState) ( \
    GET_PRICE_1(p->isMatch[state][posState]) \
  + GET_PRICE_1(p->isRep0Long[state][posState])) \
  + GET_PRICE_1(p->isRep[state]) \
  + GET_PRICE_0(p->isRepG0[state])
  

static UInt32 GetPrice_PureRep(const CLzmaEnc *p, unsigned repIndex, size_t state, size_t posState)
{
  UInt32 price;
  UInt32 prob = p->isRepG0[state];
  if (repIndex == 0)
  {
    price = GET_PRICE_0(prob);
    price += GET_PRICE_1(p->isRep0Long[state][posState]);
  }
  else
  {
    price = GET_PRICE_1(prob);
    prob = p->isRepG1[state];
    if (repIndex == 1)
      price += GET_PRICE_0(prob);
    else
    {
      price += GET_PRICE_1(prob);
      price += GET_PRICE(p->isRepG2[state], repIndex - 2);
    }
  }
  return price;
}


static unsigned Backward(CLzmaEnc *p, unsigned cur)
{
  unsigned wr = cur + 1;
  p->optEnd = wr;

  for (;;)
  {
    UInt32 dist = p->opt[cur].dist;
    UInt32 len = p->opt[cur].len;
    UInt32 extra = p->opt[cur].extra;
    cur -= len;

    if (extra)
    {
      wr--;
      p->opt[wr].len = len;
      cur -= extra;
      len = extra;
      if (extra == 1)
      {
        p->opt[wr].dist = dist;
        dist = MARK_LIT;
      }
      else
      {
        p->opt[wr].dist = 0;
        len--;
        wr--;
        p->opt[wr].dist = MARK_LIT;
        p->opt[wr].len = 1;
      }
    }

    if (cur == 0)
    {
      p->backRes = dist;
      p->optCur = wr;
      return len;
    }
    
    wr--;
    p->opt[wr].dist = dist;
    p->opt[wr].len = len;
  }
}



#define LIT_PROBS(pos, prevByte) \
  (p->litProbs + (UInt32)3 * (((((pos) << 8) + (prevByte)) & p->lpMask) << p->lc))


static unsigned GetOptimum(CLzmaEnc *p, UInt32 position)
{
  unsigned last, cur;
  UInt32 reps[LZMA_NUM_REPS];
  unsigned repLens[LZMA_NUM_REPS];
  UInt32 *matches;

  {
    UInt32 numAvail;
    unsigned numPairs, mainLen, repMaxIndex, i, posState;
    UInt32 matchPrice, repMatchPrice;
    const Byte *data;
    Byte curByte, matchByte;
    
    p->optCur = p->optEnd = 0;
    
    if (p->additionalOffset == 0)
      mainLen = ReadMatchDistances(p, &numPairs);
    else
    {
      mainLen = p->longestMatchLen;
      numPairs = p->numPairs;
    }
    
    numAvail = p->numAvail;
    if (numAvail < 2)
    {
      p->backRes = MARK_LIT;
      return 1;
    }
    if (numAvail > LZMA_MATCH_LEN_MAX)
      numAvail = LZMA_MATCH_LEN_MAX;
    
    data = p->matchFinder.GetPointerToCurrentPos(p->matchFinderObj) - 1;
    repMaxIndex = 0;
    
    for (i = 0; i < LZMA_NUM_REPS; i++)
    {
      unsigned len;
      const Byte *data2;
      reps[i] = p->reps[i];
      data2 = data - reps[i];
      if (data[0] != data2[0] || data[1] != data2[1])
      {
        repLens[i] = 0;
        continue;
      }
      for (len = 2; len < numAvail && data[len] == data2[len]; len++);
      repLens[i] = len;
      if (len > repLens[repMaxIndex])
        repMaxIndex = i;
    }
    
    if (repLens[repMaxIndex] >= p->numFastBytes)
    {
      unsigned len;
      p->backRes = repMaxIndex;
      len = repLens[repMaxIndex];
      MOVE_POS(p, len - 1)
      return len;
    }
    
    matches = p->matches;
    
    if (mainLen >= p->numFastBytes)
    {
      p->backRes = matches[(size_t)numPairs - 1] + LZMA_NUM_REPS;
      MOVE_POS(p, mainLen - 1)
      return mainLen;
    }
    
    curByte = *data;
    matchByte = *(data - reps[0]);
    
    if (mainLen < 2 && curByte != matchByte && repLens[repMaxIndex] < 2)
    {
      p->backRes = MARK_LIT;
      return 1;
    }
    
    p->opt[0].state = (CState)p->state;
    
    posState = (position & p->pbMask);
    
    {
      const CLzmaProb *probs = LIT_PROBS(position, *(data - 1));
      p->opt[1].price = GET_PRICE_0(p->isMatch[p->state][posState]) +
        (!IsLitState(p->state) ?
          LitEnc_Matched_GetPrice(probs, curByte, matchByte, p->ProbPrices) :
          LitEnc_GetPrice(probs, curByte, p->ProbPrices));
    }
    
    MakeAs_Lit(&p->opt[1]);
    
    matchPrice = GET_PRICE_1(p->isMatch[p->state][posState]);
    repMatchPrice = matchPrice + GET_PRICE_1(p->isRep[p->state]);
    
    if (matchByte == curByte)
    {
      UInt32 shortRepPrice = repMatchPrice + GetPrice_ShortRep(p, p->state, posState);
      if (shortRepPrice < p->opt[1].price)
      {
        p->opt[1].price = shortRepPrice;
        MakeAs_ShortRep(&p->opt[1]);
      }
    }
    
    last = (mainLen >= repLens[repMaxIndex] ? mainLen : repLens[repMaxIndex]);
    
    if (last < 2)
    {
      p->backRes = p->opt[1].dist;
      return 1;
    }
    
    p->opt[1].len = 1;
    
    p->opt[0].reps[0] = reps[0];
    p->opt[0].reps[1] = reps[1];
    p->opt[0].reps[2] = reps[2];
    p->opt[0].reps[3] = reps[3];
    
    {
      unsigned len = last;
      do
        p->opt[len--].price = kInfinityPrice;
      while (len >= 2);
    }

    // ---------- REP ----------
    
    for (i = 0; i < LZMA_NUM_REPS; i++)
    {
      unsigned repLen = repLens[i];
      UInt32 price;
      if (repLen < 2)
        continue;
      price = repMatchPrice + GetPrice_PureRep(p, i, p->state, posState);
      do
      {
        UInt32 price2 = price + p->repLenEnc.prices[posState][(size_t)repLen - 2];
        COptimal *opt = &p->opt[repLen];
        if (price2 < opt->price)
        {
          opt->price = price2;
          opt->len = repLen;
          opt->dist = i;
          opt->extra = 0;
        }
      }
      while (--repLen >= 2);
    }
    
    
    // ---------- MATCH ----------
    {
      unsigned len  = ((repLens[0] >= 2) ? repLens[0] + 1 : 2);
      if (len <= mainLen)
      {
        unsigned offs = 0;
        UInt32 normalMatchPrice = matchPrice + GET_PRICE_0(p->isRep[p->state]);

        while (len > matches[offs])
          offs += 2;
    
        for (; ; len++)
        {
          COptimal *opt;
          UInt32 dist = matches[(size_t)offs + 1];
          UInt32 price2 = normalMatchPrice + p->lenEnc.prices[posState][(size_t)len - LZMA_MATCH_LEN_MIN];
          unsigned lenToPosState = GetLenToPosState(len);
       
          if (dist < kNumFullDistances)
            price2 += p->distancesPrices[lenToPosState][dist & (kNumFullDistances - 1)];
          else
          {
            unsigned slot;
            GetPosSlot2(dist, slot);
            price2 += p->alignPrices[dist & kAlignMask];
            price2 += p->posSlotPrices[lenToPosState][slot];
          }
          
          opt = &p->opt[len];
          
          if (price2 < opt->price)
          {
            opt->price = price2;
            opt->len = len;
            opt->dist = dist + LZMA_NUM_REPS;
            opt->extra = 0;
          }
          
          if (len == matches[offs])
          {
            offs += 2;
            if (offs == numPairs)
              break;
          }
        }
      }
    }
    

    cur = 0;

    #ifdef SHOW_STAT2
    /* if (position >= 0) */
    {
      unsigned i;
      printf("\n pos = %4X", position);
      for (i = cur; i <= last; i++)
      printf("\nprice[%4X] = %u", position - cur + i, p->opt[i].price);
    }
    #endif
  }


  
  // ---------- Optimal Parsing ----------

  for (;;)
  {
    UInt32 numAvail, numAvailFull;
    unsigned newLen, numPairs, prev, state, posState, startLen;
    UInt32 curPrice, litPrice, matchPrice, repMatchPrice;
    Bool nextIsLit;
    Byte curByte, matchByte;
    const Byte *data;
    COptimal *curOpt, *nextOpt;

    if (++cur == last)
      return Backward(p, cur);

    newLen = ReadMatchDistances(p, &numPairs);
    
    if (newLen >= p->numFastBytes)
    {
      p->numPairs = numPairs;
      p->longestMatchLen = newLen;
      return Backward(p, cur);
    }
    
    curOpt = &p->opt[cur];
    prev = cur - curOpt->len;
    
    if (curOpt->len == 1)
    {
      state = p->opt[prev].state;
      if (IsShortRep(curOpt))
        state = kShortRepNextStates[state];
      else
        state = kLiteralNextStates[state];
    }
    else
    {
      const COptimal *prevOpt;
      UInt32 b0;
      UInt32 dist = curOpt->dist;

      if (curOpt->extra)
      {
        prev -= curOpt->extra;
        state = kState_RepAfterLit;
        if (curOpt->extra == 1)
          state = (dist < LZMA_NUM_REPS) ? kState_RepAfterLit : kState_MatchAfterLit;
      }
      else
      {
        state = p->opt[prev].state;
        if (dist < LZMA_NUM_REPS)
          state = kRepNextStates[state];
        else
          state = kMatchNextStates[state];
      }

      prevOpt = &p->opt[prev];
      b0 = prevOpt->reps[0];

      if (dist < LZMA_NUM_REPS)
      {
        if (dist == 0)
        {
          reps[0] = b0;
          reps[1] = prevOpt->reps[1];
          reps[2] = prevOpt->reps[2];
          reps[3] = prevOpt->reps[3];
        }
        else
        {
          reps[1] = b0;
          b0 = prevOpt->reps[1];
          if (dist == 1)
          {
            reps[0] = b0;
            reps[2] = prevOpt->reps[2];
            reps[3] = prevOpt->reps[3];
          }
          else
          {
            reps[2] = b0;
            reps[0] = prevOpt->reps[dist];
            reps[3] = prevOpt->reps[dist ^ 1];
          }
        }
      }
      else
      {
        reps[0] = (dist - LZMA_NUM_REPS + 1);
        reps[1] = b0;
        reps[2] = prevOpt->reps[1];
        reps[3] = prevOpt->reps[2];
      }
    }
    
    curOpt->state = (CState)state;
    curOpt->reps[0] = reps[0];
    curOpt->reps[1] = reps[1];
    curOpt->reps[2] = reps[2];
    curOpt->reps[3] = reps[3];

    data = p->matchFinder.GetPointerToCurrentPos(p->matchFinderObj) - 1;
    curByte = *data;
    matchByte = *(data - reps[0]);

    position++;
    posState = (position & p->pbMask);

    /*
    The order of Price checks:
       <  LIT
       <= SHORT_REP
       <  LIT : REP_0
       <  REP    [ : LIT : REP_0 ]
       <  MATCH  [ : LIT : REP_0 ]
    */

    curPrice = curOpt->price;
    litPrice = curPrice + GET_PRICE_0(p->isMatch[state][posState]);

    nextOpt = &p->opt[(size_t)cur + 1];
    nextIsLit = False;

    // if (litPrice >= nextOpt->price) litPrice = 0; else // 18.new
    {
      const CLzmaProb *probs = LIT_PROBS(position, *(data - 1));
      litPrice += (!IsLitState(state) ?
          LitEnc_Matched_GetPrice(probs, curByte, matchByte, p->ProbPrices) :
          LitEnc_GetPrice(probs, curByte, p->ProbPrices));
      
      if (litPrice < nextOpt->price)
      {
        nextOpt->price = litPrice;
        nextOpt->len = 1;
        MakeAs_Lit(nextOpt);
        nextIsLit = True;
      }
    }

    matchPrice = curPrice + GET_PRICE_1(p->isMatch[state][posState]);
    repMatchPrice = matchPrice + GET_PRICE_1(p->isRep[state]);
    
    // ---------- SHORT_REP ----------
    // if (IsLitState(state)) // 18.new
    if (matchByte == curByte)
    // if (repMatchPrice < nextOpt->price) // 18.new
    if (nextOpt->len < 2
        || (nextOpt->dist != 0
            && nextOpt->extra <= 1 // 17.old
        ))
    {
      UInt32 shortRepPrice = repMatchPrice + GetPrice_ShortRep(p, state, posState);
      if (shortRepPrice <= nextOpt->price) // 17.old
      // if (shortRepPrice < nextOpt->price)  // 18.new
      {
        nextOpt->price = shortRepPrice;
        nextOpt->len = 1;
        MakeAs_ShortRep(nextOpt);
        nextIsLit = False;
      }
    }
    
    numAvailFull = p->numAvail;
    {
      UInt32 temp = kNumOpts - 1 - cur;
      if (numAvailFull > temp)
        numAvailFull = temp;
    }

    if (numAvailFull < 2)
      continue;
    numAvail = (numAvailFull <= p->numFastBytes ? numAvailFull : p->numFastBytes);

    // numAvail <= p->numFastBytes

    // ---------- LIT : REP_0 ----------

    if (
        // litPrice != 0 && // 18.new
        !nextIsLit
        && matchByte != curByte
        && numAvailFull > 2)
    {
      const Byte *data2 = data - reps[0];
      if (data[1] == data2[1] && data[2] == data2[2])
      {
        unsigned len;
        unsigned limit = p->numFastBytes + 1;
        if (limit > numAvailFull)
          limit = numAvailFull;
        for (len = 3; len < limit && data[len] == data2[len]; len++);
        
        {
          unsigned state2 = kLiteralNextStates[state];
          unsigned posState2 = (position + 1) & p->pbMask;
          UInt32 price = litPrice + GetPrice_Rep_0(p, state2, posState2);
          {
            unsigned offset = cur + len;
            while (last < offset)
              p->opt[++last].price = kInfinityPrice;
          
            // do
            {
              UInt32 price2;
              COptimal *opt;
              len--;
              // price2 = price + GetPrice_Len_Rep_0(p, len, state2, posState2);
              price2 = price + p->repLenEnc.prices[posState2][len - LZMA_MATCH_LEN_MIN];

              opt = &p->opt[offset];
              // offset--;
              if (price2 < opt->price)
              {
                opt->price = price2;
                opt->len = len;
                opt->dist = 0;
                opt->extra = 1;
              }
            }
            // while (len >= 3);
          }
        }
      }
    }
    
    startLen = 2; /* speed optimization */
    {
      // ---------- REP ----------
      unsigned repIndex = 0; // 17.old
      // unsigned repIndex = IsLitState(state) ? 0 : 1; // 18.notused
      for (; repIndex < LZMA_NUM_REPS; repIndex++)
      {
        unsigned len;
        UInt32 price;
        const Byte *data2 = data - reps[repIndex];
        if (data[0] != data2[0] || data[1] != data2[1])
          continue;
        
        for (len = 2; len < numAvail && data[len] == data2[len]; len++);
        
        // if (len < startLen) continue; // 18.new: speed optimization

        while (last < cur + len)
          p->opt[++last].price = kInfinityPrice;
        {
          unsigned len2 = len;
          price = repMatchPrice + GetPrice_PureRep(p, repIndex, state, posState);
          do
          {
            UInt32 price2 = price + p->repLenEnc.prices[posState][(size_t)len2 - 2];
            COptimal *opt = &p->opt[cur + len2];
            if (price2 < opt->price)
            {
              opt->price = price2;
              opt->len = len2;
              opt->dist = repIndex;
              opt->extra = 0;
            }
          }
          while (--len2 >= 2);
        }
        
        if (repIndex == 0) startLen = len + 1;  // 17.old
        // startLen = len + 1; // 18.new

        /* if (_maxMode) */
        {
          // ---------- REP : LIT : REP_0 ----------
          // numFastBytes + 1 + numFastBytes

          unsigned len2 = len + 1;
          unsigned limit = len2 + p->numFastBytes;
          if (limit > numAvailFull)
            limit = numAvailFull;
          
          for (; len2 < limit && data[len2] == data2[len2]; len2++);
          
          len2 -= len;
          if (len2 >= 3)
          {
            unsigned state2 = kRepNextStates[state];
            unsigned posState2 = (position + len) & p->pbMask;
            price +=
                  p->repLenEnc.prices[posState][(size_t)len - 2]
                + GET_PRICE_0(p->isMatch[state2][posState2])
                + LitEnc_Matched_GetPrice(LIT_PROBS(position + len, data[(size_t)len - 1]),
                    data[len], data2[len], p->ProbPrices);
            
            // state2 = kLiteralNextStates[state2];
            state2 = kState_LitAfterRep;
            posState2 = (posState2 + 1) & p->pbMask;


            price += GetPrice_Rep_0(p, state2, posState2);
            {
              unsigned offset = cur + len + len2;
              while (last < offset)
                p->opt[++last].price = kInfinityPrice;
              // do
              {
                unsigned price2;
                COptimal *opt;
                len2--;
                // price2 = price + GetPrice_Len_Rep_0(p, len2, state2, posState2);
                price2 = price + p->repLenEnc.prices[posState2][len2 - LZMA_MATCH_LEN_MIN];

                opt = &p->opt[offset];
                // offset--;
                if (price2 < opt->price)
                {
                  opt->price = price2;
                  opt->len = len2;
                  opt->extra = (CExtra)(len + 1);
                  opt->dist = repIndex;
                }
              }
              // while (len2 >= 3);
            }
          }
        }
      }
    }


    // ---------- MATCH ----------
    /* for (unsigned len = 2; len <= newLen; len++) */
    if (newLen > numAvail)
    {
      newLen = numAvail;
      for (numPairs = 0; newLen > matches[numPairs]; numPairs += 2);
      matches[numPairs] = newLen;
      numPairs += 2;
    }
    
    if (newLen >= startLen)
    {
      UInt32 normalMatchPrice = matchPrice + GET_PRICE_0(p->isRep[state]);
      UInt32 dist;
      unsigned offs, posSlot, len;
      while (last < cur + newLen)
        p->opt[++last].price = kInfinityPrice;

      offs = 0;
      while (startLen > matches[offs])
        offs += 2;
      dist = matches[(size_t)offs + 1];
      
      // if (dist >= kNumFullDistances)
      GetPosSlot2(dist, posSlot);
      
      for (len = /*2*/ startLen; ; len++)
      {
        UInt32 price = normalMatchPrice + p->lenEnc.prices[posState][(size_t)len - LZMA_MATCH_LEN_MIN];
        {
          COptimal *opt;
          unsigned lenToPosState = len - 2; lenToPosState = GetLenToPosState2(lenToPosState);
          if (dist < kNumFullDistances)
            price += p->distancesPrices[lenToPosState][dist & (kNumFullDistances - 1)];
          else
            price += p->posSlotPrices[lenToPosState][posSlot] + p->alignPrices[dist & kAlignMask];
          
          opt = &p->opt[cur + len];
          if (price < opt->price)
          {
            opt->price = price;
            opt->len = len;
            opt->dist = dist + LZMA_NUM_REPS;
            opt->extra = 0;
          }
        }

        if (/*_maxMode && */ len == matches[offs])
        {
          // MATCH : LIT : REP_0

          const Byte *data2 = data - dist - 1;
          unsigned len2 = len + 1;
          unsigned limit = len2 + p->numFastBytes;
          if (limit > numAvailFull)
            limit = numAvailFull;
          
          for (; len2 < limit && data[len2] == data2[len2]; len2++);
          
          len2 -= len;
          
          if (len2 >= 3)
          {
            unsigned state2 = kMatchNextStates[state];
            unsigned posState2 = (position + len) & p->pbMask;
            unsigned offset;
            price += GET_PRICE_0(p->isMatch[state2][posState2]);
            price += LitEnc_Matched_GetPrice(LIT_PROBS(position + len, data[(size_t)len - 1]),
                    data[len], data2[len], p->ProbPrices);

            // state2 = kLiteralNextStates[state2];
            state2 = kState_LitAfterMatch;

            posState2 = (posState2 + 1) & p->pbMask;
            price += GetPrice_Rep_0(p, state2, posState2);

            offset = cur + len + len2;
            while (last < offset)
              p->opt[++last].price = kInfinityPrice;
            // do
            {
              UInt32 price2;
              COptimal *opt;
              len2--;
              // price2 = price + GetPrice_Len_Rep_0(p, len2, state2, posState2);
              price2 = price + p->repLenEnc.prices[posState2][len2 - LZMA_MATCH_LEN_MIN];
              opt = &p->opt[offset];
              // offset--;
              if (price2 < opt->price)
              {
                opt->price = price2;
                opt->len = len2;
                opt->extra = (CExtra)(len + 1);
                opt->dist = dist + LZMA_NUM_REPS;
              }
            }
            // while (len2 >= 3);
          }
        
          offs += 2;
          if (offs == numPairs)
            break;
          dist = matches[(size_t)offs + 1];
          // if (dist >= kNumFullDistances)
            GetPosSlot2(dist, posSlot);
        }
      }
    }
  }
}



#define ChangePair(smallDist, bigDist) (((bigDist) >> 7) > (smallDist))



static unsigned GetOptimumFast(CLzmaEnc *p)
{
  UInt32 numAvail, mainDist;
  unsigned mainLen, numPairs, repIndex, repLen, i;
  const Byte *data;

  if (p->additionalOffset == 0)
    mainLen = ReadMatchDistances(p, &numPairs);
  else
  {
    mainLen = p->longestMatchLen;
    numPairs = p->numPairs;
  }

  numAvail = p->numAvail;
  p->backRes = MARK_LIT;
  if (numAvail < 2)
    return 1;
  if (numAvail > LZMA_MATCH_LEN_MAX)
    numAvail = LZMA_MATCH_LEN_MAX;
  data = p->matchFinder.GetPointerToCurrentPos(p->matchFinderObj) - 1;
  repLen = repIndex = 0;
  
  for (i = 0; i < LZMA_NUM_REPS; i++)
  {
    unsigned len;
    const Byte *data2 = data - p->reps[i];
    if (data[0] != data2[0] || data[1] != data2[1])
      continue;
    for (len = 2; len < numAvail && data[len] == data2[len]; len++);
    if (len >= p->numFastBytes)
    {
      p->backRes = i;
      MOVE_POS(p, len - 1)
      return len;
    }
    if (len > repLen)
    {
      repIndex = i;
      repLen = len;
    }
  }

  if (mainLen >= p->numFastBytes)
  {
    p->backRes = p->matches[(size_t)numPairs - 1] + LZMA_NUM_REPS;
    MOVE_POS(p, mainLen - 1)
    return mainLen;
  }

  mainDist = 0; /* for GCC */
  
  if (mainLen >= 2)
  {
    mainDist = p->matches[(size_t)numPairs - 1];
    while (numPairs > 2)
    {
      UInt32 dist2;
      if (mainLen != p->matches[(size_t)numPairs - 4] + 1)
        break;
      dist2 = p->matches[(size_t)numPairs - 3];
      if (!ChangePair(dist2, mainDist))
        break;
      numPairs -= 2;
      mainLen--;
      mainDist = dist2;
    }
    if (mainLen == 2 && mainDist >= 0x80)
      mainLen = 1;
  }

  if (repLen >= 2)
    if (    repLen + 1 >= mainLen
        || (repLen + 2 >= mainLen && mainDist >= (1 << 9))
        || (repLen + 3 >= mainLen && mainDist >= (1 << 15)))
  {
    p->backRes = repIndex;
    MOVE_POS(p, repLen - 1)
    return repLen;
  }
  
  if (mainLen < 2 || numAvail <= 2)
    return 1;

  {
    unsigned len1 = ReadMatchDistances(p, &p->numPairs);
    p->longestMatchLen = len1;
  
    if (len1 >= 2)
    {
      UInt32 newDist = p->matches[(size_t)p->numPairs - 1];
      if (   (len1 >= mainLen && newDist < mainDist)
          || (len1 == mainLen + 1 && !ChangePair(mainDist, newDist))
          || (len1 >  mainLen + 1)
          || (len1 + 1 >= mainLen && mainLen >= 3 && ChangePair(newDist, mainDist)))
        return 1;
    }
  }
  
  data = p->matchFinder.GetPointerToCurrentPos(p->matchFinderObj) - 1;
  
  for (i = 0; i < LZMA_NUM_REPS; i++)
  {
    unsigned len, limit;
    const Byte *data2 = data - p->reps[i];
    if (data[0] != data2[0] || data[1] != data2[1])
      continue;
    limit = mainLen - 1;
    for (len = 2;; len++)
    {
      if (len >= limit)
        return 1;
      if (data[len] != data2[len])
        break;
    }
  }
  
  p->backRes = mainDist + LZMA_NUM_REPS;
  if (mainLen != 2)
  {
    MOVE_POS(p, mainLen - 2)
  }
  return mainLen;
}




static void WriteEndMarker(CLzmaEnc *p, unsigned posState)
{
  UInt32 range;
  range = p->rc.range;
  {
    UInt32 ttt, newBound;
    CLzmaProb *prob = &p->isMatch[p->state][posState];
    RC_BIT_PRE(&p->rc, prob)
    RC_BIT_1(&p->rc, prob)
    prob = &p->isRep[p->state];
    RC_BIT_PRE(&p->rc, prob)
    RC_BIT_0(&p->rc, prob)
  }
  p->state = kMatchNextStates[p->state];
  
  p->rc.range = range;
  LenEnc_Encode(&p->lenProbs, &p->rc, 0, posState);
  range = p->rc.range;

  {
    // RcTree_Encode_PosSlot(&p->rc, p->posSlotEncoder[0], (1 << kNumPosSlotBits) - 1);
    CLzmaProb *probs = p->posSlotEncoder[0];
    unsigned m = 1;
    do
    {
      UInt32 ttt, newBound;
      RC_BIT_PRE(p, probs + m)
      RC_BIT_1(&p->rc, probs + m);
      m = (m << 1) + 1;
    }
    while (m < (1 << kNumPosSlotBits));
  }
  {
    // RangeEnc_EncodeDirectBits(&p->rc, ((UInt32)1 << (30 - kNumAlignBits)) - 1, 30 - kNumAlignBits);    UInt32 range = p->range;
    unsigned numBits = 30 - kNumAlignBits;
    do
    {
      range >>= 1;
      p->rc.low += range;
      RC_NORM(&p->rc)
    }
    while (--numBits);
  }
   
  {
    // RcTree_ReverseEncode(&p->rc, p->posAlignEncoder, kNumAlignBits, kAlignMask);
    CLzmaProb *probs = p->posAlignEncoder;
    unsigned m = 1;
    do
    {
      UInt32 ttt, newBound;
      RC_BIT_PRE(p, probs + m)
      RC_BIT_1(&p->rc, probs + m);
      m = (m << 1) + 1;
    }
    while (m < kAlignTableSize);
  }
  p->rc.range = range;
}


static SRes CheckErrors(CLzmaEnc *p)
{
  if (p->result != SZ_OK)
    return p->result;
  if (p->rc.res != SZ_OK)
    p->result = SZ_ERROR_WRITE;
  if (p->matchFinderBase.result != SZ_OK)
    p->result = SZ_ERROR_READ;
  if (p->result != SZ_OK)
    p->finished = True;
  return p->result;
}


MY_NO_INLINE static SRes Flush(CLzmaEnc *p, UInt32 nowPos)
{
  /* ReleaseMFStream(); */
  p->finished = True;
  if (p->writeEndMark)
    WriteEndMarker(p, nowPos & p->pbMask);
  RangeEnc_FlushData(&p->rc);
  RangeEnc_FlushStream(&p->rc);
  return CheckErrors(p);
}



static void FillAlignPrices(CLzmaEnc *p)
{
  unsigned i;
  const CProbPrice *ProbPrices = p->ProbPrices;
  const CLzmaProb *probs = p->posAlignEncoder;
  p->alignPriceCount = 0;
  for (i = 0; i < kAlignTableSize / 2; i++)
  {
    UInt32 price = 0;
    unsigned symbol = i;
    unsigned m = 1;
    unsigned bit;
    UInt32 prob;
    bit = symbol & 1; symbol >>= 1; price += GET_PRICEa(probs[m], bit); m = (m << 1) + bit;
    bit = symbol & 1; symbol >>= 1; price += GET_PRICEa(probs[m], bit); m = (m << 1) + bit;
    bit = symbol & 1; symbol >>= 1; price += GET_PRICEa(probs[m], bit); m = (m << 1) + bit;
    prob = probs[m];
    p->alignPrices[i    ] = price + GET_PRICEa_0(prob);
    p->alignPrices[i + 8] = price + GET_PRICEa_1(prob);
    // p->alignPrices[i] = RcTree_ReverseGetPrice(p->posAlignEncoder, kNumAlignBits, i, p->ProbPrices);
  }
}


static void FillDistancesPrices(CLzmaEnc *p)
{
  UInt32 tempPrices[kNumFullDistances];
  unsigned i, lenToPosState;

  const CProbPrice *ProbPrices = p->ProbPrices;
  p->matchPriceCount = 0;

  for (i = kStartPosModelIndex; i < kNumFullDistances; i++)
  {
    unsigned posSlot = GetPosSlot1(i);
    unsigned footerBits = ((posSlot >> 1) - 1);
    unsigned base = ((2 | (posSlot & 1)) << footerBits);
    // tempPrices[i] = RcTree_ReverseGetPrice(p->posEncoders + base, footerBits, i - base, p->ProbPrices);

    const CLzmaProb *probs = p->posEncoders + base;
    UInt32 price = 0;
    unsigned m = 1;
    unsigned symbol = i - base;
    do
    {
      unsigned bit = symbol & 1;
      symbol >>= 1;
      price += GET_PRICEa(probs[m], bit);
      m = (m << 1) + bit;
    }
    while (--footerBits);
    tempPrices[i] = price;
  }

  for (lenToPosState = 0; lenToPosState < kNumLenToPosStates; lenToPosState++)
  {
    unsigned posSlot;
    const CLzmaProb *encoder = p->posSlotEncoder[lenToPosState];
    UInt32 *posSlotPrices = p->posSlotPrices[lenToPosState];
    unsigned distTableSize = p->distTableSize;
    const CLzmaProb *probs = encoder;
    for (posSlot = 0; posSlot < distTableSize; posSlot += 2)
    {
      // posSlotPrices[posSlot] = RcTree_GetPrice(encoder, kNumPosSlotBits, posSlot, p->ProbPrices);
      UInt32 price = 0;
      unsigned bit;
      unsigned symbol = (posSlot >> 1) + (1 << (kNumPosSlotBits - 1));
      UInt32 prob;
      bit = symbol & 1; symbol >>= 1; price += GET_PRICEa(probs[symbol], bit);
      bit = symbol & 1; symbol >>= 1; price += GET_PRICEa(probs[symbol], bit);
      bit = symbol & 1; symbol >>= 1; price += GET_PRICEa(probs[symbol], bit);
      bit = symbol & 1; symbol >>= 1; price += GET_PRICEa(probs[symbol], bit);
      bit = symbol & 1; symbol >>= 1; price += GET_PRICEa(probs[symbol], bit);
      prob = probs[(posSlot >> 1) + (1 << (kNumPosSlotBits - 1))];
      posSlotPrices[posSlot    ] = price + GET_PRICEa_0(prob);
      posSlotPrices[posSlot + 1] = price + GET_PRICEa_1(prob);
    }
    for (posSlot = kEndPosModelIndex; posSlot < distTableSize; posSlot++)
      posSlotPrices[posSlot] += ((UInt32)(((posSlot >> 1) - 1) - kNumAlignBits) << kNumBitPriceShiftBits);

    {
      UInt32 *distancesPrices = p->distancesPrices[lenToPosState];
      {
        distancesPrices[0] = posSlotPrices[0];
        distancesPrices[1] = posSlotPrices[1];
        distancesPrices[2] = posSlotPrices[2];
        distancesPrices[3] = posSlotPrices[3];
      }
      for (i = 4; i < kNumFullDistances; i += 2)
      {
        UInt32 slotPrice = posSlotPrices[GetPosSlot1(i)];
        distancesPrices[i    ] = slotPrice + tempPrices[i];
        distancesPrices[i + 1] = slotPrice + tempPrices[i + 1];
      }
    }
  }
}



void LzmaEnc_Construct(CLzmaEnc *p)
{
  RangeEnc_Construct(&p->rc);
  MatchFinder_Construct(&p->matchFinderBase);
  
  #ifndef _7ZIP_ST
  MatchFinderMt_Construct(&p->matchFinderMt);
  p->matchFinderMt.MatchFinder = &p->matchFinderBase;
  #endif

  {
    CLzmaEncProps props;
    LzmaEncProps_Init(&props);
    LzmaEnc_SetProps(p, &props);
  }

  #ifndef LZMA_LOG_BSR
  LzmaEnc_FastPosInit(p->g_FastPos);
  #endif

  LzmaEnc_InitPriceTables(p->ProbPrices);
  p->litProbs = NULL;
  p->saveState.litProbs = NULL;

}

CLzmaEncHandle LzmaEnc_Create(ISzAllocPtr alloc)
{
  void *p;
  p = ISzAlloc_Alloc(alloc, sizeof(CLzmaEnc));
  if (p)
    LzmaEnc_Construct((CLzmaEnc *)p);
  return p;
}

void LzmaEnc_FreeLits(CLzmaEnc *p, ISzAllocPtr alloc)
{
  ISzAlloc_Free(alloc, p->litProbs);
  ISzAlloc_Free(alloc, p->saveState.litProbs);
  p->litProbs = NULL;
  p->saveState.litProbs = NULL;
}

void LzmaEnc_Destruct(CLzmaEnc *p, ISzAllocPtr alloc, ISzAllocPtr allocBig)
{
  #ifndef _7ZIP_ST
  MatchFinderMt_Destruct(&p->matchFinderMt, allocBig);
  #endif
  
  MatchFinder_Free(&p->matchFinderBase, allocBig);
  LzmaEnc_FreeLits(p, alloc);
  RangeEnc_Free(&p->rc, alloc);
}

void LzmaEnc_Destroy(CLzmaEncHandle p, ISzAllocPtr alloc, ISzAllocPtr allocBig)
{
  LzmaEnc_Destruct((CLzmaEnc *)p, alloc, allocBig);
  ISzAlloc_Free(alloc, p);
}


static SRes LzmaEnc_CodeOneBlock(CLzmaEnc *p, UInt32 maxPackSize, UInt32 maxUnpackSize)
{
  UInt32 nowPos32, startPos32;
  if (p->needInit)
  {
    p->matchFinder.Init(p->matchFinderObj);
    p->needInit = 0;
  }

  if (p->finished)
    return p->result;
  RINOK(CheckErrors(p));

  nowPos32 = (UInt32)p->nowPos64;
  startPos32 = nowPos32;

  if (p->nowPos64 == 0)
  {
    unsigned numPairs;
    Byte curByte;
    if (p->matchFinder.GetNumAvailableBytes(p->matchFinderObj) == 0)
      return Flush(p, nowPos32);
    ReadMatchDistances(p, &numPairs);
    RangeEnc_EncodeBit_0(&p->rc, &p->isMatch[kState_Start][0]);
    // p->state = kLiteralNextStates[p->state];
    curByte = *(p->matchFinder.GetPointerToCurrentPos(p->matchFinderObj) - p->additionalOffset);
    LitEnc_Encode(&p->rc, p->litProbs, curByte);
    p->additionalOffset--;
    nowPos32++;
  }

  if (p->matchFinder.GetNumAvailableBytes(p->matchFinderObj) != 0)
  
  for (;;)
  {
    UInt32 dist;
    unsigned len, posState;
    UInt32 range, ttt, newBound;
    CLzmaProb *probs;
  
    if (p->fastMode)
      len = GetOptimumFast(p);
    else
    {
      unsigned oci = p->optCur;
      if (p->optEnd == oci)
        len = GetOptimum(p, nowPos32);
      else
      {
        const COptimal *opt = &p->opt[oci];
        len = opt->len;
        p->backRes = opt->dist;
        p->optCur = oci + 1;
      }
    }

    posState = (unsigned)nowPos32 & p->pbMask;
    range = p->rc.range;
    probs = &p->isMatch[p->state][posState];
    
    RC_BIT_PRE(&p->rc, probs)
    
    dist = p->backRes;

    #ifdef SHOW_STAT2
    printf("\n pos = %6X, len = %3u  pos = %6u", nowPos32, len, dist);
    #endif

    if (dist == MARK_LIT)
    {
      Byte curByte;
      const Byte *data;
      unsigned state;

      RC_BIT_0(&p->rc, probs);
      p->rc.range = range;
      data = p->matchFinder.GetPointerToCurrentPos(p->matchFinderObj) - p->additionalOffset;
      probs = LIT_PROBS(nowPos32, *(data - 1));
      curByte = *data;
      state = p->state;
      p->state = kLiteralNextStates[state];
      if (IsLitState(state))
        LitEnc_Encode(&p->rc, probs, curByte);
      else
        LitEnc_EncodeMatched(&p->rc, probs, curByte, *(data - p->reps[0]));
    }
    else
    {
      RC_BIT_1(&p->rc, probs);
      probs = &p->isRep[p->state];
      RC_BIT_PRE(&p->rc, probs)
      
      if (dist < LZMA_NUM_REPS)
      {
        RC_BIT_1(&p->rc, probs);
        probs = &p->isRepG0[p->state];
        RC_BIT_PRE(&p->rc, probs)
        if (dist == 0)
        {
          RC_BIT_0(&p->rc, probs);
          probs = &p->isRep0Long[p->state][posState];
          RC_BIT_PRE(&p->rc, probs)
          if (len != 1)
          {
            RC_BIT_1_BASE(&p->rc, probs);
          }
          else
          {
            RC_BIT_0_BASE(&p->rc, probs);
            p->state = kShortRepNextStates[p->state];
          }
        }
        else
        {
          RC_BIT_1(&p->rc, probs);
          probs = &p->isRepG1[p->state];
          RC_BIT_PRE(&p->rc, probs)
          if (dist == 1)
          {
            RC_BIT_0_BASE(&p->rc, probs);
            dist = p->reps[1];
          }
          else
          {
            RC_BIT_1(&p->rc, probs);
            probs = &p->isRepG2[p->state];
            RC_BIT_PRE(&p->rc, probs)
            if (dist == 2)
            {
              RC_BIT_0_BASE(&p->rc, probs);
              dist = p->reps[2];
            }
            else
            {
              RC_BIT_1_BASE(&p->rc, probs);
              dist = p->reps[3];
              p->reps[3] = p->reps[2];
            }
            p->reps[2] = p->reps[1];
          }
          p->reps[1] = p->reps[0];
          p->reps[0] = dist;
        }

        RC_NORM(&p->rc)

        p->rc.range = range;

        if (len != 1)
        {
          LenEnc_Encode(&p->repLenProbs, &p->rc, len - LZMA_MATCH_LEN_MIN, posState);
          if (!p->fastMode)
            if (--p->repLenEnc.counters[posState] == 0)
              LenPriceEnc_UpdateTable(&p->repLenEnc, posState, &p->repLenProbs, p->ProbPrices);

          p->state = kRepNextStates[p->state];
        }
      }
      else
      {
        unsigned posSlot;
        RC_BIT_0(&p->rc, probs);
        p->rc.range = range;
        p->state = kMatchNextStates[p->state];

        LenEnc_Encode(&p->lenProbs, &p->rc, len - LZMA_MATCH_LEN_MIN, posState);
        if (!p->fastMode)
          if (--p->lenEnc.counters[posState] == 0)
            LenPriceEnc_UpdateTable(&p->lenEnc, posState, &p->lenProbs, p->ProbPrices);

        dist -= LZMA_NUM_REPS;
        p->reps[3] = p->reps[2];
        p->reps[2] = p->reps[1];
        p->reps[1] = p->reps[0];
        p->reps[0] = dist + 1;
        
        p->matchPriceCount++;
        GetPosSlot(dist, posSlot);
        // RcTree_Encode_PosSlot(&p->rc, p->posSlotEncoder[GetLenToPosState(len)], posSlot);
        {
          UInt32 symbol = posSlot + (1 << kNumPosSlotBits);
          range = p->rc.range;
          probs = p->posSlotEncoder[GetLenToPosState(len)];
          do
          {
            CLzmaProb *prob = probs + (symbol >> kNumPosSlotBits);
            UInt32 bit = (symbol >> (kNumPosSlotBits - 1)) & 1;
            symbol <<= 1;
            RC_BIT(&p->rc, prob, bit);
          }
          while (symbol < (1 << kNumPosSlotBits * 2));
          p->rc.range = range;
        }
        
        if (dist >= kStartPosModelIndex)
        {
          unsigned footerBits = ((posSlot >> 1) - 1);

          if (dist < kNumFullDistances)
          {
            unsigned base = ((2 | (posSlot & 1)) << footerBits);
            RcTree_ReverseEncode(&p->rc, p->posEncoders + base, footerBits, dist - base);
          }
          else
          {
            UInt32 pos2 = (dist | 0xF) << (32 - footerBits);
            range = p->rc.range;
            // RangeEnc_EncodeDirectBits(&p->rc, posReduced >> kNumAlignBits, footerBits - kNumAlignBits);
            /*
            do
            {
              range >>= 1;
              p->rc.low += range & (0 - ((dist >> --footerBits) & 1));
              RC_NORM(&p->rc)
            }
            while (footerBits > kNumAlignBits);
            */
            do
            {
              range >>= 1;
              p->rc.low += range & (0 - (pos2 >> 31));
              pos2 += pos2;
              RC_NORM(&p->rc)
            }
            while (pos2 != 0xF0000000);


            // RcTree_ReverseEncode(&p->rc, p->posAlignEncoder, kNumAlignBits, posReduced & kAlignMask);

            {
              unsigned m = 1;
              unsigned bit;
              bit = dist & 1; dist >>= 1; RC_BIT(&p->rc, p->posAlignEncoder + m, bit); m = (m << 1) + bit;
              bit = dist & 1; dist >>= 1; RC_BIT(&p->rc, p->posAlignEncoder + m, bit); m = (m << 1) + bit;
              bit = dist & 1; dist >>= 1; RC_BIT(&p->rc, p->posAlignEncoder + m, bit); m = (m << 1) + bit;
              bit = dist & 1;             RC_BIT(&p->rc, p->posAlignEncoder + m, bit);
              p->rc.range = range;
              p->alignPriceCount++;
            }
          }
        }
      }
    }

    nowPos32 += len;
    p->additionalOffset -= len;
    
    if (p->additionalOffset == 0)
    {
      UInt32 processed;

      if (!p->fastMode)
      {
        if (p->matchPriceCount >= (1 << 7))
          FillDistancesPrices(p);
        if (p->alignPriceCount >= kAlignTableSize)
          FillAlignPrices(p);
      }
    
      if (p->matchFinder.GetNumAvailableBytes(p->matchFinderObj) == 0)
        break;
      processed = nowPos32 - startPos32;
      
      if (maxPackSize)
      {
        if (processed + kNumOpts + 300 >= maxUnpackSize
            || RangeEnc_GetProcessed_sizet(&p->rc) + kPackReserve >= maxPackSize)
          break;
      }
      else if (processed >= (1 << 17))
      {
        p->nowPos64 += nowPos32 - startPos32;
        return CheckErrors(p);
      }
    }
  }

  p->nowPos64 += nowPos32 - startPos32;
  return Flush(p, nowPos32);
}



#define kBigHashDicLimit ((UInt32)1 << 24)

static SRes LzmaEnc_Alloc(CLzmaEnc *p, UInt32 keepWindowSize, ISzAllocPtr alloc, ISzAllocPtr allocBig)
{
  UInt32 beforeSize = kNumOpts;
  if (!RangeEnc_Alloc(&p->rc, alloc))
    return SZ_ERROR_MEM;

  #ifndef _7ZIP_ST
  p->mtMode = (p->multiThread && !p->fastMode && (p->matchFinderBase.btMode != 0));
  #endif

  {
    unsigned lclp = p->lc + p->lp;
    if (!p->litProbs || !p->saveState.litProbs || p->lclp != lclp)
    {
      LzmaEnc_FreeLits(p, alloc);
      p->litProbs = (CLzmaProb *)ISzAlloc_Alloc(alloc, ((UInt32)0x300 << lclp) * sizeof(CLzmaProb));
      p->saveState.litProbs = (CLzmaProb *)ISzAlloc_Alloc(alloc, ((UInt32)0x300 << lclp) * sizeof(CLzmaProb));
      if (!p->litProbs || !p->saveState.litProbs)
      {
        LzmaEnc_FreeLits(p, alloc);
        return SZ_ERROR_MEM;
      }
      p->lclp = lclp;
    }
  }

  p->matchFinderBase.bigHash = (Byte)(p->dictSize > kBigHashDicLimit ? 1 : 0);

  if (beforeSize + p->dictSize < keepWindowSize)
    beforeSize = keepWindowSize - p->dictSize;

  #ifndef _7ZIP_ST
  if (p->mtMode)
  {
    RINOK(MatchFinderMt_Create(&p->matchFinderMt, p->dictSize, beforeSize, p->numFastBytes,
        LZMA_MATCH_LEN_MAX
        + 1  /* 18.04 */
        , allocBig));
    p->matchFinderObj = &p->matchFinderMt;
    p->matchFinderBase.bigHash = (Byte)(
        (p->dictSize > kBigHashDicLimit && p->matchFinderBase.hashMask >= 0xFFFFFF) ? 1 : 0);
    MatchFinderMt_CreateVTable(&p->matchFinderMt, &p->matchFinder);
  }
  else
  #endif
  {
    if (!MatchFinder_Create(&p->matchFinderBase, p->dictSize, beforeSize, p->numFastBytes, LZMA_MATCH_LEN_MAX, allocBig))
      return SZ_ERROR_MEM;
    p->matchFinderObj = &p->matchFinderBase;
    MatchFinder_CreateVTable(&p->matchFinderBase, &p->matchFinder);
  }
  
  return SZ_OK;
}

void LzmaEnc_Init(CLzmaEnc *p)
{
  unsigned i;
  p->state = 0;
  p->reps[0] =
  p->reps[1] =
  p->reps[2] =
  p->reps[3] = 1;

  RangeEnc_Init(&p->rc);

  for (i = 0; i < (1 << kNumAlignBits); i++)
    p->posAlignEncoder[i] = kProbInitValue;

  for (i = 0; i < kNumStates; i++)
  {
    unsigned j;
    for (j = 0; j < LZMA_NUM_PB_STATES_MAX; j++)
    {
      p->isMatch[i][j] = kProbInitValue;
      p->isRep0Long[i][j] = kProbInitValue;
    }
    p->isRep[i] = kProbInitValue;
    p->isRepG0[i] = kProbInitValue;
    p->isRepG1[i] = kProbInitValue;
    p->isRepG2[i] = kProbInitValue;
  }

  {
    for (i = 0; i < kNumLenToPosStates; i++)
    {
      CLzmaProb *probs = p->posSlotEncoder[i];
      unsigned j;
      for (j = 0; j < (1 << kNumPosSlotBits); j++)
        probs[j] = kProbInitValue;
    }
  }
  {
    for (i = 0; i < kNumFullDistances; i++)
      p->posEncoders[i] = kProbInitValue;
  }

  {
    UInt32 num = (UInt32)0x300 << (p->lp + p->lc);
    UInt32 k;
    CLzmaProb *probs = p->litProbs;
    for (k = 0; k < num; k++)
      probs[k] = kProbInitValue;
  }


  LenEnc_Init(&p->lenProbs);
  LenEnc_Init(&p->repLenProbs);

  p->optEnd = 0;
  p->optCur = 0;
  p->additionalOffset = 0;

  p->pbMask = (1 << p->pb) - 1;
  p->lpMask = ((UInt32)0x100 << p->lp) - ((unsigned)0x100 >> p->lc);
}

void LzmaEnc_InitPrices(CLzmaEnc *p)
{
  if (!p->fastMode)
  {
    FillDistancesPrices(p);
    FillAlignPrices(p);
  }

  p->lenEnc.tableSize =
  p->repLenEnc.tableSize =
      p->numFastBytes + 1 - LZMA_MATCH_LEN_MIN;
  LenPriceEnc_UpdateTables(&p->lenEnc, 1 << p->pb, &p->lenProbs, p->ProbPrices);
  LenPriceEnc_UpdateTables(&p->repLenEnc, 1 << p->pb, &p->repLenProbs, p->ProbPrices);
}

static SRes LzmaEnc_AllocAndInit(CLzmaEnc *p, UInt32 keepWindowSize, ISzAllocPtr alloc, ISzAllocPtr allocBig)
{
  unsigned i;
  for (i = kEndPosModelIndex / 2; i < kDicLogSizeMax; i++)
    if (p->dictSize <= ((UInt32)1 << i))
      break;
  p->distTableSize = i * 2;

  p->finished = False;
  p->result = SZ_OK;
  RINOK(LzmaEnc_Alloc(p, keepWindowSize, alloc, allocBig));
  LzmaEnc_Init(p);
  LzmaEnc_InitPrices(p);
  p->nowPos64 = 0;
  return SZ_OK;
}

static SRes LzmaEnc_Prepare(CLzmaEncHandle pp, ISeqOutStream *outStream, ISeqInStream *inStream,
    ISzAllocPtr alloc, ISzAllocPtr allocBig)
{
  CLzmaEnc *p = (CLzmaEnc *)pp;
  p->matchFinderBase.stream = inStream;
  p->needInit = 1;
  p->rc.outStream = outStream;
  return LzmaEnc_AllocAndInit(p, 0, alloc, allocBig);
}

SRes LzmaEnc_PrepareForLzma2(CLzmaEncHandle pp,
    ISeqInStream *inStream, UInt32 keepWindowSize,
    ISzAllocPtr alloc, ISzAllocPtr allocBig)
{
  CLzmaEnc *p = (CLzmaEnc *)pp;
  p->matchFinderBase.stream = inStream;
  p->needInit = 1;
  return LzmaEnc_AllocAndInit(p, keepWindowSize, alloc, allocBig);
}

static void LzmaEnc_SetInputBuf(CLzmaEnc *p, const Byte *src, SizeT srcLen)
{
  p->matchFinderBase.directInput = 1;
  p->matchFinderBase.bufferBase = (Byte *)src;
  p->matchFinderBase.directInputRem = srcLen;
}

SRes LzmaEnc_MemPrepare(CLzmaEncHandle pp, const Byte *src, SizeT srcLen,
    UInt32 keepWindowSize, ISzAllocPtr alloc, ISzAllocPtr allocBig)
{
  CLzmaEnc *p = (CLzmaEnc *)pp;
  LzmaEnc_SetInputBuf(p, src, srcLen);
  p->needInit = 1;

  LzmaEnc_SetDataSize(pp, srcLen);
  return LzmaEnc_AllocAndInit(p, keepWindowSize, alloc, allocBig);
}

void LzmaEnc_Finish(CLzmaEncHandle pp)
{
  #ifndef _7ZIP_ST
  CLzmaEnc *p = (CLzmaEnc *)pp;
  if (p->mtMode)
    MatchFinderMt_ReleaseStream(&p->matchFinderMt);
  #else
  UNUSED_VAR(pp);
  #endif
}


typedef struct
{
  ISeqOutStream vt;
  Byte *data;
  SizeT rem;
  Bool overflow;
} CLzmaEnc_SeqOutStreamBuf;

static size_t SeqOutStreamBuf_Write(const ISeqOutStream *pp, const void *data, size_t size)
{
  CLzmaEnc_SeqOutStreamBuf *p = CONTAINER_FROM_VTBL(pp, CLzmaEnc_SeqOutStreamBuf, vt);
  if (p->rem < size)
  {
    size = p->rem;
    p->overflow = True;
  }
  memcpy(p->data, data, size);
  p->rem -= size;
  p->data += size;
  return size;
}


UInt32 LzmaEnc_GetNumAvailableBytes(CLzmaEncHandle pp)
{
  const CLzmaEnc *p = (CLzmaEnc *)pp;
  return p->matchFinder.GetNumAvailableBytes(p->matchFinderObj);
}


const Byte *LzmaEnc_GetCurBuf(CLzmaEncHandle pp)
{
  const CLzmaEnc *p = (CLzmaEnc *)pp;
  return p->matchFinder.GetPointerToCurrentPos(p->matchFinderObj) - p->additionalOffset;
}


SRes LzmaEnc_CodeOneMemBlock(CLzmaEncHandle pp, Bool reInit,
    Byte *dest, size_t *destLen, UInt32 desiredPackSize, UInt32 *unpackSize)
{
  CLzmaEnc *p = (CLzmaEnc *)pp;
  UInt64 nowPos64;
  SRes res;
  CLzmaEnc_SeqOutStreamBuf outStream;

  outStream.vt.Write = SeqOutStreamBuf_Write;
  outStream.data = dest;
  outStream.rem = *destLen;
  outStream.overflow = False;

  p->writeEndMark = False;
  p->finished = False;
  p->result = SZ_OK;

  if (reInit)
    LzmaEnc_Init(p);
  LzmaEnc_InitPrices(p);

  nowPos64 = p->nowPos64;
  RangeEnc_Init(&p->rc);
  p->rc.outStream = &outStream.vt;

  if (desiredPackSize == 0)
    return SZ_ERROR_OUTPUT_EOF;

  res = LzmaEnc_CodeOneBlock(p, desiredPackSize, *unpackSize);
  
  *unpackSize = (UInt32)(p->nowPos64 - nowPos64);
  *destLen -= outStream.rem;
  if (outStream.overflow)
    return SZ_ERROR_OUTPUT_EOF;

  return res;
}


static SRes LzmaEnc_Encode2(CLzmaEnc *p, ICompressProgress *progress)
{
  SRes res = SZ_OK;

  #ifndef _7ZIP_ST
  Byte allocaDummy[0x300];
  allocaDummy[0] = 0;
  allocaDummy[1] = allocaDummy[0];
  #endif

  for (;;)
  {
    res = LzmaEnc_CodeOneBlock(p, 0, 0);
    if (res != SZ_OK || p->finished)
      break;
    if (progress)
    {
      res = ICompressProgress_Progress(progress, p->nowPos64, RangeEnc_GetProcessed(&p->rc));
      if (res != SZ_OK)
      {
        res = SZ_ERROR_PROGRESS;
        break;
      }
    }
  }
  
  LzmaEnc_Finish(p);

  /*
  if (res == SZ_OK && !Inline_MatchFinder_IsFinishedOK(&p->matchFinderBase))
    res = SZ_ERROR_FAIL;
  }
  */

  return res;
}


SRes LzmaEnc_Encode(CLzmaEncHandle pp, ISeqOutStream *outStream, ISeqInStream *inStream, ICompressProgress *progress,
    ISzAllocPtr alloc, ISzAllocPtr allocBig)
{
  RINOK(LzmaEnc_Prepare(pp, outStream, inStream, alloc, allocBig));
  return LzmaEnc_Encode2((CLzmaEnc *)pp, progress);
}


SRes LzmaEnc_WriteProperties(CLzmaEncHandle pp, Byte *props, SizeT *size)
{
  CLzmaEnc *p = (CLzmaEnc *)pp;
  unsigned i;
  UInt32 dictSize = p->dictSize;
  if (*size < LZMA_PROPS_SIZE)
    return SZ_ERROR_PARAM;
  *size = LZMA_PROPS_SIZE;
  props[0] = (Byte)((p->pb * 5 + p->lp) * 9 + p->lc);

  if (dictSize >= ((UInt32)1 << 22))
  {
    UInt32 kDictMask = ((UInt32)1 << 20) - 1;
    if (dictSize < (UInt32)0xFFFFFFFF - kDictMask)
      dictSize = (dictSize + kDictMask) & ~kDictMask;
  }
  else for (i = 11; i <= 30; i++)
  {
    if (dictSize <= ((UInt32)2 << i)) { dictSize = (2 << i); break; }
    if (dictSize <= ((UInt32)3 << i)) { dictSize = (3 << i); break; }
  }

  for (i = 0; i < 4; i++)
    props[1 + i] = (Byte)(dictSize >> (8 * i));
  return SZ_OK;
}


unsigned LzmaEnc_IsWriteEndMark(CLzmaEncHandle pp)
{
  return ((CLzmaEnc *)pp)->writeEndMark;
}


SRes LzmaEnc_MemEncode(CLzmaEncHandle pp, Byte *dest, SizeT *destLen, const Byte *src, SizeT srcLen,
    int writeEndMark, ICompressProgress *progress, ISzAllocPtr alloc, ISzAllocPtr allocBig)
{
  SRes res;
  CLzmaEnc *p = (CLzmaEnc *)pp;

  CLzmaEnc_SeqOutStreamBuf outStream;

  outStream.vt.Write = SeqOutStreamBuf_Write;
  outStream.data = dest;
  outStream.rem = *destLen;
  outStream.overflow = False;

  p->writeEndMark = writeEndMark;
  p->rc.outStream = &outStream.vt;

  res = LzmaEnc_MemPrepare(pp, src, srcLen, 0, alloc, allocBig);
  
  if (res == SZ_OK)
  {
    res = LzmaEnc_Encode2(p, progress);
    if (res == SZ_OK && p->nowPos64 != srcLen)
      res = SZ_ERROR_FAIL;
  }

  *destLen -= outStream.rem;
  if (outStream.overflow)
    return SZ_ERROR_OUTPUT_EOF;
  return res;
}


SRes LzmaEncode(Byte *dest, SizeT *destLen, const Byte *src, SizeT srcLen,
    const CLzmaEncProps *props, Byte *propsEncoded, SizeT *propsSize, int writeEndMark,
    ICompressProgress *progress, ISzAllocPtr alloc, ISzAllocPtr allocBig)
{
  CLzmaEnc *p = (CLzmaEnc *)LzmaEnc_Create(alloc);
  SRes res;
  if (!p)
    return SZ_ERROR_MEM;

  res = LzmaEnc_SetProps(p, props);
  if (res == SZ_OK)
  {
    res = LzmaEnc_WriteProperties(p, propsEncoded, propsSize);
    if (res == SZ_OK)
      res = LzmaEnc_MemEncode(p, dest, destLen, src, srcLen,
          writeEndMark, progress, alloc, allocBig);
  }

  LzmaEnc_Destroy(p, alloc, allocBig);
  return res;
}
>>>>>>> 18dc2b41... 18.05
