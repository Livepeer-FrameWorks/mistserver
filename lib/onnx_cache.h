#pragma once

#include <memory>
#include <string>

namespace ONNX {
  namespace ORTHelpers {
    class OpenVINOCache {
      public:
        static std::unique_ptr<OpenVINOCache> prepare(const std::string & root, const std::string & model,
                                                      const std::string & version, const std::string & recipe, std::string & why);
        ~OpenVINOCache();
        OpenVINOCache(const OpenVINOCache &) = delete;
        OpenVINOCache & operator=(const OpenVINOCache &) = delete;
        const std::string & directory() const { return working_; }
        void complete(bool successful);

      private:
        OpenVINOCache() = default;
        std::string root_, entry_, working_, initial_;
        int compiler_ = -1;
        int lease_ = -1;
        bool hit_ = false;
    };
  } // namespace ORTHelpers
} // namespace ONNX
