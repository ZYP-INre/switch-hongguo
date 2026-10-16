// SPDX-License-Identifier: GPL-3.0-only
//
// 分片式（DASH / fMP4）单轨的按需读取与解密。
//
// 与 StreamReader 的分工：
//   StreamReader 处理"moov 在前、样本索引在 stbl"的**渐进式** MP4；
//   这里处理"初始化段 + 一串 moof/mdat 分片"的 **fMP4** —— 样本索引在 moof 里，
//   所以必须逐分片解析 trun/senc 才知道每个样本的偏移与 IV。
//
// ⚠ 解密是**就地等长**的：read(a, n) 得到的字节与远端 [a, a+n) 一一对应，
//   偏移完全一致。因此上层（本地代理）拿到的是一份"明文镜像"，
//   播放器照常做 Range 请求即可，不需要任何额外约定。
#pragma once

#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

#include "hongguo/http.hpp"
#include "hongguo/model.hpp"

namespace hongguo {

class DashTrackReader {
 public:
  DashTrackReader(Http& http, DashTrack track, std::optional<Bytes> key);

  // 拉初始化段、改写头部（还原编码格式）、解析分片索引。可重复调用。
  void prepare();

  // 读取 [start, start+length) 的明文。越界时按到文件末尾截断。
  Bytes read(int64_t start, int length);

  // 取一小段明文，**不经过顺序读窗口**。给"只看几百字节头部字段"的探测用：
  // 合成器扫每个分片开头的 tfhd 就是这种场景，走 read() 会为 512 字节白白拉回一个窗口。
  Bytes probe(int64_t start, int length) const;

  int64_t total() const { return track_.total; }
  const DashTrack& track() const { return track_; }
  // sidx 里声明的时间基。ffmpeg 的 DASH 解复用器用它换算媒体时长。
  int64_t timescale() const { return timescale_; }
  const Bytes& header() const { return header_; }
  bool hasKey() const { return key_.has_value() && !key_->empty(); }
  size_t segmentCount() const { return segments_.size(); }
  // 分片表（源轨上的偏移）。合成器按它排布输出流。
  struct SegmentRange {
    int64_t start = 0;
    int64_t size = 0;
  };
  std::vector<SegmentRange> segmentRanges() const;

 private:
  // 分片在原始资源里的字节区间。
  struct Segment {
    int64_t start = 0;
    int64_t size = 0;
  };

  // 一个**加密区间**。子样本加密时一个样本会拆成多段，段与段之间是明文。
  struct EncryptedRun {
    int64_t start = 0;
    int64_t end = 0;
    Bytes iv;  // 16 字节计数器初值（整段共享）
  };

  Bytes fetch(int64_t start, int64_t end) const;
  void parseIndex();
  // 解析某个分片的加密区间（原来的 runsFor 本体，不带缓存）。
  std::vector<EncryptedRun> parseRuns(const Segment& segment) const;
  // 带缓存的分片表查询。播放器是逐段顺序读的，同一个分片会被读很多次
  // （代理一次只吐 128KB，而一个 5 秒分片有一两 MB）—— 不带缓存的话，
  // 每读一次都要把这份几 KB 的 moof 重新从 CDN 拉一遍，白费一次往返。
  const std::vector<EncryptedRun>& runsFor(size_t index) const;
  // 取一段**明文**（已还原编码格式、已就地解密），与远端 [start, end) 一一对应。
  // read() 的窗口缓存底层就是它。
  Bytes plainRange(int64_t start, int64_t end) const;

  Http& http_;
  DashTrack track_;
  std::optional<Bytes> key_;
  Bytes header_;
  std::vector<Segment> segments_;
  int64_t timescale_ = 0;

  // ⚠ 视频与音频两条轨在代理里是**并行**读的，而这里现在自带缓存，
  //   所以读路径必须自己上锁 —— 不能再指望调用方那把锁（合成模式下已经没有全局锁了）。
  mutable std::mutex mutex_;
  mutable std::vector<std::vector<EncryptedRun>> runCache_;
  mutable std::vector<char> runReady_;

  // 顺序读窗口，单位字节。
  //
  // 以前每收一个 128KB 的 Range 就去 CDN 拉一次 —— 一集 175 秒的片子要拉两百多次，
  // 时间全花在"等下一个往返"上（实测整集吞吐只有 400KB/s 上下，而 CDN 直连是它的十几倍）。
  // 现在一次多拉一点、按窗口留着：请求数直接降一个量级。
  //
  // ⚠ 窗口不能太大。首字节要等这一整块拉回来才发得出去，而 local_proxy 里
  //   kChunkSize 那段注释记着同一个取舍：CDN 慢的时候，块越大越容易把播放器等超时。
  //   256KB 是"请求数减半"与"首字节别超过一两秒"之间的折中。
  static constexpr int64_t kWindowBytes = 256 * 1024;
  mutable Bytes window_;
  mutable int64_t windowStart_ = 0;
};

// 把一集的两条 DASH 轨合成**一条**普通 fMP4。
//
// 为什么不走 DASH：实测目标机的 ffmpeg（devkitPro 的 libavformat）**没有编译 dash 解复用器**
//   （nm 里只有 ff_mov_demuxer / ff_mpegts_demuxer，没有 ff_dash_demuxer）。
//   也就是说 MPD 这条路在 Switch 上根本走不通。而 mov 解复用器是有的，
//   所以干脆在本地把两条轨拼成一个双轨 fMP4 —— 播放器那边看不出任何区别。
//
// 拼法（全部是"搬 box + 改两个 track_ID"，不做重编码、不重排时间）：
//   * 头部：ftyp + moov( mvhd + 视频 trak + 音频 trak(改名) + mvex(两个 trex) )
//   * 分片：视频分片与音频分片按序号交替拼接；音频的 tfhd 里 track_ID 要改
//
// ⚠ 输出流的偏移是**合成后**的偏移，和源轨偏移不是一回事 —— 所以这里维护一张
//   分片表（Piece），read() 按表去各源轨取数据。偏移映射集中在这一处，别处不要假设。
class DashMuxer {
 public:
  DashMuxer(DashTrackReader& video, DashTrackReader* audio);

  void prepare();
  int64_t total() const { return total_; }
  Bytes read(int64_t start, int length);
  size_t pieceCount() const { return pieces_.size(); }
  int64_t headerSize() const { return static_cast<int64_t>(header_.size()); }

 private:
  struct Piece {
    int64_t outStart = 0;
    int64_t length = 0;
    int source = 0;        // 0 = 合成头部（已在内存里）
    int64_t sourceOffset = 0;
    // 音频分片要把 tfhd 里的 track_ID 改成新值。字段的**源轨绝对偏移**在
    // prepare 阶段就扫好了 —— 只记 flag 的话，等到 read() 只有半个分片的字节，
    // 根本找不到 tfhd 在哪几个字节上。
    int64_t patchAt = -1;
  };

  DashTrackReader& video_;
  DashTrackReader* audio_ = nullptr;
  Bytes header_;
  std::vector<Piece> pieces_;
  int64_t total_ = 0;
  int audioTrackId_ = 0;
  int videoTrackId_ = 0;
};

}  // namespace hongguo
