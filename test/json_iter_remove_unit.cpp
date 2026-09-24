#include <mist/json.h>

#include <iostream>
#include <map>
#include <set>
#include <string>

namespace {
  int failures = 0;

  void check(bool ok, const std::string & message) {
    if (ok) { return; }
    std::cerr << "FAIL: " << message << std::endl;
    ++failures;
  }

  JSON::Value makeArray(size_t count) {
    JSON::Value arr;
    for (size_t i = 0; i < count; ++i) { arr.append((int64_t)i); }
    return arr;
  }

  JSON::Value makeObject(size_t count) {
    JSON::Value obj;
    for (size_t i = 0; i < count; ++i) { obj[std::string(1, (char)('a' + i))] = (int64_t)i; }
    return obj;
  }

  /// Iterates val with jsonForEach, removing every element whose integer value is in drop.
  /// Returns how often each value was visited and checks num() matches the array position.
  std::map<int64_t, size_t> removeWhile(JSON::Value & val, const std::set<int64_t> & drop, const std::string & label) {
    std::map<int64_t, size_t> visits;
    size_t position = 0;
    jsonForEach (val, it) {
      const int64_t v = it->asInt();
      ++visits[v];
      if (val.isArray()) { check(it.num() == position, label + ": num() does not match the element position"); }
      if (drop.count(v)) {
        it.remove();
      } else {
        ++position;
      }
    }
    return visits;
  }

  void expectAllOnce(const std::map<int64_t, size_t> & visits, size_t count, const std::string & label) {
    check(visits.size() == count, label + ": not every element was visited");
    for (std::map<int64_t, size_t>::const_iterator it = visits.begin(); it != visits.end(); ++it) {
      check(it->second == 1, label + ": element " + std::to_string(it->first) + " visited " + std::to_string(it->second) + " times");
    }
  }

  void expectRemaining(const JSON::Value & val, const std::set<int64_t> & drop, size_t count, const std::string & label) {
    check(val.size() == count - drop.size(), label + ": wrong number of remaining elements");
    std::set<int64_t> seen;
    jsonForEachConst (val, it) {
      check(!drop.count(it->asInt()), label + ": a removed element is still present");
      seen.insert(it->asInt());
    }
    check(seen.size() == count - drop.size(), label + ": remaining elements are not distinct");
  }

  void runCase(const std::set<int64_t> & drop, const std::string & label) {
    const size_t count = 5;
    JSON::Value arr = makeArray(count);
    expectAllOnce(removeWhile(arr, drop, "array " + label), count, "array " + label);
    expectRemaining(arr, drop, count, "array " + label);
    check(arr.isArray(), "array " + label + ": removing elements changed the value type");

    JSON::Value obj = makeObject(count);
    expectAllOnce(removeWhile(obj, drop, "object " + label), count, "object " + label);
    expectRemaining(obj, drop, count, "object " + label);
    check(obj.isObject(), "object " + label + ": removing elements changed the value type");
  }
} // namespace

int main() {
  runCase({0}, "remove first");
  runCase({2}, "remove middle");
  runCase({4}, "remove last");
  runCase({1, 2}, "remove adjacent middle pair");
  runCase({0, 1, 2, 3, 4}, "remove all");

  // Nested loop over another container after a remove, then continue (controller_api JWK pattern).
  {
    JSON::Value arr = makeArray(4);
    JSON::Value other = makeArray(3);
    std::map<int64_t, size_t> visits;
    jsonForEach (arr, it) {
      ++visits[it->asInt()];
      if (it->asInt() % 2 == 0) {
        it.remove();
        jsonForEach (other, jt) {
          if (jt->asInt() == 1) {
            jt.remove();
            break;
          }
        }
        continue;
      }
    }
    expectAllOnce(visits, 4, "remove then continue");
    check(arr.size() == 2 && arr[0u].asInt() == 1 && arr[1u].asInt() == 3, "remove then continue: wrong survivors");
    check(other.size() == 2 && other[0u].asInt() == 0 && other[1u].asInt() == 2, "nested remove then break: wrong survivors");
  }

  // A remove on an exhausted iterator is a no-op.
  {
    JSON::Value arr = makeArray(1);
    JSON::Iter it(arr);
    ++it;
    it.remove();
    check(arr.size() == 1, "remove past the end must not change the container");
  }

  if (failures) {
    std::cerr << failures << " failure(s)" << std::endl;
    return 1;
  }
  return 0;
}
