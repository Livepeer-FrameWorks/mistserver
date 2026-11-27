#pragma once
#include "shared_memory.h"
#include "util.h"

#include <deque>
#include <map>
#include <memory>
#include <vector>

// needed for COMM_LOOP to work
#include "procs.h" // IWYU pragma: keep


#define COMM_LOOP(comm, onActive, onDisconnect) \
  {\
    size_t rc = comm.recordCount();\
    size_t maxRec = 0;\
    for (size_t id = 0; id < rc; ++id){\
      uint8_t s = comm.getStatus(id);\
      if (s == COMM_STATUS_INVALID){continue;}\
      if (!(s & COMM_STATUS_DISCONNECT) && comm.getPid(id) && !Util::Procs::isRunning(comm.getPid(id))){\
        comm.setStatus(COMM_STATUS_DISCONNECT | s, id);\
      }\
      onActive;\
      if (s & COMM_STATUS_DISCONNECT){\
        onDisconnect;\
        comm.setStatus(COMM_STATUS_INVALID, id);\
      }\
      maxRec = id;\
    }\
    if (maxRec+1 < rc){comm.setRecordCount(maxRec+1, rc);}\
  }

namespace Comms{
  extern uint8_t sessionViewerMode;
  extern uint8_t sessionInputMode;
  extern uint8_t sessionOutputMode;
  extern uint8_t sessionUnspecifiedMode;
  extern uint8_t sessionStreamInfoMode;
  extern uint8_t tknMode;
  extern uint8_t defaultCommFlags;
  void sessionConfigCache(uint64_t bootMs = 0);
  /// Returns the scheme://host[:port] a viewer request comes from: its Origin header, or the origin
  /// part of its Referer when Origin is empty or "null". Empty when neither names one.
  std::string viewerSessionOrigin(const std::string & origin, const std::string & referer);
  /// Returns the resolved stream of this viewer's live, admitted decision,
  /// or an empty string. Does not create sessions during a handler outage.
  std::string admittedViewerStream(const std::string & streamName, const std::string & ip, const std::string & token,
                                   const std::string & protocol, const std::string & origin, const std::string & referer);
  /// Builds the USER_NEW trigger payload. Lines 8 and 9 are the viewer request's Origin and Referer,
  /// written even when empty, and the payload ends with a newline so empty trailing lines survive.
  std::string userNewPayload(const std::string & streamName, const std::string & host, const std::string & token,
                             const std::string & protocol, const std::string & reqUrl, const std::string & sessionId,
                             bool validToken, const std::string & origin, const std::string & referer);

  /// Whether a connection without a session must stop. A session that exists but refused the
  /// connection (USER_NEW) always ends it, and so does any session problem for a viewer. An
  /// internal process reader whose session process could not be started only lacks accounting,
  /// so it keeps running and retries on its next stats update.
  inline bool missingSessionEndsConnection(bool processReader, bool sessionStartFailed) {
    return !processReader || !sessionStartFailed;
  }

  /// How long a connection waits for its session process to create the session semaphore,
  /// unless the process is stopping.
  const uint64_t SESSION_SEMAPHORE_WAIT_MS = 45000;

  /// Returns a 64-bit hash of the field table (names, types and sizes, in order) of the RelAccX
  /// structure at `page`, or 0 when `len` bytes cannot hold a valid field table.
  uint64_t layoutFingerprint(const char *page, size_t len);

  /// The statistics index (SHM_STATS_INDEX) lists the statistics segments. A segment is a
  /// Sessions page whose layout never changes after it is ready, so a build with another layout
  /// creates a new segment instead of changing a live one. Every release reads this format, so it
  /// never changes:
  ///   offset  0  uint32  magic, STATS_INDEX_MAGIC
  ///   offset  4  uint32  format version, STATS_INDEX_VERSION
  ///   offset  8  uint64  generation: odd while a writer changes the index, even otherwise
  ///   offset 16  uint32  entry count
  ///   offset 20  uint32  entry size in bytes, at least STATS_INDEX_ENTRY_SIZE
  ///   offset 24  40 bytes reserved, zero
  ///   offset 64  entry count entries of entry size bytes each:
  ///     +0   char[64] segment page name, NUL-terminated, starting with COMMS_STATISTICS
  ///     +64  uint32   state: STATS_SEGMENT_CURRENT or STATS_SEGMENT_RETIRING
  ///     +68  uint32   reserved, zero
  ///     +72  uint64   layout fingerprint of the segment (layoutFingerprint)
  /// Integers are in host byte order. Writers hold SEM_STATISTICS and raise the generation by one
  /// before and by one after changing anything. Readers copy the index between two reads of the
  /// generation and retry until both reads are equal and even. Readers ignore entry bytes beyond
  /// the fields above and skip entries they cannot validate.
  const uint32_t STATS_INDEX_MAGIC = 0x5849534D;
  const uint32_t STATS_INDEX_VERSION = 1;
  const size_t STATS_INDEX_HEADER_SIZE = 64;
  const size_t STATS_INDEX_ENTRY_SIZE = 80;
  const size_t STATS_INDEX_NAME_SIZE = 64;
  const uint32_t STATS_SEGMENT_CURRENT = 1;
  const uint32_t STATS_SEGMENT_RETIRING = 2;

  struct StatsSegment {
      std::string name;
      uint32_t state = 0;
      uint64_t fingerprint = 0;
  };

  struct StatsIndex {
      uint64_t generation = 0;
      std::vector<StatsSegment> segments;
      /// The first current segment, or null.
      const StatsSegment *current() const;
  };

  /// Copies a consistent snapshot of the index at `page` into `out`. Returns false when the page
  /// holds no valid index or stays mid-write for all `attempts` tries.
  bool readStatsIndex(const char *page, size_t len, StatsIndex & out, size_t attempts = 400);
  /// Reads the index page SHM_STATS_INDEX; false when it does not exist or is invalid.
  bool readStatsIndex(StatsIndex & out);
  /// Replaces the entries of the index at `page`. The caller holds SEM_STATISTICS. Returns false,
  /// leaving the index unchanged, when an entry is invalid or the entries do not fit.
  bool writeStatsIndex(char *page, size_t len, const std::vector<StatsSegment> & segments);
  /// Whether `name` may name a statistics segment.
  bool validStatsSegmentName(const std::string & name);

  /// A field of a controller state page.
  struct StateField {
      const char *name;
      uint8_t type;
      uint32_t size;
  };

  /// Maps the state page `name` with the layout `fields` into `page` and `rax`, creating it when
  /// absent. A ready page whose field table equals that layout is reused as is. Any other page is
  /// flagged for reload and unlinked, and its newest records that fit `capacityBytes` are copied
  /// into a new page for every field both layouts share. Closing `page` never unlinks it. Returns
  /// false when no page could be mapped.
  bool openStatePage(IPC::sharedPage & page, Util::RelAccX & rax, const std::string & name, size_t size,
                     size_t capacityBytes, const std::vector<StateField> & fields);

  class Comms{
  public:
    Comms();
    virtual ~Comms();
    operator bool() const;
    void reload(const std::string & prefix, size_t baseSize, bool _master = false, bool reIssue = false, bool waitForPage = true);
    virtual void addFields();
    virtual void nullFields();
    virtual void fieldAccess();
    size_t recordCount() const;
    void setRecordCount(size_t idx, size_t prevCount);
    uint8_t getStatus() const;
    uint8_t getStatus(size_t idx) const;
    void setStatus(uint8_t _status);
    void setStatus(uint8_t _status, size_t idx);
    uint64_t getCommand() const;
    uint64_t getCommand(size_t idx) const;
    void setCommand(uint64_t _cmd);
    void setCommand(uint64_t _cmd, size_t idx);
    uint32_t getPid() const;
    uint32_t getPid(size_t idx) const;
    void setPid(uint32_t _pid);
    void setPid(uint32_t _pid, size_t idx);
    void finishAll();
    void setMaster(bool _master);
    bool registrationClosed() const { return closedRegistration; }
    const std::string &pageName() const{return dataPage.name;}

  protected:
    bool master;
    bool closedRegistration;
    uint64_t index;
    size_t currentSize;
    IPC::semaphore sem;
    IPC::sharedPage dataPage;
    Util::RelAccX dataAccX;
    Util::FieldAccX status;
    Util::FieldAccX pid;
  };

  class Connections : public Comms{
  public:
    void reload(const std::string & streamName, const std::string & ip, const std::string & tkn,
                const std::string & protocol, const std::string & reqUrl, bool _master = false, bool reIssue = false,
                const std::string & origin = "",
                const std::string & referer = "", const std::string & requestedStream = "");
    void reload(const std::string & sessId, bool _master = false, bool reIssue = false);
    void unload();
    operator bool() const{return dataPage.mapped && (master || index != INVALID_RECORD_INDEX);}
    std::string generateSession(const std::string & streamName, const std::string & ip, const std::string & tkn,
                                const std::string & connector, uint64_t sessionMode, const std::string & viewerOrigin = "");
    std::string sessionId;
    std::string initialTkn;
    /// Whether the last reload could not start the session process, or stopped waiting for it
    /// because this process is stopping, as opposed to a session that exists but refused this
    /// connection.
    bool sessionStartFailed() const { return startFailed; }

    void setExit();
    bool getExit();
    
    virtual void addFields();
    virtual void nullFields();
    virtual void fieldAccess();

    const std::string & getTkn() const{return initialTkn;}

    uint64_t getNow() const;
    uint64_t getNow(size_t idx) const;
    void setNow(uint64_t _now);
    void setNow(uint64_t _now, size_t idx);

    uint64_t getTime() const;
    uint64_t getTime(size_t idx) const;
    void setTime(uint64_t _time);
    void setTime(uint64_t _time, size_t idx);

    uint64_t getLastSecond() const;
    uint64_t getLastSecond(size_t idx) const;
    void setLastSecond(uint64_t _lastSecond);
    void setLastSecond(uint64_t _lastSecond, size_t idx);

    uint64_t getDown() const;
    uint64_t getDown(size_t idx) const;
    void setDown(uint64_t _down);
    void setDown(uint64_t _down, size_t idx);

    uint64_t getUp() const;
    uint64_t getUp(size_t idx) const;
    void setUp(uint64_t _up);
    void setUp(uint64_t _up, size_t idx);

    std::string getHost() const;
    std::string getHost(size_t idx) const;
    void setHost(std::string _host);
    void setHost(std::string _host, size_t idx);

    std::string getStream() const;
    std::string getStream(size_t idx) const;
    void setStream(std::string _stream);
    void setStream(std::string _stream, size_t idx);

    std::string getConnector() const;
    std::string getConnector(size_t idx) const;
    void setConnector(std::string _connector);
    void setConnector(std::string _connector, size_t idx);
    bool hasConnector(size_t idx, std::string protocol);

    uint64_t getPacketCount() const;
    uint64_t getPacketCount(size_t idx) const;
    void setPacketCount(uint64_t _count);
    void setPacketCount(uint64_t _count, size_t idx);

    uint64_t getPacketLostCount() const;
    uint64_t getPacketLostCount(size_t idx) const;
    void setPacketLostCount(uint64_t _lost);
    void setPacketLostCount(uint64_t _lost, size_t idx);

    uint64_t getPacketRetransmitCount() const;
    uint64_t getPacketRetransmitCount(size_t idx) const;
    void setPacketRetransmitCount(uint64_t _retransmit);
    void setPacketRetransmitCount(uint64_t _retransmit, size_t idx);

  protected:
    Util::FieldAccX now;
    Util::FieldAccX time;
    Util::FieldAccX lastSecond;
    Util::FieldAccX down;
    Util::FieldAccX up;
    Util::FieldAccX host;
    Util::FieldAccX stream;
    Util::FieldAccX connector;
    Util::FieldAccX sessId;
    Util::FieldAccX tags;
    Util::FieldAccX pktcount;
    Util::FieldAccX pktloss;
    Util::FieldAccX pktretrans;
    bool startFailed = false;
  };

  class Users : public Comms{
  public:
    Users();
    ~Users();
    Users(const Users &rhs);
    void reload(const std::string &_streamName = "", bool _master = false, bool reIssue = false);
    void reloadReadonly(const std::string &_streamName);
    void reload(const std::string &_streamName, size_t track, uint8_t initialState = COMM_STATUS_ACTIVE);
    bool beginIdleShutdown(char *streamState);
    virtual void addFields();
    virtual void nullFields();
    virtual void fieldAccess();

    operator bool() const{return dataPage.mapped;}

    uint32_t getTrack() const;
    uint32_t getTrack(size_t idx) const;
    void setTrack(uint32_t _track);
    void setTrack(uint32_t _track, size_t idx);

    size_t getKeyNum() const;
    size_t getKeyNum(size_t idx) const;
    void setKeyNum(size_t _keyNum);
    void setKeyNum(size_t _keyNum, size_t idx);

  private:
    std::string streamName;
    size_t requestedTrack = 0;

    Util::FieldAccX track;
    Util::FieldAccX keyNum;
    bool noMaster;
  };

  class Sessions : public Connections{
    public:
      Sessions();
      ~Sessions();
      /// As a client, registers on the current statistics segment the index names, or on the legacy
      /// page COMMS_STATISTICS when there is no index. As master, attaches the current segment when
      /// its layout is this build's, and otherwise creates a segment with this build's layout,
      /// publishes it as current and lists the previous current segment and the legacy page as
      /// retiring.
      void reload(bool _master = false, bool reIssue = false);
      /// Attaches the existing segment `name` as master without creating it.
      bool attachSegment(const std::string & name);
      /// Unlinks this master's segment and closes it, leaving the statistics semaphore in place.
      void dropSegment();
      /// The layout fingerprint of this build's statistics records.
      static uint64_t buildFingerprint();
      std::string getRequestedStream(size_t idx) const;
      std::string getSessId() const;
      std::string getSessId(size_t idx) const;
      void setSessId(std::string _sid);
      void setSessId(std::string _sid, size_t idx);
      bool sessIdExists(std::string _sid);
      virtual void addFields();
      virtual void nullFields();
      virtual void fieldAccess();

      void setRequestedStream(const std::string & name);

      std::string getTags() const;
      std::string getTags(size_t idx) const;
      void setTags(std::string _sid);
      void setTags(std::string _sid, size_t idx);
    private:
      Util::FieldAccX requestedStream;
  };

  /// The controller's statistics segments: the current one first, then every retiring one.
  class StatisticsSegments {
    public:
      StatisticsSegments();
      ~StatisticsSegments();
      /// Attaches or creates the current segment, then attaches every retiring segment the index
      /// lists. Retiring segments that no longer exist leave the index.
      void start();
      /// On a rolling restart keeps every segment and the index for the next controller. Otherwise
      /// unlinks the index, optionally stops all registered clients, and leaves the segments to be
      /// unlinked when they close.
      void stop(bool restarting, bool killClients);
      size_t size() const { return segments.size(); }
      Sessions & operator[](size_t i) { return *segments[i]; }
      /// Closes registration on a retiring segment once it has no live records, and unlinks it
      /// `graceMs` later.
      void retire(uint64_t nowMs);
      /// Whether any segment has a live record for this session id.
      bool sessIdExists(const std::string & sessId);
      uint64_t graceMs = 60000;

    private:
      void unlist(const std::string & name);
      std::deque<std::unique_ptr<Sessions>> segments;
      std::map<std::string, uint64_t> closedAt;
      IPC::semaphore sem;
  };
}// namespace Comms
