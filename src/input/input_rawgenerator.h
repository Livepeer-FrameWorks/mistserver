#pragma once
#include "input.h"

namespace Mist{

  class InputRawGenerator : public Input{
  public:
    InputRawGenerator(Util::Config *cfg);
    virtual bool needsLock(){return false;}
    virtual bool needHeader(){return false;}
    virtual void checkHeaderTimes(const HTTP::URL & streamFile){}

  protected:
    virtual void streamMainLoop();
    bool checkArguments(){return true;}
    bool readHeader();
    bool preRun(){return true;}
    void getNext(size_t idx = INVALID_TRACK_ID){};
    void seek(uint64_t seekTime, size_t idx = INVALID_TRACK_ID){}

    size_t vidIdx;
    uint64_t nextVid;
    size_t audIdx;
    size_t subIdx;
    uint64_t nextSub;
    size_t metaIdx;
    uint64_t bMs;
    Util::ResizeablePointer vidPtr;
  };

}// namespace Mist

typedef Mist::InputRawGenerator mistIn;
