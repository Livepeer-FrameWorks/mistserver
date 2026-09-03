#include <mist/amf.h>
#include <mist/json.h>

#include <iostream>

// Helper functions for TAP
size_t testCount = 0;
void testRes(bool success, std::string desc, std::function<void()> onFail) {
  std::cout << (success ? "ok" : "not ok") << " " << ++testCount << " - " << desc << std::endl;
  if (!success) { onFail(); }
}

int main(int argc, const char *argv[]) {
  // TAP header line listing test count
  std::cout << "TAP version 14" << std::endl << "1..4" << std::endl;

  AMF::Object A = AMF::parse("\002\000\013onClockSync\003\000\013streamClock\000A\302{\t "
                             "\000\000\000\000\017streamClockBase\000@"
                             "\306\322\200\000\000\000\000\000\twallClock\000A\302{.P\200\000\000\000\000\t",
                             86);
  JSON::Value jData;
  jData["streamClock"] = 620106304;
  jData["streamClockBase"] = 11685;
  jData["wallClock"] = 620125345;
  testRes((A.getContentP(0)->StrValue() == "onClockSync") && (A.getContentP(1)->toJSON() == jData),
          "Clock metadata message", [&]() {
    std::cerr << "Mismatch: " << A.getContentP(0)->StrValue() << " != onClockSync or " << A.getContentP(1)->toJSON()
              << " != " << jData;
  });

  JSON::Value preJSON = JSON::fromString(R"({"0":0, "test":"test", "double":0.1, "array":[0,1,2,3,{"foo":"bar"}]})");
  testRes(AMF::fromJSON(preJSON).toJSON() == preJSON, "AMF<->JSON conversion",
          [&]() { std::cerr << "Mismatch: " << AMF::fromJSON(preJSON).toJSON() << " != " << preJSON; });

  AMF::Object original = AMF::fromJSON(preJSON);
  AMF::Object copied(original);
  copied.getContentP("test")->StrValue() = "changed";
  copied.getContentP("array")->getContentP(4)->getContentP("foo")->StrValue() = "changed";
  testRes(original.getContentP("test")->StrValue() == "test" &&
            original.getContentP("array")->getContentP(4)->getContentP("foo")->StrValue() == "bar",
          "AMF copy construction owns an independent nested object tree",
          [&]() { std::cerr << "Copy mutation changed the source object"; });

  AMF::Object assigned = AMF::fromJSON(std::string(R"({"stale":"value"})"));
  assigned = original;
  assigned.getContentP("array")->getContentP(4)->getContentP("foo")->StrValue() = "assigned";
  testRes(!assigned.hasContent("stale") && original.getContentP("array")->getContentP(4)->getContentP("foo")->StrValue() == "bar",
          "AMF copy assignment replaces and independently owns nested contents",
          [&]() { std::cerr << "Assignment retained stale data or aliased the source object"; });

  return 0;
}
