#include "controller_i18n.h"
#include "translations.vfs.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <map>
#include <utility>
#include <vector>

namespace Controller {

  typedef std::map<std::string, std::string> Catalog;
  typedef std::map<std::string, Catalog> Catalogs;

  static std::string lower(std::string value) {
    for (size_t i = 0; i < value.size(); ++i) { value[i] = std::tolower((unsigned char)value[i]); }
    return value;
  }

  static std::string baseLanguage(const std::string &value) {
    size_t separator = value.find('-');
    return separator == std::string::npos ? value : value.substr(0, separator);
  }

  static const Catalogs &catalogs() {
    static const Catalogs value = []() {
      Catalogs loaded;
      static const char *languages[] = {"de-DE", "es-ES"};
      for (size_t i = 0; i < sizeof(languages) / sizeof(languages[0]); ++i) {
        const std::string path = std::string("/backend/") + languages[i] + ".json";
        const EmbeddedFile *entry = vfsLookup(translations_vfs, translations_vfs_count, path.c_str());
        if (!entry) continue;
        JSON::Value parsed;
        parsed.fromString(std::string(entry->data, entry->data_len));
        Catalog &catalog = loaded[languages[i]];
        jsonForEachConst(parsed, item) {
          if (item->isString()) catalog[item.key()] = item->asStringRef();
        }
      }
      return loaded;
    }();
    return value;
  }

  std::string tr(const std::string &message, const std::string &lang) {
    if (lang.empty() || lang == "en") return message;
    Catalogs::const_iterator catalog = catalogs().find(lang);
    if (catalog == catalogs().end()) return message;
    Catalog::const_iterator translated = catalog->second.find(message);
    if (translated == catalog->second.end() || translated->second.empty()) return message;
    return translated->second;
  }

  std::string resolveLang(const HTTP::Parser &header) {
    std::string accept = header.GetHeader("Accept-Language");
    if (accept.empty()) return "en";
    std::vector<std::pair<double, std::string> > candidates;
    size_t position = 0;
    while (position < accept.size()) {
      size_t comma = accept.find(',', position);
      std::string part = accept.substr(position, comma == std::string::npos ? std::string::npos : comma - position);
      position = comma == std::string::npos ? accept.size() : comma + 1;
      size_t semicolon = part.find(';');
      std::string code = part.substr(0, semicolon);
      while (!code.empty() && std::isspace((unsigned char)code[0])) code.erase(0, 1);
      while (!code.empty() && std::isspace((unsigned char)code[code.size() - 1])) code.erase(code.size() - 1);
      double quality = 1.0;
      size_t q = part.find("q=", semicolon == std::string::npos ? 0 : semicolon);
      if (q != std::string::npos) quality = std::atof(part.c_str() + q + 2);
      if (!code.empty() && code != "*" && quality > 0) candidates.push_back(std::make_pair(-quality, code));
    }
    std::stable_sort(candidates.begin(), candidates.end());
    for (size_t i = 0; i < candidates.size(); ++i) {
      const std::string wanted = lower(candidates[i].second);
      if (wanted == "en" || wanted.find("en-") == 0) return "en";
      for (Catalogs::const_iterator catalog = catalogs().begin(); catalog != catalogs().end(); ++catalog) {
        if (lower(catalog->first) == wanted) return catalog->first;
      }
      const std::string base = baseLanguage(wanted);
      for (Catalogs::const_iterator catalog = catalogs().begin(); catalog != catalogs().end(); ++catalog) {
        if (baseLanguage(lower(catalog->first)) == base) return catalog->first;
      }
    }
    return "en";
  }

  static void translateString(JSON::Value &object, const char *key, const std::string &lang) {
    if (object.isMember(key) && object[key].isString()) object[key] = tr(object[key].asStringRef(), lang);
  }

  static void translateStringValues(JSON::Value &value, const std::string &lang) {
    if (value.isString()) {
      value = tr(value.asStringRef(), lang);
      return;
    }
    if (!value.isObject() && !value.isArray()) return;
    jsonForEach(value, item) { translateStringValues(*item, lang); }
  }

  static void translateFieldTree(JSON::Value &tree, const std::string &lang) {
    if (!tree.isObject() && !tree.isArray()) return;
    jsonForEach(tree, field) {
      if (!field->isObject()) {
        if (field->isArray()) translateFieldTree(*field, lang);
        continue;
      }
      translateString(*field, "name", lang);
      translateString(*field, "help", lang);
      if (field->isMember("select") && (*field)["select"].isArray()) {
        jsonForEach((*field)["select"], option) {
          if (option->isArray() && option->size() > 1 && (*option)[1u].isString()) {
            (*option)[1u] = tr((*option)[1u].asStringRef(), lang);
          }
        }
      }
      if (field->isMember("options")) translateFieldTree((*field)["options"], lang);
      translateFieldTree(*field, lang);
    }
  }

  void translateCapabilities(JSON::Value &capabilities, const std::string &lang) {
    if (lang.empty() || lang == "en") return;
    static const char *categories[] = {"connectors", "inputs", "processes"};
    for (size_t category = 0; category < sizeof(categories) / sizeof(categories[0]); ++category) {
      if (!capabilities.isMember(categories[category])) continue;
      jsonForEach(capabilities[categories[category]], plugin) {
        if (!plugin->isObject()) continue;
        translateString(*plugin, "friendly", lang);
        translateString(*plugin, "desc", lang);
        translateString(*plugin, "hrn", lang);
        if (plugin->isMember("source_help")) translateStringValues((*plugin)["source_help"], lang);
        if (plugin->isMember("methods")) {
          jsonForEach((*plugin)["methods"], method) { translateString(*method, "hrn", lang); }
        }
        if (plugin->isMember("required")) translateFieldTree((*plugin)["required"], lang);
        if (plugin->isMember("optional")) translateFieldTree((*plugin)["optional"], lang);
        if (plugin->isMember("ainfo")) translateFieldTree((*plugin)["ainfo"], lang);
      }
    }
    if (capabilities.isMember("triggers")) {
      jsonForEach(capabilities["triggers"], trigger) {
        translateString(*trigger, "when", lang);
        translateString(*trigger, "payload", lang);
        translateString(*trigger, "argument", lang);
        translateString(*trigger, "response_action", lang);
      }
    }
  }
}
