//
// Created by fang on 2025/1/22.
//

#include "bilibili/util/json.hpp"
#include "bilibili/util/http.hpp"

namespace bilibili {
    std::string parseLink(const std::string &url) {
        if (url.rfind("//", 0) == 0) {
            return HTTP::PROTOCOL + url;
        }
        return url;
    }
}
