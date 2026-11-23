#include <mist/config.h>
#include <mist/json.h>
#include <mist/triggers.h>

#include <iostream>
#include <iterator>
#include <unistd.h>

int main(int argc, char **argv) {
  if (Util::Config::answerInformation(argc, argv, "(bounded PROCESS_REPLACE handler worker)")) { return 0; }
  if (argc != 2 || setsid() < 0) { return 2; }
  // No handler may start until the parent can identify this owned process group.
  std::cout << 'R' << std::flush;
  const std::string payload{std::istreambuf_iterator<char>(std::cin), std::istreambuf_iterator<char>()};
  Triggers::Result result;
  Triggers::doTrigger("PROCESS_REPLACE", payload, argv[1], false, result);
  JSON::Value reply;
  reply["success"] = !result.handlerFailed && result.action == Triggers::ACT_VALUE && !result.response.empty();
  reply["response"] = result.response;
  std::cout << reply.toString() << std::flush;
  return 0;
}
