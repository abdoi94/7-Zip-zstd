<<<<<<< HEAD
// XzDecoder.h

#ifndef __XZ_DECODER_H
#define __XZ_DECODER_H

#include "../../../C/Xz.h"

#include "../../Common/MyCom.h"

#include "../ICoder.h"

namespace NCompress {
namespace NXz {

struct CXzUnpackerCPP
{
  Byte *InBuf;
  Byte *OutBuf;
  CXzUnpacker p;
  
  CXzUnpackerCPP();
  ~CXzUnpackerCPP();
};


struct CStatInfo
{
  UInt64 InSize;
  UInt64 OutSize;
  UInt64 PhySize;

  UInt64 NumStreams;
  UInt64 NumBlocks;

  bool UnpackSize_Defined;

  bool NumStreams_Defined;
  bool NumBlocks_Defined;

  bool IsArc;
  bool UnexpectedEnd;
  bool DataAfterEnd;
  bool Unsupported;
  bool HeadersError;
  bool DataError;
  bool CrcError;

  CStatInfo() { Clear(); }

  void Clear();
};


struct CDecoder: public CStatInfo
{
  CXzUnpackerCPP xzu;
  SRes DecodeRes; // it's not HRESULT

  CDecoder(): DecodeRes(SZ_OK) {}

  /* Decode() can return ERROR code only if there is progress or stream error.
     Decode() returns S_OK in case of xz decoding error, but DecodeRes and CStatInfo contain error information */
  HRESULT Decode(ISequentialInStream *seqInStream, ISequentialOutStream *outStream,
      const UInt64 *outSizeLimit, bool finishStream, ICompressProgressInfo *compressProgress);
  Int32 Get_Extract_OperationResult() const;
};


class CComDecoder:
  public ICompressCoder,
  public ICompressSetFinishMode,
  public ICompressGetInStreamProcessedSize,
  public CMyUnknownImp
{
  CDecoder _decoder;
  bool _finishStream;

public:
  MY_UNKNOWN_IMP2(
      ICompressSetFinishMode,
      ICompressGetInStreamProcessedSize)
  
  STDMETHOD(Code)(ISequentialInStream *inStream, ISequentialOutStream *outStream,
      const UInt64 *inSize, const UInt64 *outSize, ICompressProgressInfo *progress);
  STDMETHOD(SetFinishMode)(UInt32 finishMode);
  STDMETHOD(GetInStreamProcessedSize)(UInt64 *value);

  CComDecoder(): _finishStream(false) {}
};

}}

#endif
=======
// XzDecoder.h

#ifndef __XZ_DECODER_H
#define __XZ_DECODER_H

#include "../../../C/Xz.h"

#include "../../Common/MyCom.h"

#include "../ICoder.h"

namespace NCompress {
namespace NXz {

struct CDecoder
{
  CXzDecMtHandle xz;
  int _tryMt;
  UInt32 _numThreads;
  UInt64 _memUsage;

  SRes MainDecodeSRes; // it's not HRESULT
  bool MainDecodeSRes_wasUsed;
  CXzStatInfo Stat;

  CDecoder():
      xz(NULL),
      _tryMt(True),
      _numThreads(1),
      _memUsage((UInt64)(sizeof(size_t)) << 28),
      MainDecodeSRes(SZ_OK),
      MainDecodeSRes_wasUsed(false)
    {}
  
  ~CDecoder()
  {
    if (xz)
      XzDecMt_Destroy(xz);
  }

  /* Decode() can return ERROR code only if there is progress or stream error.
     Decode() returns S_OK in case of xz decoding error, but DecodeRes and CStatInfo contain error information */
  HRESULT Decode(ISequentialInStream *seqInStream, ISequentialOutStream *outStream,
      const UInt64 *outSizeLimit, bool finishStream, ICompressProgressInfo *compressProgress);
};


class CComDecoder:
  public ICompressCoder,
  public ICompressSetFinishMode,
  public ICompressGetInStreamProcessedSize,

  #ifndef _7ZIP_ST
  public ICompressSetCoderMt,
  public ICompressSetMemLimit,
  #endif

  public CMyUnknownImp,
  public CDecoder
{
  bool _finishStream;

public:
  MY_QUERYINTERFACE_BEGIN2(ICompressCoder)

  MY_QUERYINTERFACE_ENTRY(ICompressSetFinishMode)
  MY_QUERYINTERFACE_ENTRY(ICompressGetInStreamProcessedSize)

  #ifndef _7ZIP_ST
  MY_QUERYINTERFACE_ENTRY(ICompressSetCoderMt)
  MY_QUERYINTERFACE_ENTRY(ICompressSetMemLimit)
  #endif
  
  MY_QUERYINTERFACE_END
  MY_ADDREF_RELEASE

  STDMETHOD(Code)(ISequentialInStream *inStream, ISequentialOutStream *outStream,
      const UInt64 *inSize, const UInt64 *outSize, ICompressProgressInfo *progress);
  STDMETHOD(SetFinishMode)(UInt32 finishMode);
  STDMETHOD(GetInStreamProcessedSize)(UInt64 *value);

  #ifndef _7ZIP_ST
  STDMETHOD(SetNumberOfThreads)(UInt32 numThreads);
  STDMETHOD(SetMemLimit)(UInt64 memUsage);
  #endif

  CComDecoder(): _finishStream(false) {}
};

}}

#endif
>>>>>>> f19b649c... 18.03
