<<<<<<< HEAD
// Lzma2Decoder.cpp

#include "StdAfx.h"

#include "../../../C/Alloc.h"

#include "../Common/StreamUtils.h"

#include "Lzma2Decoder.h"

static HRESULT SResToHRESULT(SRes res)
{
  switch (res)
  {
    case SZ_OK: return S_OK;
    case SZ_ERROR_MEM: return E_OUTOFMEMORY;
    case SZ_ERROR_PARAM: return E_INVALIDARG;
    case SZ_ERROR_UNSUPPORTED: return E_NOTIMPL;
    case SZ_ERROR_DATA: return S_FALSE;
  }
  return E_FAIL;
}

namespace NCompress {
namespace NLzma2 {

CDecoder::CDecoder():
    _inBuf(NULL),
    _finishMode(false),
    _outSizeDefined(false),
    _outStep(1 << 22),
    _inBufSize(0),
    _inBufSizeNew(1 << 20)
{
  Lzma2Dec_Construct(&_state);
}

CDecoder::~CDecoder()
{
  Lzma2Dec_Free(&_state, &g_Alloc);
  MidFree(_inBuf);
}

STDMETHODIMP CDecoder::SetInBufSize(UInt32 , UInt32 size) { _inBufSizeNew = size; return S_OK; }
STDMETHODIMP CDecoder::SetOutBufSize(UInt32 , UInt32 size) { _outStep = size; return S_OK; }

STDMETHODIMP CDecoder::SetDecoderProperties2(const Byte *prop, UInt32 size)
{
  if (size != 1)
    return E_NOTIMPL;
  
  RINOK(SResToHRESULT(Lzma2Dec_Allocate(&_state, prop[0], &g_Alloc)));
  
  if (!_inBuf || _inBufSize != _inBufSizeNew)
  {
    MidFree(_inBuf);
    _inBufSize = 0;
    _inBuf = (Byte *)MidAlloc(_inBufSizeNew);
    if (!_inBuf)
      return E_OUTOFMEMORY;
    _inBufSize = _inBufSizeNew;
  }

  return S_OK;
}


STDMETHODIMP CDecoder::SetOutStreamSize(const UInt64 *outSize)
{
  _outSizeDefined = (outSize != NULL);
  _outSize = 0;
  if (_outSizeDefined)
    _outSize = *outSize;
  _inPos = _inLim = 0;
  _inProcessed = 0;
  _outProcessed = 0;

  Lzma2Dec_Init(&_state);

  return S_OK;
}


STDMETHODIMP CDecoder::SetFinishMode(UInt32 finishMode)
{
  _finishMode = (finishMode != 0);
  return S_OK;
}


STDMETHODIMP CDecoder::GetInStreamProcessedSize(UInt64 *value)
{
  *value = _inProcessed;
  return S_OK;
}


STDMETHODIMP CDecoder::Code(ISequentialInStream *inStream, ISequentialOutStream *outStream,
    const UInt64 *inSize, const UInt64 *outSize, ICompressProgressInfo *progress)
{
  if (!_inBuf)
    return S_FALSE;

  SetOutStreamSize(outSize);

  SizeT wrPos = _state.decoder.dicPos;
  HRESULT readRes = S_OK;

  for (;;)
  {
    if (_inPos == _inLim && readRes == S_OK)
    {
      _inPos = _inLim = 0;
      readRes = inStream->Read(_inBuf, _inBufSize, &_inLim);
    }

    const SizeT dicPos = _state.decoder.dicPos;
    SizeT size;
    {
      SizeT next = _state.decoder.dicBufSize;
      if (next - wrPos > _outStep)
        next = wrPos + _outStep;
      size = next - dicPos;
    }

    ELzmaFinishMode finishMode = LZMA_FINISH_ANY;
    if (_outSizeDefined)
    {
      const UInt64 rem = _outSize - _outProcessed;
      if (size >= rem)
      {
        size = (SizeT)rem;
        if (_finishMode)
          finishMode = LZMA_FINISH_END;
      }
    }

    SizeT inProcessed = _inLim - _inPos;
    ELzmaStatus status;
    
    SRes res = Lzma2Dec_DecodeToDic(&_state, dicPos + size, _inBuf + _inPos, &inProcessed, finishMode, &status);

    
    _inPos += (UInt32)inProcessed;
    _inProcessed += inProcessed;
    const SizeT outProcessed = _state.decoder.dicPos - dicPos;
    _outProcessed += outProcessed;

    
    bool outFinished = (_outSizeDefined && _outProcessed >= _outSize);

    bool needStop = (res != 0
        || (inProcessed == 0 && outProcessed == 0)
        || status == LZMA_STATUS_FINISHED_WITH_MARK
        || (!_finishMode && outFinished));

    if (needStop || outProcessed >= size)
    {
      HRESULT res2 = WriteStream(outStream, _state.decoder.dic + wrPos, _state.decoder.dicPos - wrPos);

      if (_state.decoder.dicPos == _state.decoder.dicBufSize)
        _state.decoder.dicPos = 0;
      wrPos = _state.decoder.dicPos;

      RINOK(res2);

      if (needStop)
      {
        if (res != 0)
          return S_FALSE;

        if (status == LZMA_STATUS_FINISHED_WITH_MARK)
        {
          if (_finishMode)
          {
            if (inSize && *inSize != _inProcessed)
              return S_FALSE;
            if (_outSizeDefined && _outSize != _outProcessed)
              return S_FALSE;
          }
          return readRes;
        }

        if (!_finishMode && outFinished)
          return readRes;
   
        return S_FALSE;
      }
    }
    
    if (progress)
    {
      RINOK(progress->SetRatioInfo(&_inProcessed, &_outProcessed));
    }
  }
}


#ifndef NO_READ_FROM_CODER

STDMETHODIMP CDecoder::SetInStream(ISequentialInStream *inStream) { _inStream = inStream; return S_OK; }
STDMETHODIMP CDecoder::ReleaseInStream() { _inStream.Release(); return S_OK; }
  

STDMETHODIMP CDecoder::Read(void *data, UInt32 size, UInt32 *processedSize)
{
  if (processedSize)
    *processedSize = 0;

  ELzmaFinishMode finishMode = LZMA_FINISH_ANY;
  if (_outSizeDefined)
  {
    const UInt64 rem = _outSize - _outProcessed;
    if (size >= rem)
    {
      size = (UInt32)rem;
      if (_finishMode)
        finishMode = LZMA_FINISH_END;
    }
  }

  HRESULT readRes = S_OK;

  for (;;)
  {
    if (_inPos == _inLim && readRes == S_OK)
    {
      _inPos = _inLim = 0;
      readRes = _inStream->Read(_inBuf, _inBufSize, &_inLim);
    }
    
    SizeT inProcessed = _inLim - _inPos;
    SizeT outProcessed = size;
    ELzmaStatus status;

    SRes res = Lzma2Dec_DecodeToBuf(&_state, (Byte *)data, &outProcessed,
        _inBuf + _inPos, &inProcessed, finishMode, &status);
    
    
    _inPos += (UInt32)inProcessed;
    _inProcessed += inProcessed;
    _outProcessed += outProcessed;
    size -= (UInt32)outProcessed;
    data = (Byte *)data + outProcessed;
    if (processedSize)
      *processedSize += (UInt32)outProcessed;
    
    if (res != 0)
      return S_FALSE;
    
    /*
    if (status == LZMA_STATUS_FINISHED_WITH_MARK)
      return readRes;

    if (size == 0 && status != LZMA_STATUS_NEEDS_MORE_INPUT)
    {
      if (_finishMode && _outSizeDefined && _outProcessed >= _outSize)
        return S_FALSE;
      return readRes;
    }
    */

    if (inProcessed == 0 && outProcessed == 0)
      return readRes;
  }
}

#endif

}}
=======
// Lzma2Decoder.cpp

#include "StdAfx.h"

// #include <stdio.h>

#include "../../../C/Alloc.h"
// #include "../../../C/CpuTicks.h"

#include "../Common/StreamUtils.h"

#include "Lzma2Decoder.h"

namespace NCompress {
namespace NLzma2 {

CDecoder::CDecoder():
      _dec(NULL)
    , _inProcessed(0)
    , _prop(0xFF)
    , _finishMode(false)
    , _inBufSize(1 << 20)
    , _outStep(1 << 20)
    #ifndef _7ZIP_ST
    , _tryMt(1)
    , _numThreads(1)
    , _memUsage((UInt64)(sizeof(size_t)) << 28)
    #endif
{}

CDecoder::~CDecoder()
{
  if (_dec)
    Lzma2DecMt_Destroy(_dec);
}

STDMETHODIMP CDecoder::SetInBufSize(UInt32 , UInt32 size) { _inBufSize = size; return S_OK; }
STDMETHODIMP CDecoder::SetOutBufSize(UInt32 , UInt32 size) { _outStep = size; return S_OK; }

STDMETHODIMP CDecoder::SetDecoderProperties2(const Byte *prop, UInt32 size)
{
  if (size != 1)
    return E_NOTIMPL;
  if (prop[0] > 40)
    return E_NOTIMPL;
  _prop = prop[0];
  return S_OK;
}


STDMETHODIMP CDecoder::SetFinishMode(UInt32 finishMode)
{
  _finishMode = (finishMode != 0);
  return S_OK;
}



#ifndef _7ZIP_ST

static UInt64 Get_ExpectedBlockSize_From_Dict(UInt32 dictSize)
{
  const UInt32 kMinSize = (UInt32)1 << 20;
  const UInt32 kMaxSize = (UInt32)1 << 28;
  UInt64 blockSize = (UInt64)dictSize << 2;
  if (blockSize < kMinSize) blockSize = kMinSize;
  if (blockSize > kMaxSize) blockSize = kMaxSize;
  if (blockSize < dictSize) blockSize = dictSize;
  blockSize += (kMinSize - 1);
  blockSize &= ~(UInt64)(kMinSize - 1);
  return blockSize;
}

#define LZMA2_DIC_SIZE_FROM_PROP_FULL(p) ((p) == 40 ? 0xFFFFFFFF : (((UInt32)2 | ((p) & 1)) << ((p) / 2 + 11)))

#endif

#define RET_IF_WRAP_ERROR_CONFIRMED(wrapRes, sRes, sResErrorCode) \
  if (wrapRes != S_OK && sRes == sResErrorCode) return wrapRes;

#define RET_IF_WRAP_ERROR(wrapRes, sRes, sResErrorCode) \
  if (wrapRes != S_OK /* && (sRes == SZ_OK || sRes == sResErrorCode) */) return wrapRes;

STDMETHODIMP CDecoder::Code(ISequentialInStream *inStream, ISequentialOutStream *outStream,
    const UInt64 *inSize, const UInt64 *outSize, ICompressProgressInfo *progress)
{
  _inProcessed = 0;

  if (!_dec)
  {
    _dec = Lzma2DecMt_Create(
      // &g_AlignedAlloc,
      &g_Alloc,
      &g_MidAlloc);
    if (!_dec)
      return E_OUTOFMEMORY;
  }

  CLzma2DecMtProps props;
  Lzma2DecMtProps_Init(&props);

  props.inBufSize_ST = _inBufSize;
  props.outStep_ST = _outStep;

  #ifndef _7ZIP_ST
  {
    props.numThreads = 1;
    UInt32 numThreads = _numThreads;

    if (_tryMt && numThreads >= 1)
    {
      UInt64 useLimit = _memUsage;
      UInt32 dictSize = LZMA2_DIC_SIZE_FROM_PROP_FULL(_prop);
      UInt64 expectedBlockSize64 = Get_ExpectedBlockSize_From_Dict(dictSize);
      size_t expectedBlockSize = (size_t)expectedBlockSize64;
      size_t inBlockMax = expectedBlockSize + expectedBlockSize / 16;
      if (expectedBlockSize == expectedBlockSize64 && inBlockMax >= expectedBlockSize)
      {
        props.outBlockMax = expectedBlockSize;
        props.inBlockMax = inBlockMax;
        const size_t kOverheadSize = props.inBufSize_MT + (1 << 16);
        UInt64 okThreads = useLimit / (props.outBlockMax + props.inBlockMax + kOverheadSize);
        if (numThreads > okThreads)
          numThreads = (UInt32)okThreads;
        if (numThreads == 0)
          numThreads = 1;
        props.numThreads = numThreads;
      }
    }
  }
  #endif

  CSeqInStreamWrap inWrap;
  CSeqOutStreamWrap outWrap;
  CCompressProgressWrap progressWrap;

  inWrap.Init(inStream);
  outWrap.Init(outStream);
  progressWrap.Init(progress);

  SRes res;

  UInt64 inProcessed = 0;
  int isMT = False;

  #ifndef _7ZIP_ST
  isMT = _tryMt;
  #endif

  // UInt64 cpuTicks = GetCpuTicks();

  res = Lzma2DecMt_Decode(_dec, _prop, &props,
      &outWrap.vt, outSize, _finishMode,
      &inWrap.vt,
      &inProcessed,
      &isMT,
      progress ? &progressWrap.vt : NULL);

  /*
  cpuTicks = GetCpuTicks() - cpuTicks;
  printf("\n             ticks = %10I64u\n", cpuTicks / 1000000);
  */


  #ifndef _7ZIP_ST
  /* we reset _tryMt, only if p->props.numThreads was changed */
  if (props.numThreads > 1)
    _tryMt = isMT;
  #endif

  _inProcessed = inProcessed;

  RET_IF_WRAP_ERROR(progressWrap.Res, res, SZ_ERROR_PROGRESS)
  RET_IF_WRAP_ERROR(outWrap.Res, res, SZ_ERROR_WRITE)
  RET_IF_WRAP_ERROR_CONFIRMED(inWrap.Res, res, SZ_ERROR_READ)

  if (res == SZ_OK && _finishMode)
  {
    if (inSize && *inSize != inProcessed)
      res = SZ_ERROR_DATA;
    if (outSize && *outSize != outWrap.Processed)
      res = SZ_ERROR_DATA;
  }

  return SResToHRESULT(res);
}


STDMETHODIMP CDecoder::GetInStreamProcessedSize(UInt64 *value)
{
  *value = _inProcessed;
  return S_OK;
}


#ifndef _7ZIP_ST

STDMETHODIMP CDecoder::SetNumberOfThreads(UInt32 numThreads)
{
  _numThreads = numThreads;
  return S_OK;
}

STDMETHODIMP CDecoder::SetMemLimit(UInt64 memUsage)
{
  _memUsage = memUsage;
  return S_OK;
}

#endif


#ifndef NO_READ_FROM_CODER

STDMETHODIMP CDecoder::SetOutStreamSize(const UInt64 *outSize)
{
  CLzma2DecMtProps props;
  Lzma2DecMtProps_Init(&props);
  props.inBufSize_ST = _inBufSize;
  props.outStep_ST = _outStep;

  _inProcessed = 0;

  if (!_dec)
  {
    _dec = Lzma2DecMt_Create(&g_AlignedAlloc, &g_MidAlloc);
    if (!_dec)
      return E_OUTOFMEMORY;
  }

  _inWrap.Init(_inStream);

  SRes res = Lzma2DecMt_Init(_dec, _prop, &props, outSize, _finishMode, &_inWrap.vt);

  if (res != SZ_OK)
    return SResToHRESULT(res);
  return S_OK;
}


STDMETHODIMP CDecoder::SetInStream(ISequentialInStream *inStream) { _inStream = inStream; return S_OK; }
STDMETHODIMP CDecoder::ReleaseInStream() { _inStream.Release(); return S_OK; }
  

STDMETHODIMP CDecoder::Read(void *data, UInt32 size, UInt32 *processedSize)
{
  if (processedSize)
    *processedSize = 0;

  size_t size2 = size;
  UInt64 inProcessed = 0;

  SRes res = Lzma2DecMt_Read(_dec, (Byte *)data, &size2, &inProcessed);

  _inProcessed += inProcessed;
  if (processedSize)
    *processedSize = (UInt32)size2;
  if (res != SZ_OK)
    return SResToHRESULT(res);
  return S_OK;
}

#endif

}}
>>>>>>> f19b649c... 18.03
