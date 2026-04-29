#include <mist/defines.h>
#include <mist/json.h>

namespace Mist{
  JSON::Value opt;

  class ProcThumbs{
  public:
    ProcThumbs(){};
    bool CheckConfig();
    void Run();
  };
}// namespace Mist
