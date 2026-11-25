#include "xz/music_queue.h"

#include "xz/json_util.h"

namespace xz {

const char* RepeatName(Repeat r) {
    switch (r) {
        case Repeat::kOne: return "one";
        case Repeat::kOff: return "off";
        default: return "list";
    }
}

void MusicQueue::Set(const std::vector<std::string>& urls,
                     const std::vector<std::string>& titles, int start_index) {
    urls_.assign(urls.begin(), urls.end());
    titles_.clear();
    for (size_t i = 0; i < urls_.size(); ++i) {
        titles_.push_back(i < titles.size() ? titles[i] : std::string());
    }
    if (urls_.empty()) {
        index_ = -1;
    } else {
        index_ = (start_index < 0 || start_index >= static_cast<int>(urls_.size()))
                     ? 0 : start_index;
    }
}

void MusicQueue::Enqueue(const std::string& url, const std::string& title) {
    if (url.empty()) return;
    urls_.push_back(url);
    titles_.push_back(title);
    if (index_ < 0) index_ = 0;
}

bool MusicQueue::PlayIndex(int index) {
    if (index < 0 || index >= static_cast<int>(urls_.size())) return false;
    index_ = index;
    return true;
}

bool MusicQueue::Next() {
    if (urls_.empty()) return false;
    index_ = (index_ + 1) % static_cast<int>(urls_.size());
    return true;
}

bool MusicQueue::Prev() {
    if (urls_.empty()) return false;
    int n = static_cast<int>(urls_.size());
    index_ = (index_ - 1 + n) % n;
    return true;
}

void MusicQueue::Clear() {
    urls_.clear();
    titles_.clear();
    index_ = -1;
}

std::string MusicQueue::UrlAt(int i) const {
    if (i < 0 || i >= static_cast<int>(urls_.size())) return "";
    return urls_[i];
}

std::string MusicQueue::TitleAt(int i) const {
    if (i < 0 || i >= static_cast<int>(titles_.size())) return "";
    return titles_[i];
}

std::string MusicQueue::Snapshot(int limit) const {
    if (limit <= 0) limit = 20;
    int n = static_cast<int>(urls_.size());
    std::string out = "{\"length\":" + std::to_string(n) +
                      ",\"current_index\":" + std::to_string(index_) +
                      ",\"repeat\":\"" + RepeatName(repeat_) + "\",\"items\":[";
    for (int i = 0; i < n && i < limit; ++i) {
        if (i) out += ',';
        out += "{\"index\":" + std::to_string(i) +
               ",\"title\":" + Quote(TitleAt(i)) +
               ",\"url\":" + Quote(UrlAt(i)) + "}";
    }
    out += "]}";
    return out;
}

}  // namespace xz