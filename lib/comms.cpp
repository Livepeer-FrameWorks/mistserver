#include "comms.h"

#include "auth.h"
#include "config.h"
#include "defines.h"
#include "procs.h"
#include "stream.h"
#include "timing.h"

#include <ctype.h>
#include <fcntl.h>
#include <sched.h>
#include <sstream>
#include <string.h>

namespace Comms{
  uint8_t sessionViewerMode = SESS_BUNDLE_DEFAULT_VIEWER;
  uint8_t sessionInputMode = SESS_BUNDLE_DEFAULT_OTHER;
  uint8_t sessionOutputMode = SESS_BUNDLE_DEFAULT_OTHER;
  uint8_t sessionUnspecifiedMode = 0;
  uint8_t sessionStreamInfoMode = SESS_DEFAULT_STREAM_INFO_MODE;
  uint8_t tknMode = SESS_TKN_DEFAULT_MODE;
  uint8_t defaultCommFlags = 0;

  /// \brief Refreshes the session configuration if the last update was more than 5 seconds ago
  void sessionConfigCache(uint64_t bootMs){
    static uint64_t lastUpdate = 0;
    if (!bootMs){bootMs = Util::bootMS();}
    if (bootMs > lastUpdate + 5000){
      VERYHIGH_MSG("Updating session config");
      JSON::Value tmpVal = Util::getGlobalConfig("sessionViewerMode");
      if (!tmpVal.isNull()){ sessionViewerMode = tmpVal.asInt(); }
      tmpVal = Util::getGlobalConfig("sessionInputMode");
      if (!tmpVal.isNull()){ sessionInputMode = tmpVal.asInt(); }
      tmpVal = Util::getGlobalConfig("sessionOutputMode");
      if (!tmpVal.isNull()){ sessionOutputMode = tmpVal.asInt(); }
      tmpVal = Util::getGlobalConfig("sessionUnspecifiedMode");
      if (!tmpVal.isNull()){ sessionUnspecifiedMode = tmpVal.asInt(); }
      tmpVal = Util::getGlobalConfig("sessionStreamInfoMode");
      if (!tmpVal.isNull()){ sessionStreamInfoMode = tmpVal.asInt(); }
      tmpVal = Util::getGlobalConfig("tknMode");
      if (!tmpVal.isNull()){ tknMode = tmpVal.asInt(); }
      lastUpdate = bootMs;
    }
  }

  Comms::Comms(){
    index = INVALID_RECORD_INDEX;
    currentSize = 0;
    master = false;
    closedRegistration = false;
  }

  Comms::~Comms(){
    if (dataPage && index != INVALID_RECORD_INDEX && status){
      setStatus(COMM_STATUS_DISCONNECT | getStatus());
    }
    if (master){
      if (dataPage.mapped){
        finishAll();
        dataPage.master = true;
      }
      sem.unlink();
    }
    sem.close();
  }

  void Comms::addFields(){
    dataAccX.addField("status", RAX_UINT);
    dataAccX.addField("pid", RAX_64UINT);
  }

  void Comms::nullFields(){
    setPid(getpid());
  }

  void Comms::fieldAccess(){
    status = dataAccX.getFieldAccX("status");
    pid = dataAccX.getFieldAccX("pid");
  }

  size_t Comms::recordCount() const {
    return dataAccX.getEndPos();
  }

  void Comms::setRecordCount(size_t idx, size_t prevCount){
    IPC::semGuard G(&sem);
    if (dataAccX.getEndPos() <= prevCount) { dataAccX.setEndPos(idx); }
  }

  uint8_t Comms::getStatus() const{return status.uint(index);}
  uint8_t Comms::getStatus(size_t idx) const{return (master ? status.uint(idx) : 0);}
  void Comms::setStatus(uint8_t _status){status.set(_status, index);}
  void Comms::setStatus(uint8_t _status, size_t idx){
    if (!master){return;}
    status.set(_status, idx);
  }

  uint32_t Comms::getPid() const{return pid.uint(index);}
  uint32_t Comms::getPid(size_t idx) const{return (master ? pid.uint(idx) : 0);}
  void Comms::setPid(uint32_t _pid){pid.set(_pid, index);}
  void Comms::setPid(uint32_t _pid, size_t idx){
    if (!master){return;}
    pid.set(_pid, idx);
  }

  void Comms::finishAll(){
    if (!master){return;}
    size_t c = 0;
    bool keepGoing = true;
    do{
      keepGoing = false;
      for (size_t i = 0; i < recordCount(); i++){
        if (getStatus(i) == COMM_STATUS_INVALID || (getStatus(i) & COMM_STATUS_DISCONNECT)){continue;}
        uint64_t cPid = getPid(i);
        if (cPid > 1 && !(getStatus(i) & COMM_STATUS_NOKILL)){
          if (c > 4){
            Util::Procs::Stop(cPid); // soft kill after 4 loops
          }
          keepGoing = true;
        }
        setStatus(COMM_STATUS_REQDISCONNECT | getStatus(i), i);
      }
      if (keepGoing){Util::sleep(250);}
    }while (keepGoing && ++c < 8);
  }

  Comms::operator bool() const{
    if (master){return dataPage;}
    return dataPage && (getStatus() != COMM_STATUS_INVALID) && !(getStatus() & COMM_STATUS_DISCONNECT);
  }

  void Comms::setMaster(bool _master){
    master = _master;
    dataPage.master = _master;
  }

  void Comms::reload(const std::string & prefix, size_t baseSize, bool _master, bool reIssue, bool waitForPage) {
    closedRegistration = false;
    master = _master;
    if (!currentSize){currentSize = baseSize;}

    if (master){
      dataPage.init(prefix, currentSize, false, false);
      if (dataPage){
        dataPage.master = true;
        dataAccX = Util::RelAccX(dataPage.mapped);
        fieldAccess();
      }else{
        dataPage.init(prefix, currentSize, true);
        dataAccX = Util::RelAccX(dataPage.mapped, false);
        addFields();
        fieldAccess();
        size_t reqCount = (currentSize - dataAccX.getOffset()) / dataAccX.getRSize();
        dataAccX.setRCount(reqCount);
        dataAccX.setEndPos(1);
        dataAccX.setPresent(0);
        dataAccX.setReady();
      }
      return;
    }

    dataPage.init(prefix, currentSize, false, waitForPage);
    if (!dataPage){
      if (waitForPage) { WARN_MSG("Unable to open page %s", prefix.c_str()); }
      return;
    }
    dataAccX = Util::RelAccX(dataPage.mapped);
    if (dataAccX.isExit()){
      closedRegistration = true;
      index = INVALID_RECORD_INDEX;
      dataPage.close();
      return;
    }
    fieldAccess();
    if (index == INVALID_RECORD_INDEX || reIssue){
      size_t reqCount = dataAccX.getRCount();
      for (index = 0; index < reqCount; ++index){
        if (getStatus() == COMM_STATUS_INVALID){
          IPC::semGuard G(&sem);
          // The input can close registration after the page was mapped.
          // Its idle decision and this claim share this semaphore.
          if (dataAccX.isExit()) {
            closedRegistration = true;
            index = INVALID_RECORD_INDEX;
            dataPage.close();
            return;
          }
          if (getStatus() != COMM_STATUS_INVALID){continue;}
          nullFields();
          setStatus(COMM_STATUS_ACTIVE | defaultCommFlags);
          // Update endPos if/as needed
          if (dataAccX.getEndPos() <= index) { dataAccX.setEndPos(index + 1); }
          break;
        }
      }
      if (index >= reqCount){
        FAIL_MSG("Could not register entry on comm page!");
        dataPage.close();
      }
    }
  }

  /// Builds `fields` into the not yet ready structure `rax`.
  static void addStateFields(Util::RelAccX & rax, const std::vector<StateField> & fields) {
    for (const StateField & f : fields) { rax.addField(f.name, f.type, f.size); }
  }

  bool openStatePage(IPC::sharedPage & page, Util::RelAccX & rax, const std::string & name, size_t size,
                     size_t capacityBytes, const std::vector<StateField> & fields) {
    std::vector<char> scratch(4096, 0);
    Util::RelAccX wanted(scratch.data(), false);
    addStateFields(wanted, fields);
    const uint64_t layout = layoutFingerprint(scratch.data(), scratch.size());

    IPC::sharedPage previous(name, size, false, false);
    Util::RelAccX old;
    if (previous) {
      old = Util::RelAccX(previous.mapped, false);
      if (old.isReady() && layoutFingerprint(previous.mapped, previous.len) == layout) {
        page.swap(previous);
        rax = Util::RelAccX(page.mapped, false);
        return true;
      }
      // Readers map this page by name; they find the new page once this name is unlinked.
      old.setReload();
      IPC::sharedPage unlinker(name, 0, false, false);
      if (unlinker) { unlinker.master = true; }
    }
    page.init(name, size, true);
    if (!page.mapped) { return false; }
    page.master = false;
    rax = Util::RelAccX(page.mapped, false);
    addStateFields(rax, fields);
    const uint64_t capacity = (capacityBytes - rax.getOffset()) / rax.getRSize();
    rax.setRCount(capacity);
    if (previous && old.isReady() && old.getRCount()) {
      // Records keep their numbers; only the newest ones that fit the new capacity are kept.
      const uint64_t end = old.getEndPos();
      uint64_t first = old.getDeleted();
      if (end > first + capacity) { first = end - capacity; }
      if (end - first > old.getRCount()) { first = end - old.getRCount(); }
      for (uint64_t n = first; n < end; ++n) {
        for (const StateField & f : fields) {
          if (!old.hasField(f.name)) { continue; }
          const Util::RelAccXFieldData from = old.getFieldData(f.name);
          const Util::RelAccXFieldData to = rax.getFieldData(f.name);
          if ((from.type & 0xF0) != (to.type & 0xF0)) { continue; }
          switch (to.type & 0xF0) {
            case RAX_UINT:
            case RAX_INT: rax.setInt(to, old.getInt(from, n), n); break;
            case RAX_STRING: {
              const char *str = old.getPointer(from, n);
              rax.setString(to, std::string(str, strnlen(str, from.size)), n);
            } break;
            case RAX_RAW: memcpy(rax.getPointer(to, n), old.getPointer(from, n), std::min(from.size, to.size)); break;
            default: break;
          }
        }
      }
      rax.setDeleted(first);
      rax.setStartPos(first);
      rax.setEndPos(end);
      rax.setPresent(end - first);
      INFO_MSG("Recreated state page %s with this build's layout, keeping %" PRIu64 " records", name.c_str(), end - first);
    } else if (previous) {
      INFO_MSG("Recreated state page %s with this build's layout", name.c_str());
    }
    rax.setReady();
    return true;
  }

  const StatsSegment *StatsIndex::current() const {
    for (const StatsSegment & seg : segments) {
      if (seg.state == STATS_SEGMENT_CURRENT) { return &seg; }
    }
    return 0;
  }

  uint64_t layoutFingerprint(const char *page, size_t len) {
    // Header bytes 1 and 26-27 hold where the RelAccX field table starts and ends.
    if (!page || len < 36) { return 0; }
    const size_t start = (uint8_t)page[1];
    uint16_t end = 0;
    memcpy(&end, page + 26, sizeof(end));
    if (start < 11 || end <= start || end > len) { return 0; }
    uint64_t hash = 0xcbf29ce484222325ULL;
    for (size_t i = start; i < end; ++i) {
      hash ^= (uint8_t)page[i];
      hash *= 0x100000001b3ULL;
    }
    return hash ? hash : 1;
  }

  bool validStatsSegmentName(const std::string & name) {
    const std::string base = COMMS_STATISTICS;
    if (name == base) { return true; }
    if (name.size() <= base.size() + 1 || name.size() >= STATS_INDEX_NAME_SIZE) { return false; }
    if (name.compare(0, base.size(), base) || name[base.size()] != '.') { return false; }
    for (size_t i = base.size() + 1; i < name.size(); ++i) {
      if (!isdigit((unsigned char)name[i])) { return false; }
    }
    return true;
  }

  static uint64_t indexGeneration(const char *page) {
    return __atomic_load_n((const uint64_t *)(page + 8), __ATOMIC_ACQUIRE);
  }

  /// Copies the index. A writer holding SEM_STATISTICS passes `locked`: no other writer can be
  /// active then, and a generation left odd by a writer that died mid-write does not block it.
  static bool copyStatsIndex(const char *page, size_t len, StatsIndex & out, size_t attempts, bool locked) {
    if (!page || len < STATS_INDEX_HEADER_SIZE) { return false; }
    std::string entries;
    for (size_t attempt = 0; attempt < attempts; ++attempt) {
      if (attempt) {
        if (attempt < 50) {
          sched_yield();
        } else {
          Util::sleep(1);
        }
      }
      const uint64_t before = indexGeneration(page);
      if ((before & 1) && !locked) { continue; }
      uint32_t magic = 0, version = 0, count = 0, entrySize = 0;
      memcpy(&magic, page, 4);
      memcpy(&version, page + 4, 4);
      memcpy(&count, page + 16, 4);
      memcpy(&entrySize, page + 20, 4);
      const size_t room = len - STATS_INDEX_HEADER_SIZE;
      const bool header = magic == STATS_INDEX_MAGIC && version == STATS_INDEX_VERSION &&
        entrySize >= STATS_INDEX_ENTRY_SIZE && entrySize <= room && count <= room / entrySize;
      if (header) { entries.assign(page + STATS_INDEX_HEADER_SIZE, (size_t)count * entrySize); }
      __atomic_thread_fence(__ATOMIC_ACQUIRE);
      if (!locked && indexGeneration(page) != before) { continue; }
      if (!header) { return false; }
      out.generation = before;
      out.segments.clear();
      for (uint32_t i = 0; i < count; ++i) {
        const char *entry = entries.data() + (size_t)i * entrySize;
        const char *end = (const char *)memchr(entry, 0, STATS_INDEX_NAME_SIZE);
        if (!end) { continue; }
        StatsSegment seg;
        seg.name.assign(entry, end - entry);
        memcpy(&seg.state, entry + 64, 4);
        memcpy(&seg.fingerprint, entry + 72, 8);
        if (!validStatsSegmentName(seg.name) || (seg.state != STATS_SEGMENT_CURRENT && seg.state != STATS_SEGMENT_RETIRING)) {
          continue;
        }
        out.segments.push_back(seg);
      }
      return true;
    }
    return false;
  }

  bool readStatsIndex(const char *page, size_t len, StatsIndex & out, size_t attempts) {
    return copyStatsIndex(page, len, out, attempts, false);
  }

  bool readStatsIndex(StatsIndex & out) {
    IPC::sharedPage page(SHM_STATS_INDEX, 0, false, false);
    if (!page) { return false; }
    return readStatsIndex(page.mapped, page.len, out);
  }

  bool writeStatsIndex(char *page, size_t len, const std::vector<StatsSegment> & segments) {
    if (!page || len < STATS_INDEX_HEADER_SIZE) { return false; }
    if (segments.size() > (len - STATS_INDEX_HEADER_SIZE) / STATS_INDEX_ENTRY_SIZE) { return false; }
    for (const StatsSegment & seg : segments) {
      if (!validStatsSegmentName(seg.name)) { return false; }
      if (seg.state != STATS_SEGMENT_CURRENT && seg.state != STATS_SEGMENT_RETIRING) { return false; }
    }
    uint64_t *generation = (uint64_t *)(page + 8);
    uint64_t before = __atomic_load_n(generation, __ATOMIC_RELAXED);
    if (before & 1) { ++before; }
    __atomic_store_n(generation, before + 1, __ATOMIC_RELAXED);
    __atomic_thread_fence(__ATOMIC_RELEASE);
    uint32_t value = STATS_INDEX_MAGIC;
    memcpy(page, &value, 4);
    value = STATS_INDEX_VERSION;
    memcpy(page + 4, &value, 4);
    value = segments.size();
    memcpy(page + 16, &value, 4);
    value = STATS_INDEX_ENTRY_SIZE;
    memcpy(page + 20, &value, 4);
    memset(page + 24, 0, STATS_INDEX_HEADER_SIZE - 24);
    memset(page + STATS_INDEX_HEADER_SIZE, 0, segments.size() * STATS_INDEX_ENTRY_SIZE);
    for (size_t i = 0; i < segments.size(); ++i) {
      char *entry = page + STATS_INDEX_HEADER_SIZE + i * STATS_INDEX_ENTRY_SIZE;
      memcpy(entry, segments[i].name.data(), segments[i].name.size());
      memcpy(entry + 64, &segments[i].state, 4);
      memcpy(entry + 72, &segments[i].fingerprint, 8);
    }
    __atomic_thread_fence(__ATOMIC_RELEASE);
    __atomic_store_n(generation, before + 2, __ATOMIC_RELEASE);
    return true;
  }

  /// Maps the index page; with `create`, creates it when absent. The page is never unlinked when
  /// `page` closes.
  static bool openStatsIndexPage(IPC::sharedPage & page, bool create) {
    page.init(SHM_STATS_INDEX, SHM_STATS_INDEX_SIZE, false, false);
    if (!page && create) {
      page.init(SHM_STATS_INDEX, SHM_STATS_INDEX_SIZE, true, false);
      page.master = false;
    }
    return page && page.len >= STATS_INDEX_HEADER_SIZE;
  }

  /// Whether the segment page `name` exists; `fingerprint` is its layout, or 0 while it is not ready.
  static bool probeStatsSegment(const std::string & name, uint64_t & fingerprint) {
    fingerprint = 0;
    IPC::sharedPage page(name, 0, false, false);
    if (!page) { return false; }
    Util::RelAccX rax(page.mapped, false);
    if (rax.isReady()) { fingerprint = layoutFingerprint(page.mapped, page.len); }
    return true;
  }

  /// Removes `name` from the index; when no segment is left, unlinks the index. The caller holds
  /// SEM_STATISTICS.
  static void unlistStatsSegment(const std::string & name) {
    IPC::sharedPage indexPage;
    if (!openStatsIndexPage(indexPage, false)) { return; }
    StatsIndex idx;
    if (!copyStatsIndex(indexPage.mapped, indexPage.len, idx, 1, true)) { return; }
    std::vector<StatsSegment> keep;
    for (const StatsSegment & seg : idx.segments) {
      if (seg.name != name) { keep.push_back(seg); }
    }
    if (keep.size() == idx.segments.size()) { return; }
    if (keep.empty()) {
      indexPage.master = true;
      return;
    }
    writeStatsIndex(indexPage.mapped, indexPage.len, keep);
  }

  Sessions::Sessions() : Connections(){sem.open(SEM_STATISTICS, O_CREAT | O_RDWR, ACCESSPERMS, 1);}

  Sessions::~Sessions() {
    // A master that closes for good unlinks its segment, so the index stops listing it.
    if (!master || !dataPage.mapped) { return; }
    IPC::semGuard guard(&sem);
    unlistStatsSegment(dataPage.name);
  }

  uint64_t Sessions::buildFingerprint() {
    static const uint64_t fingerprint = []() {
      std::vector<char> scratch(4096, 0);
      Sessions layout;
      layout.dataAccX = Util::RelAccX(scratch.data(), false);
      layout.addFields();
      const uint64_t result = layoutFingerprint(scratch.data(), scratch.size());
      layout.dataAccX = Util::RelAccX();
      return result;
    }();
    return fingerprint;
  }

  void Sessions::reload(bool _master, bool reIssue) {
    if (!_master) {
      // The controller creates the index page, then the segment, then publishes it, and it
      // closes registration on a retiring segment; a fresh read of the index names where to
      // register next.
      bool indexSeen = false;
      for (size_t attempt = 0; attempt < 10; ++attempt) {
        if (attempt) { Util::sleep(10); }
        IPC::sharedPage indexPage(SHM_STATS_INDEX, 0, false, false);
        if (!indexPage) { break; }
        indexSeen = true;
        StatsIndex idx;
        if (!readStatsIndex(indexPage.mapped, indexPage.len, idx)) { continue; }
        const StatsSegment *current = idx.current();
        if (!current) { continue; }
        if (dataPage.mapped && dataPage.name != current->name) {
          if (index != INVALID_RECORD_INDEX && status) { setStatus(COMM_STATUS_DISCONNECT | getStatus()); }
          index = INVALID_RECORD_INDEX;
          dataPage.close();
        }
        Comms::reload(current->name, COMMS_STATISTICS_INITSIZE, false, reIssue, false);
        if (*this) { return; }
      }
      // Without an index the running controller predates it and reads only the legacy page, which
      // it may still be creating.
      if (dataPage.mapped && dataPage.name != COMMS_STATISTICS) {
        index = INVALID_RECORD_INDEX;
        dataPage.close();
      }
      Comms::reload(COMMS_STATISTICS, COMMS_STATISTICS_INITSIZE, false, reIssue, !indexSeen);
      return;
    }

    IPC::semGuard guard(&sem);
    IPC::sharedPage indexPage;
    if (!openStatsIndexPage(indexPage, true)) {
      FAIL_MSG("Could not open the statistics index; using the legacy statistics page");
      Comms::reload(COMMS_STATISTICS, COMMS_STATISTICS_INITSIZE, true, reIssue);
      return;
    }
    StatsIndex idx;
    copyStatsIndex(indexPage.mapped, indexPage.len, idx, 1, true);
    const uint64_t mine = buildFingerprint();
    std::vector<StatsSegment> keep;
    std::string currentName;
    bool legacyListed = false;
    for (StatsSegment seg : idx.segments) {
      if (seg.name == COMMS_STATISTICS) { legacyListed = true; }
      uint64_t fingerprint = 0;
      if (!probeStatsSegment(seg.name, fingerprint) || !fingerprint) { continue; }
      bool duplicate = false;
      for (const StatsSegment & k : keep) { duplicate |= (k.name == seg.name); }
      if (duplicate) { continue; }
      seg.fingerprint = fingerprint;
      if (seg.state == STATS_SEGMENT_CURRENT) {
        if (currentName.empty() && fingerprint == mine) {
          currentName = seg.name;
          keep.insert(keep.begin(), seg);
          continue;
        }
        seg.state = STATS_SEGMENT_RETIRING;
      }
      keep.push_back(seg);
    }
    uint64_t legacyFingerprint = 0;
    if (!legacyListed && probeStatsSegment(COMMS_STATISTICS, legacyFingerprint) && legacyFingerprint) {
      StatsSegment legacy;
      legacy.name = COMMS_STATISTICS;
      legacy.state = STATS_SEGMENT_RETIRING;
      legacy.fingerprint = legacyFingerprint;
      keep.push_back(legacy);
    }
    if (currentName.empty()) {
      // Segment names only need to be unique; a leftover page of that name is never reused.
      uint64_t number = (idx.generation | 1) + 1;
      uint64_t unused = 0;
      do {
        currentName = std::string(COMMS_STATISTICS) + "." + std::to_string(number++);
      } while (probeStatsSegment(currentName, unused));
    }
    Comms::reload(currentName, COMMS_STATISTICS_INITSIZE, true, reIssue);
    if (!dataPage) {
      FAIL_MSG("Could not open statistics segment %s", currentName.c_str());
      return;
    }
    if (keep.empty() || keep.front().name != currentName) {
      StatsSegment created;
      created.name = currentName;
      created.state = STATS_SEGMENT_CURRENT;
      created.fingerprint = mine;
      keep.insert(keep.begin(), created);
      INFO_MSG("Statistics segment %s is now current", currentName.c_str());
    }
    const size_t capacity = (indexPage.len - STATS_INDEX_HEADER_SIZE) / STATS_INDEX_ENTRY_SIZE;
    if (keep.size() > capacity) {
      WARN_MSG("Statistics index full: no longer listing %zu retiring segments", keep.size() - capacity);
      keep.resize(capacity);
    }
    if (!writeStatsIndex(indexPage.mapped, indexPage.len, keep)) { FAIL_MSG("Could not write the statistics index"); }
  }

  bool Sessions::attachSegment(const std::string & name) {
    master = true;
    closedRegistration = false;
    index = INVALID_RECORD_INDEX;
    dataPage.init(name, 0, false, false);
    if (!dataPage) { return false; }
    Util::RelAccX rax(dataPage.mapped, false);
    if (!rax.isReady()) {
      dataPage.close();
      return false;
    }
    dataPage.master = true;
    dataAccX = Util::RelAccX(dataPage.mapped, false);
    fieldAccess();
    return true;
  }

  void Sessions::dropSegment() {
    master = false;
    index = INVALID_RECORD_INDEX;
    if (dataPage.mapped) { dataPage.master = true; }
    dataPage.close();
  }

  std::string Sessions::getRequestedStream(size_t idx) const {
    return master ? requestedStream.string(idx) : "";
  }

  StatisticsSegments::StatisticsSegments() {}

  StatisticsSegments::~StatisticsSegments() {}

  void StatisticsSegments::start() {
    if (segments.size()) { return; }
    if (!sem) { sem.open(SEM_STATISTICS, O_CREAT | O_RDWR, ACCESSPERMS, 1); }
    std::unique_ptr<Sessions> current(new Sessions());
    current->reload(true);
    const std::string currentName = current->pageName();
    segments.push_back(std::move(current));
    StatsIndex idx;
    if (!readStatsIndex(idx)) { return; }
    for (const StatsSegment & seg : idx.segments) {
      if (seg.state != STATS_SEGMENT_RETIRING || seg.name == currentName) { continue; }
      std::unique_ptr<Sessions> old(new Sessions());
      if (!old->attachSegment(seg.name)) {
        unlist(seg.name);
        continue;
      }
      INFO_MSG("Reading retiring statistics segment %s", seg.name.c_str());
      segments.push_back(std::move(old));
    }
  }

  void StatisticsSegments::stop(bool restarting, bool killClients) {
    if (restarting) {
      for (auto & seg : segments) { seg->setMaster(false); }
      return;
    }
    if (killClients) {
      for (auto & seg : segments) { seg->finishAll(); }
    }
    IPC::sharedPage indexPage;
    if (openStatsIndexPage(indexPage, false)) { indexPage.master = true; }
  }

  void StatisticsSegments::unlist(const std::string & name) {
    IPC::semGuard guard(&sem);
    unlistStatsSegment(name);
  }

  /// Whether any record of `seg` is still in use. The collection loop invalidates records whose
  /// process ended, so only records of running processes stay in use.
  static bool segmentInUse(Sessions & seg) {
    for (size_t i = 0; i < seg.recordCount(); ++i) {
      if (seg.getStatus(i) != COMM_STATUS_INVALID) { return true; }
    }
    return false;
  }

  void StatisticsSegments::retire(uint64_t nowMs) {
    for (size_t i = 1; i < segments.size();) {
      Sessions & seg = *segments[i];
      const std::string name = seg.pageName();
      std::map<std::string, uint64_t>::iterator closed = closedAt.find(name);
      if (closed == closedAt.end()) {
        if (!segmentInUse(seg)) {
          // Clients claim records while holding this semaphore, so none can register after this.
          IPC::semGuard guard(&sem);
          if (!segmentInUse(seg)) {
            seg.setExit();
            closedAt[name] = nowMs;
            INFO_MSG("Closed registration on drained statistics segment %s", name.c_str());
          }
        }
        ++i;
        continue;
      }
      if (nowMs - closed->second < graceMs) {
        ++i;
        continue;
      }
      unlist(name);
      seg.dropSegment();
      closedAt.erase(closed);
      segments.erase(segments.begin() + i);
      INFO_MSG("Removed retired statistics segment %s", name.c_str());
    }
  }

  bool StatisticsSegments::sessIdExists(const std::string & sessId) {
    for (auto & seg : segments) {
      if (seg->sessIdExists(sessId)) { return true; }
    }
    return false;
  }

  std::string Sessions::getSessId() const{return sessId.string(index);}
  std::string Sessions::getSessId(size_t idx) const{return (master ? sessId.string(idx) : 0);}
  void Sessions::setSessId(std::string _sid){sessId.set(_sid, index);}
  void Sessions::setSessId(std::string _sid, size_t idx){
    if (!master){return;}
    sessId.set(_sid, idx);
  }

  bool Sessions::sessIdExists(std::string _sid){
    for (size_t i = 0; i < recordCount(); i++){
      if (getStatus(i) == COMM_STATUS_INVALID || (getStatus(i) & COMM_STATUS_DISCONNECT)){continue;}
      if (getSessId(i) == _sid){
        if (Util::Procs::isRunning(getPid(i))){
          return true;
        }
      }
    }
    return false;
  }

  void Sessions::addFields(){
    Connections::addFields();
    dataAccX.addField("tags", RAX_STRING, 512);
    dataAccX.addField("sessid", RAX_STRING, 80);
    dataAccX.addField("requestedstream", RAX_STRING, 100);
  }

  void Sessions::nullFields(){
    Connections::nullFields();
    setSessId("");
    setTags("");
    setRequestedStream("");
  }

  void Sessions::fieldAccess(){
    Connections::fieldAccess();
    tags = dataAccX.getFieldAccX("tags");
    sessId = dataAccX.getFieldAccX("sessid");
    requestedStream = dataAccX.getFieldAccX("requestedstream");
  }

  void Sessions::setRequestedStream(const std::string & name) {
    requestedStream.set(name, index);
  }

  std::string Sessions::getTags() const{return tags.string(index);}
  std::string Sessions::getTags(size_t idx) const{return (master ? tags.string(idx) : 0);}
  void Sessions::setTags(std::string _sid){tags.set(_sid, index);}
  void Sessions::setTags(std::string _sid, size_t idx){
    if (!master){return;}
    tags.set(_sid, idx);
  }

  Users::Users() : Comms(){
    noMaster = false;
  }

  Users::~Users(){
    if (noMaster){master = false;}
  }

  bool Users::beginIdleShutdown(char *streamState) {
    if (!master || !dataPage || !streamState) { return false; }
    IPC::semGuard guard(&sem);
    for (size_t i = 0; i < recordCount(); ++i) {
      const uint8_t state = getStatus(i);
      if (state == COMM_STATUS_INVALID || (state & COMM_STATUS_DISCONNECT)) { continue; }
      const uint32_t reader = getPid(i);
      if (reader > 1 && Util::Procs::isRunning(reader)) { return false; }
    }
    // Readers already registered keep this generation alive. Later claims
    // see the exit flag and wait for the next input instead of attaching here.
    dataAccX.setExit();
    streamState[0] = STRMSTAT_SHUTDOWN;
    return true;
  }

  Users::Users(const Users &rhs) : Comms(){
    if (rhs){
      reload(rhs.streamName, (size_t)rhs.getTrack());
      if (*this){
        setKeyNum(rhs.getKeyNum());
        setTrack(rhs.getTrack());
      }
    }
  }

  void Users::reload(const std::string &_streamName, bool _master, bool reIssue){
    streamName = _streamName;

    char semName[NAME_BUFFER_SIZE];
    snprintf(semName, NAME_BUFFER_SIZE, SEM_USERS, streamName.c_str());
    sem.open(semName, O_CREAT | O_RDWR, ACCESSPERMS, 1);

    char userPageName[NAME_BUFFER_SIZE];
    snprintf(userPageName, NAME_BUFFER_SIZE, COMMS_USERS, streamName.c_str());

    Comms::reload(userPageName, COMMS_USERS_INITSIZE, _master, reIssue);
  }

  void Users::addFields(){
    Comms::addFields();
    dataAccX.addField("track", RAX_64UINT);
    dataAccX.addField("keynum", RAX_64UINT);
  }

  void Users::nullFields(){
    Comms::nullFields();
    setTrack(requestedTrack);
    setKeyNum(0);
  }

  void Users::fieldAccess(){
    Comms::fieldAccess();
    track = dataAccX.getFieldAccX("track");
    keyNum = dataAccX.getFieldAccX("keynum");
  }

  void Users::reload(const std::string &_streamName, size_t idx, uint8_t initialState){
    requestedTrack = idx;
    reload(_streamName);
    if (dataPage){
      setTrack(idx);
      setKeyNum(0);
      setStatus(initialState | defaultCommFlags);
    }
  }

  void Users::reloadReadonly(const std::string &_streamName){
    index = 1;
    noMaster = true;
    reload(_streamName);
    master = true;
  }

  uint32_t Users::getTrack() const{return track.uint(index);}
  uint32_t Users::getTrack(size_t idx) const{return (master ? track.uint(idx) : 0);}
  void Users::setTrack(uint32_t _track){track.set(_track, index);}
  void Users::setTrack(uint32_t _track, size_t idx){
    if (!master){return;}
    track.set(_track, idx);
  }

  size_t Users::getKeyNum() const{return keyNum.uint(index);}
  size_t Users::getKeyNum(size_t idx) const{return (master ? keyNum.uint(idx) : 0);}
  void Users::setKeyNum(size_t _keyNum){keyNum.set(_keyNum, index);}
  void Users::setKeyNum(size_t _keyNum, size_t idx){
    if (!master){return;}
    keyNum.set(_keyNum, idx);
  }



  void Connections::reload(const std::string & sessId, bool _master, bool reIssue){
    // Open SEM_SESSION
    if(!sem){
      char semName[NAME_BUFFER_SIZE];
      snprintf(semName, NAME_BUFFER_SIZE, SEM_SESSION, sessId.c_str());
      // A booting session creates its semaphore within milliseconds; a loaded host may need
      // much longer, so the wait is long. It ends early once this process is told to stop: it
      // has no use for the session then and must not be held up by it.
      const uint64_t waitStart = Util::bootMS();
      uint64_t backoff = 5;
      do {
        sem.open(semName, O_RDWR, ACCESSPERMS, 1, true);
        if (sem) { break; }
        if (Util::Config::stopRequested()) {
          INFO_MSG("Not waiting for session %s: this process is stopping", sessId.c_str());
          startFailed = true;
          index = INVALID_RECORD_INDEX;
          return;
        }
        Util::sleep(backoff);
        if (backoff < 500) { backoff *= 2; }
      } while (Util::bootMS() - waitStart < SESSION_SEMAPHORE_WAIT_MS);
      if (!sem){
        FAIL_MSG("Could not open session semaphore; aborting!");
        index = INVALID_RECORD_INDEX;
        return;
      }
    }
    char userPageName[NAME_BUFFER_SIZE];
    snprintf(userPageName, NAME_BUFFER_SIZE, COMMS_SESSIONS, sessId.c_str());
    Comms::reload(userPageName, COMMS_SESSIONS_INITSIZE, _master, reIssue);
  }

  /// \brief Claims a spot on the connections page for the input/output which calls this function
  ///        Starts the MistSession binary for each session, which handles the statistics
  ///         and the USER_NEW and USER_END triggers
  /// \param streamName: Name of the stream the input is providing or an output is making available to viewers
  /// \param ip: IP address of the viewer which wants to access streamName. For inputs this value can be set to any value
  /// \param tkn: Session token given by the player or randomly generated
  /// \param protocol: Protocol currently in use for this connection
  /// \param _master: If True, we are reading from this page. If False, we are writing (to our entry) on this page
  /// \param reIssue: If True, claim a new entry on this page
  void Connections::reload(const std::string & streamName, const std::string & ip, const std::string & tkn,
                           const std::string & protocol, const std::string & reqUrl, bool _master, bool reIssue,
                           const std::string & origin, const std::string & referer, const std::string & requestedStream) {
    initialTkn = tkn;
    startFailed = false;
    uint8_t sessMode = sessionViewerMode;
    // USER_NEW decides a viewer session once, including its origin, so requests from a
    // different site must not join a session another site's request opened.
    const std::string sessOrigin = viewerSessionOrigin(origin, referer);
    // Generate a unique session ID for each viewer, input or output
    if (protocol.size() >= 6 && protocol.substr(0, 6) == "INPUT:"){
      sessMode = sessionInputMode;
      sessionId = "I" + generateSession(streamName, ip, tkn, protocol, sessMode);
    }else if (protocol.size() >= 7 && protocol.substr(0, 7) == "OUTPUT:"){
      sessMode = sessionOutputMode;
      sessionId = "O" + generateSession(streamName, ip, tkn, protocol, sessMode);
    }else{
      // If the session only contains the HTTP connector, check sessionStreamInfoMode
      if (protocol.size() == 4 && protocol == "HTTP"){
        if (sessionStreamInfoMode == SESS_HTTP_AS_VIEWER){
          sessionId = generateSession(streamName, ip, tkn, protocol, sessMode, sessOrigin);
        }else if (sessionStreamInfoMode == SESS_HTTP_AS_OUTPUT){
          sessMode = sessionOutputMode;
          sessionId = "O" + generateSession(streamName, ip, tkn, protocol, sessMode);
        }else if (sessionStreamInfoMode == SESS_HTTP_DISABLED){
          return;
        }else if (sessionStreamInfoMode == SESS_HTTP_AS_UNSPECIFIED){
          // Set sessMode to include all variables when determining the session ID
          sessMode = sessionUnspecifiedMode;
          sessionId = "U" + generateSession(streamName, ip, tkn, protocol, sessMode, sessOrigin);
        }else{
          sessionId = generateSession(streamName, ip, tkn, protocol, sessMode, sessOrigin);
        }
      }else{
        sessionId = generateSession(streamName, ip, tkn, protocol, sessMode, sessOrigin);
      }
    }
    char userPageName[NAME_BUFFER_SIZE];
    snprintf(userPageName, NAME_BUFFER_SIZE, COMMS_SESSIONS, sessionId.c_str());
    // Check if the page exists, if not, spawn new session process
    if (!_master){
      dataPage.init(userPageName, 0, false, false);
      if (!dataPage){
        INFO_MSG("Booting new session %s", sessionId.c_str());
        std::string host;
        Socket::hostBytesToStr(ip.data(), ip.size(), host);
        pid_t thisPid;
        std::deque<std::string> args;
        args.push_back(Util::getMyPath() + "MistSession");
        args.push_back(sessionId);
        // Values not passed as arguments reach the session through its own environment. Every
        // thread of this process can boot sessions and fire triggers concurrently, so the
        // process environment itself is never modified.
        std::map<std::string, std::string> sessionEnv;

        // First bit defines whether to include stream name
        if (sessMode & 0x08){
          args.push_back("--streamname");
          args.push_back(streamName);
        }else{
          sessionEnv["SESSION_STREAM"] = streamName;
        }
        // Second bit defines whether to include viewer ip
        if (sessMode & 0x04){
          args.push_back("--ip");
          args.push_back(host);
        }else{
          sessionEnv["SESSION_IP"] = host;
        }
        // Third bit defines whether to include tkn
        if (sessMode & 0x02){
          args.push_back("--tkn");
          args.push_back(tkn);
        }else{
          sessionEnv["SESSION_TKN"] = tkn;
        }
        // Fourth bit defines whether to include protocol
        if (sessMode & 0x01){
          args.push_back("--protocol");
          args.push_back(protocol);
        }else{
          sessionEnv["SESSION_PROTOCOL"] = protocol;
        }
        if (Util::printDebugLevel != DEBUG){
          args.push_back("--debug");
          args.push_back(JSON::Value(Util::printDebugLevel).asString());
        }
        sessionEnv["SESSION_REQURL"] = reqUrl;
        sessionEnv["SESSION_ORIGIN"] = origin;
        sessionEnv["SESSION_REFERER"] = referer;
        sessionEnv["SESSION_REQUESTED_STREAM"] = requestedStream;
        int err = fileno(stderr);
        thisPid = Util::Procs::StartPiped(args, 0, 0, &err, Util::environmentWith(sessionEnv));
        if (!thisPid) {
          // No session process means no session semaphore will ever appear: report the
          // failure now instead of waiting it out. The caller retries on its next stats tick.
          WARN_MSG("Could not start session %s; retrying on the next stats update", sessionId.c_str());
          startFailed = true;
          index = INVALID_RECORD_INDEX;
          return;
        }
        Util::Procs::forget(thisPid);
      }else{
        INFO_MSG("Connecting to existing session %s", sessionId.c_str());
      }
    }
    reload(sessionId, _master, reIssue);
    if (index != INVALID_RECORD_INDEX){
      setConnector(protocol);
      setHost(ip);
      setStream(streamName);
      VERYHIGH_MSG("Reloading connection. Claimed record %" PRIu64, index);
    }
  }

  /// \brief Marks the data page as closed, so that we longer write any new data to is
  void Connections::setExit(){
    if (!master){return;}
    dataAccX.setExit();
  }

  bool Connections::getExit(){
    return dataAccX.isExit();
  }

  void Connections::unload(){
    if (index != INVALID_RECORD_INDEX){
      setStatus(COMM_STATUS_DISCONNECT | getStatus());
    }
    index = INVALID_RECORD_INDEX;
  }
  void Connections::addFields(){
    Comms::addFields();
    dataAccX.addField("now", RAX_64UINT);
    dataAccX.addField("time", RAX_64UINT);
    dataAccX.addField("lastsecond", RAX_64UINT);
    dataAccX.addField("down", RAX_64UINT);
    dataAccX.addField("up", RAX_64UINT);
    dataAccX.addField("host", RAX_RAW, 16);
    dataAccX.addField("stream", RAX_STRING, 100);
    dataAccX.addField("connector", RAX_STRING, 20);
    dataAccX.addField("pktcount", RAX_64UINT);
    dataAccX.addField("pktloss", RAX_64UINT);
    dataAccX.addField("pktretrans", RAX_64UINT);
  }

  void Connections::nullFields(){
    Comms::nullFields();
    setConnector("");
    setStream("");
    setHost("");
    setUp(0);
    setDown(0);
    setLastSecond(0);
    setTime(0);
    setNow(0);
    setPacketCount(0);
    setPacketLostCount(0);
    setPacketRetransmitCount(0);
  }

  void Connections::fieldAccess(){
    Comms::fieldAccess();
    now = dataAccX.getFieldAccX("now");
    time = dataAccX.getFieldAccX("time");
    lastSecond = dataAccX.getFieldAccX("lastsecond");
    down = dataAccX.getFieldAccX("down");
    up = dataAccX.getFieldAccX("up");
    host = dataAccX.getFieldAccX("host");
    stream = dataAccX.getFieldAccX("stream");
    connector = dataAccX.getFieldAccX("connector");
    pktcount = dataAccX.getFieldAccX("pktcount");
    pktloss = dataAccX.getFieldAccX("pktloss");
    pktretrans = dataAccX.getFieldAccX("pktretrans");
  }

  uint64_t Connections::getNow() const{return now.uint(index);}
  uint64_t Connections::getNow(size_t idx) const{return (master ? now.uint(idx) : 0);}
  void Connections::setNow(uint64_t _now){now.set(_now, index);}
  void Connections::setNow(uint64_t _now, size_t idx){
    if (!master){return;}
    now.set(_now, idx);
  }

  uint64_t Connections::getTime() const{return time.uint(index);}
  uint64_t Connections::getTime(size_t idx) const{return (master ? time.uint(idx) : 0);}
  void Connections::setTime(uint64_t _time){time.set(_time, index);}
  void Connections::setTime(uint64_t _time, size_t idx){
    if (!master){return;}
    time.set(_time, idx);
  }

  uint64_t Connections::getLastSecond() const{return lastSecond.uint(index);}
  uint64_t Connections::getLastSecond(size_t idx) const{
    return (master ? lastSecond.uint(idx) : 0);
  }
  void Connections::setLastSecond(uint64_t _lastSecond){lastSecond.set(_lastSecond, index);}
  void Connections::setLastSecond(uint64_t _lastSecond, size_t idx){
    if (!master){return;}
    lastSecond.set(_lastSecond, idx);
  }

  uint64_t Connections::getDown() const{return down.uint(index);}
  uint64_t Connections::getDown(size_t idx) const{return (master ? down.uint(idx) : 0);}
  void Connections::setDown(uint64_t _down){down.set(_down, index);}
  void Connections::setDown(uint64_t _down, size_t idx){
    if (!master){return;}
    down.set(_down, idx);
  }

  uint64_t Connections::getUp() const{return up.uint(index);}
  uint64_t Connections::getUp(size_t idx) const{return (master ? up.uint(idx) : 0);}
  void Connections::setUp(uint64_t _up){up.set(_up, index);}
  void Connections::setUp(uint64_t _up, size_t idx){
    if (!master){return;}
    up.set(_up, idx);
  }

  std::string Connections::getHost() const{return std::string(host.ptr(index), 16);}
  std::string Connections::getHost(size_t idx) const{
    if (!master){return std::string((size_t)16, (char)'\000');}
    return std::string(host.ptr(idx), 16);
  }
  void Connections::setHost(std::string _host){host.set(_host, index);}
  void Connections::setHost(std::string _host, size_t idx){
    if (!master){return;}
    host.set(_host, idx);
  }

  std::string Connections::getStream() const{return stream.string(index);}
  std::string Connections::getStream(size_t idx) const{return (master ? stream.string(idx) : "");}
  void Connections::setStream(std::string _stream){stream.set(_stream, index);}
  void Connections::setStream(std::string _stream, size_t idx){
    if (!master){return;}
    stream.set(_stream, idx);
  }

  std::string Connections::getConnector() const{return connector.string(index);}
  std::string Connections::getConnector(size_t idx) const{
    return (master ? connector.string(idx) : "");
  }
  void Connections::setConnector(std::string _connector){connector.set(_connector, index);}
  void Connections::setConnector(std::string _connector, size_t idx){
    if (!master){return;}
    connector.set(_connector, idx);
  }

  bool Connections::hasConnector(size_t idx, std::string protocol){
    std::stringstream sstream(connector.string(idx));
    std::string _conn;
    while (std::getline(sstream, _conn, ',')){
      if (_conn == protocol){
        return true;
      }
    }
    return false;
  }

  uint64_t Connections::getPacketCount() const{return pktcount.uint(index);}
  uint64_t Connections::getPacketCount(size_t idx) const{
    return (master ? pktcount.uint(idx) : 0);
  }
  void Connections::setPacketCount(uint64_t _count){pktcount.set(_count, index);}
  void Connections::setPacketCount(uint64_t _count, size_t idx){
    if (!master){return;}
    pktcount.set(_count, idx);
  }

  uint64_t Connections::getPacketLostCount() const{return pktloss.uint(index);}
  uint64_t Connections::getPacketLostCount(size_t idx) const{
    return (master ? pktloss.uint(idx) : 0);
  }
  void Connections::setPacketLostCount(uint64_t _lost){pktloss.set(_lost, index);}
  void Connections::setPacketLostCount(uint64_t _lost, size_t idx){
    if (!master){return;}
    pktloss.set(_lost, idx);
  }

  uint64_t Connections::getPacketRetransmitCount() const{return pktretrans.uint(index);}
  uint64_t Connections::getPacketRetransmitCount(size_t idx) const{
    return (master ? pktretrans.uint(idx) : 0);
  }
  void Connections::setPacketRetransmitCount(uint64_t _retrans){pktretrans.set(_retrans, index);}
  void Connections::setPacketRetransmitCount(uint64_t _retrans, size_t idx){
    if (!master){return;}
    pktretrans.set(_retrans, idx);
  }

  /// \brief Generates a session ID which is unique per viewer
  /// \return generated session ID as string
  std::string viewerSessionOrigin(const std::string & origin, const std::string & referer) {
    const std::string & source = (origin.size() && origin != "null") ? origin : referer;
    size_t schemeEnd = source.find("://");
    if (schemeEnd == std::string::npos || !schemeEnd) { return ""; }
    size_t hostEnd = source.find_first_of("/?#", schemeEnd + 3);
    std::string result = source.substr(0, hostEnd);
    for (size_t i = 0; i < result.size(); ++i) { result[i] = tolower(result[i]); }
    return result;
  }

  std::string userNewPayload(const std::string & streamName, const std::string & host, const std::string & token,
                             const std::string & protocol, const std::string & reqUrl, const std::string & sessionId,
                             bool validToken, const std::string & origin, const std::string & referer) {
    return streamName + "\n" + host + "\n" + token + "\n" + protocol + "\n" + reqUrl + "\n" + sessionId + "\n" +
      (validToken ? "true" : "false") + "\n" + origin + "\n" + referer + "\n";
  }

  /// Returns the resolved stream of this viewer's live, admitted session on the statistics
  /// segment `segment`, or an empty string.
  static std::string admittedOnSegment(const std::string & segment, const std::string & streamName,
                                       const std::string & ip, const std::string & token, const std::string & protocol,
                                       const std::string & origin, const std::string & referer) {
    IPC::sharedPage statsPage(segment, 0, false, false);
    if (!statsPage) { return ""; }
    Util::RelAccX stats(statsPage.mapped, false);
    if (!stats.isReady() || stats.isExit() || stats.isReload() || !stats.hasField("sessid") ||
        !stats.hasField("stream") || !stats.hasField("status") || !stats.hasField("pid") || !stats.hasField("now")) {
      return "";
    }
    Util::FieldAccX ids = stats.getFieldAccX("sessid"), streams = stats.getFieldAccX("stream");
    Util::FieldAccX states = stats.getFieldAccX("status"), pids = stats.getFieldAccX("pid"), times = stats.getFieldAccX("now");
    Util::FieldAccX requested;
    if (stats.hasField("requestedstream")) { requested = stats.getFieldAccX("requestedstream"); }
    const uint64_t now = Util::bootSecs();
    const uint64_t count = std::min<uint64_t>(stats.getEndPos(), stats.getRCount());
    Connections identity;
    for (uint64_t i = 0; i < count; ++i) {
      const uint8_t state = states.uint(i);
      const std::string target = streams.string(i), id = ids.string(i);
      if (!(state & COMM_STATUS_ACTIVE) || (state & (COMM_STATUS_DISCONNECT | COMM_STATUS_REQDISCONNECT | COMM_STATUS_SESSSTOP)) ||
          target.empty() || (target != streamName && (!requested || requested.string(i) != streamName))) {
        continue;
      }
      const uint64_t observed = times.uint(i);
      const uint32_t pid = pids.uint(i);
      if (!observed || observed > now || now - observed > STATS_DELAY || pid <= 1 || !Util::Procs::isRunning(pid) ||
          identity.generateSession(target, ip, token, protocol, sessionViewerMode, viewerSessionOrigin(origin, referer)) != id) {
        continue;
      }
      char name[NAME_BUFFER_SIZE];
      snprintf(name, sizeof(name), SEM_SESSION, id.c_str());
      IPC::semaphore lock(name, O_RDWR, ACCESSPERMS, 1, true);
      // MistSession releases this only after USER_NEW has answered. Its live
      // session survives the gaps between individual HLS HTTP requests.
      if (!lock || !lock.tryWait()) { continue; }
      snprintf(name, sizeof(name), COMMS_SESSIONS, id.c_str());
      IPC::sharedPage sessionPage(name, 0, false, false);
      bool admitted = false;
      if (sessionPage) {
        Util::RelAccX session(sessionPage.mapped, false);
        admitted = session.isReady() && !session.isExit() && !session.isReload();
      }
      lock.post();
      if (admitted) { return target; }
    }
    return "";
  }

  std::string admittedViewerStream(const std::string & streamName, const std::string & ip, const std::string & token,
                                   const std::string & protocol, const std::string & origin, const std::string & referer) {
    if (streamName.empty() || token.empty() || ip.size() != 16) { return ""; }
    sessionConfigCache();
    // Aggregated accounting cannot prove this individual viewer's admission.
    if ((sessionViewerMode & 0x0E) != 0x0E) { return ""; }
    // A session registered on the current segment or, until it ends, on a retiring one.
    std::vector<std::string> segments;
    StatsIndex idx;
    if (readStatsIndex(idx)) {
      for (const StatsSegment & seg : idx.segments) { segments.push_back(seg.name); }
    } else {
      segments.push_back(COMMS_STATISTICS);
    }
    for (const std::string & segment : segments) {
      const std::string target = admittedOnSegment(segment, streamName, ip, token, protocol, origin, referer);
      if (target.size()) { return target; }
    }
    return "";
  }

  std::string Connections::generateSession(const std::string & streamName, const std::string & ip, const std::string & tkn,
                                           const std::string & connector, uint64_t sessionMode, const std::string & viewerOrigin) {
    std::string concat;
    std::string debugMsg = "Generating session id based on";
    // A viewer's origin always participates: it is part of what USER_NEW admitted.
    if (viewerOrigin.size()) {
      concat += viewerOrigin + "\n";
      debugMsg += " origin '" + viewerOrigin + "'";
    }
    // First bit defines whether to include stream name
    if (sessionMode & 0x08){
      concat += streamName;
      debugMsg += " stream name '" + streamName + "'";
    }
    // Second bit defines whether to include viewer ip
    if (sessionMode & 0x04){
      concat += ip;
      std::string ipHex;
      Socket::hostBytesToStr(ip.c_str(), ip.size(), ipHex);
      debugMsg += " IP '" + ipHex + "'";
    }
    // Third bit defines whether to include client-side session token
    if (sessionMode & 0x02){
      concat += tkn;
      debugMsg += " session token '" + tkn + "'";
    }
    // Fourth bit defines whether to include protocol
    if (sessionMode & 0x01){
      concat += connector;
      debugMsg += " protocol '" + connector + "'";
    }
    VERYHIGH_MSG("%s", debugMsg.c_str());
    return Secure::sha256(concat.c_str(), concat.length());
  }
}// namespace Comms
