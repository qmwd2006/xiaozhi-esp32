#pragma once
#include <deque>
#include <string>
#include <vector>

namespace xz {

enum class Repeat { kOff = 0, kList = 1, kOne = 2 };

const char* RepeatName(Repeat r);

// 队列状态机：与固件 Application 里的 music_queue_/music_index_/music_repeat_mode_ 等价
class MusicQueue {
 public:
    void Set(const std::vector<std::string>& urls, const std::vector<std::string>& titles,
             int start_index);
    void Enqueue(const std::string& url, const std::string& title);
    bool PlayIndex(int index);
    bool Next();
    bool Prev();
    void Clear();

    int Length() const { return static_cast<int>(urls_.size()); }
    int Index() const { return index_; }
    void SetIndex(int i) { index_ = i; }
    std::string UrlAt(int i) const;
    std::string TitleAt(int i) const;
    std::string CurrentUrl() const { return UrlAt(index_); }
    std::string CurrentTitle() const { return TitleAt(index_); }

    void SetRepeat(Repeat r) { repeat_ = r; }
    Repeat GetRepeat() const { return repeat_; }

    // {"length":n,"current_index":i,"repeat":"list","items":[{index,title,url}]}
    std::string Snapshot(int limit) const;

 private:
    std::deque<std::string> urls_;
    std::deque<std::string> titles_;
    int index_ = -1;
    Repeat repeat_ = Repeat::kList;
};

}  // namespace xz