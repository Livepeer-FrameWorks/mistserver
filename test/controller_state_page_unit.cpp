#include <mist/comms.h>
#include <mist/defines.h>
#include <mist/shared_memory.h>
#include <mist/util.h>

#include <iostream>
#include <string>
#include <unistd.h>

// A rolling controller restart leaves its state pages (logs, access log, streams) in place. A
// build whose layout adds a field must not keep using a page without it.
namespace {
  int failures = 0;

  void check(bool ok, const std::string & what) {
    if (ok) {
      std::cout << "ok   " << what << std::endl;
      return;
    }
    std::cerr << "FAIL " << what << std::endl;
    ++failures;
  }

  void unlinkPage(const std::string & name) {
    IPC::sharedPage page(name, 0, false, false);
    if (page) { page.master = true; }
  }

  const size_t pageSize = 64 * 1024;

  const std::vector<Comms::StateField> previousFields = {
    {"stream", RAX_128STRING, 0}, {"status", RAX_UINT, 1}, {"viewers", RAX_64UINT, 0}, {"tags", RAX_512STRING, 0}};
  const std::vector<Comms::StateField> currentFields = {{"stream", RAX_128STRING, 0},
                                                        {"status", RAX_UINT, 1},
                                                        {"viewers", RAX_64UINT, 0},
                                                        {"unspecified", RAX_64UINT, 0},
                                                        {"tags", RAX_512STRING, 0}};
} // namespace

int main() {
  const std::string name = "/MstTestStatePage" + std::to_string(getpid());
  unlinkPage(name);

  // The previous controller's page: a ring of four records, of which numbers 6 to 9 remain.
  IPC::sharedPage previous(name, pageSize, true);
  previous.master = false;
  Util::RelAccX old(previous.mapped, false);
  for (const Comms::StateField & f : previousFields) { old.addField(f.name, f.type, f.size); }
  old.setRCount(4);
  old.setReady();
  for (uint64_t n = 0; n < 10; ++n) {
    if (old.getPresent() >= 4) { old.deleteRecords(1); }
    old.setString("stream", "stream" + std::to_string(n), n);
    old.setInt("viewers", n * 10, n);
    old.setString("tags", "tag" + std::to_string(n), n);
    old.addRecords(1);
  }

  {
    IPC::sharedPage page;
    Util::RelAccX rax;
    check(Comms::openStatePage(page, rax, name, pageSize, pageSize, currentFields), "the state page opens");
    check(rax.isReady() && rax.hasField("unspecified"), "a page that lacks a field is recreated with it");
    check(old.isReload(), "the previous page is flagged for reload");
    bool copied = rax.getDeleted() == 6 && rax.getEndPos() == 10 && rax.getPresent() == 4;
    for (uint64_t n = 6; n < 10; ++n) {
      copied &= std::string(rax.getPointer("stream", n)) == "stream" + std::to_string(n);
      copied &= rax.getInt("viewers", n) == n * 10;
      copied &= std::string(rax.getPointer("tags", n)) == "tag" + std::to_string(n);
      copied &= rax.getInt("unspecified", n) == 0;
    }
    check(copied, "the remaining records keep their numbers and shared fields");
    rax.setInt("unspecified", 77, 9);
  }

  {
    IPC::sharedPage page;
    Util::RelAccX rax;
    check(Comms::openStatePage(page, rax, name, pageSize, pageSize, currentFields) && !rax.isReload() &&
            rax.getInt("unspecified", 9) == 77 && rax.getEndPos() == 10,
          "a page with this build's layout is reused as is");
  }

  {
    IPC::sharedPage page;
    Util::RelAccX rax;
    std::vector<Comms::StateField> widerTags = currentFields;
    widerTags.back().type = RAX_256STRING;
    check(Comms::openStatePage(page, rax, name, pageSize, pageSize, widerTags) &&
            rax.getFieldData("tags").size == 256 && std::string(rax.getPointer("tags", 9)) == "tag9",
          "a field whose size changed is recreated too");
  }

  unlinkPage(name);
  return failures ? 1 : 0;
}
