#include <algorithm>
#include <condition_variable>
#include <cstring>
#include <fstream>
#include <iostream>
#include <map>
#include <mutex>
#include <queue>
#include <string>
#include <thread>
#include <utility>
#include <vector>

typedef unsigned char u8;
typedef unsigned short u16;
typedef std::map<u16, std::vector<int>> SegMap;
typedef std::pair<u16, std::vector<int>> SegPair;
typedef std::unique_lock<std::mutex> MutLock;

const int kThreadN = 8;
const int kMaxBuf = 1024 * 1024;
const int kSegLen = 8;
const int kFindN = 6;
int stage;
std::mutex stage_mu;
std::condition_variable stage_cv;
int finishN;
std::mutex finishN_mu;
std::condition_variable finishN_cv;

char bufs[kThreadN][kMaxBuf + 1];
int bufs_len[kThreadN];
std::vector<SegPair> freqs[kThreadN];
u16 freq_result;

static inline u16 encode_one(char ch) {
  switch (ch) {
    case 'A':
      return 0;
    case 'T':
      return 1;
    case 'C':
      return 2;
    case 'G':
      return 3;
    default:
      std::cerr << "[error]: find undefined char: " << ch << "\n";
      return 0;
  }
}

// 已有一段seq的 `key` ，和下一个字符 `ch` ，把下一个key求出（向后挪动一格）
static inline u16 encode_forward(int key, char ch) {
  key <<= 2;
  key += encode_one(ch);
  return key;
}

static u16 encode(char* seq) {
  u16 key = 0;
  for (int i = 0; i < kSegLen; i++) {
    key <<= 2;
    key += encode_one(seq[i]);
  }
  return key;
}

static inline void adds_map(SegMap& segmap, SegPair& result) {
  auto key = result.first;
  auto& locs = result.second;
  auto iter = segmap.find(key);
  if (iter == segmap.end()) {
    segmap[key] = std::move(locs);
  } else {
    auto& old_locs = iter->second;
    int old_last_loc = *old_locs.rbegin();
    int first_loc = locs[0];
    auto insert_begin = locs.begin();
    if (first_loc < old_last_loc) {
      insert_begin++;
    }
    old_locs.insert(old_locs.end(), insert_begin, locs.end());
  }
}

static inline void add_map(SegMap& segmap, u16 key, int loc) {
  auto iter = segmap.find(key);
  if (iter == segmap.end()) {
    segmap[key] = {loc};
  } else {
    int last_loc = *iter->second.rbegin();
    // 避免两个segment重叠在一起
    if (loc >= last_loc + kSegLen) {
      iter->second.push_back(loc);
    }
  }
}

struct SegPairCmp {
  bool operator()(const SegPair& a, const SegPair& b) {
    return a.second.size() < b.second.size();
  }
};

// 遍历每个segment，求出key，放到segmap中，然后找到最多的那个。
static std::vector<SegPair> find_most_freq(SegMap& segmap, char* seq, int len) {
  u16 key = encode(seq);
  add_map(segmap, key, 0);
  for (int i = 1; i <= len - kSegLen; i++) {
    key = encode_forward(key, seq[i + kSegLen - 1]);
    add_map(segmap, key, i);
  }
  std::priority_queue<SegPair, std::vector<SegPair>, SegPairCmp> pq(
      std::make_move_iterator(segmap.begin()),
      std::make_move_iterator(segmap.end()));
  segmap.clear();
  auto max_ele = pq.top();
  pq.pop();
  int bound = std::max(int(max_ele.second.size() / kThreadN), 1);
  std::vector<SegPair> results;
  segmap.insert(max_ele);
  results.push_back(std::move(max_ele));
  while (!pq.empty() && pq.top().second.size() > bound) {
    segmap.insert(pq.top());
    results.push_back(std::move(pq.top()));
    pq.pop();
  }
  return results;
}

static void remove_freq(const std::vector<int>& locs, char* seq, int& len) {
  int prev_loc = -kSegLen;
  char *p = seq, *q = seq;
  for (int loc : locs) {
    int cp_len = loc - prev_loc - kSegLen;
    memcpy(q, p, cp_len);
    q += cp_len;
    p += cp_len + kSegLen;
    prev_loc = loc;
  }
  memcpy(q, p, len - prev_loc - kSegLen);
  len -= locs.size() * kSegLen;
}

static void thread_routine(int tid) {
  auto& buf = bufs[tid];
  auto& buf_len = bufs_len[tid];
  auto& freq = freqs[tid];
  bool quit = false;
  MutLock stage_lck(stage_mu, std::defer_lock);
  MutLock finishN_lck(finishN_mu, std::defer_lock);
  SegMap segmap;
  int cur_stage = 1;
  while (true) {
    finishN_lck.lock();
    finishN++;
    finishN_cv.notify_one();
    finishN_lck.unlock();
    if (quit) break;
    stage_lck.lock();
    stage_cv.wait(stage_lck, [&quit, cur_stage]() {
      if (stage == -1) quit = true;
      return stage == -1 || stage == cur_stage;
    });
    cur_stage++;
    stage_lck.unlock();
    auto iter = segmap.find(freq_result);
    if (iter != segmap.end()) {
      remove_freq(iter->second, buf, buf_len);
    }
    if (!quit) {
      segmap.clear();
      auto comp_results = find_most_freq(segmap, buf, buf_len);
      int offset = 0;
      for (int i = 0; i < tid; i++) {
        offset += bufs_len[i];
      }
      for (auto& result : comp_results) {
        for (auto& loc : result.second) {
          loc += offset;
        }
      }
      freq = std::move(comp_results);
    }
  }
}

static std::string make_comp_filename(std::string& in_fname) {
  auto dot_pos = in_fname.find_last_of('.');
  if (dot_pos == std::string::npos) {
    return in_fname + "_comp";
  } else {
    return in_fname.substr(0, dot_pos) + "_comp" + in_fname.substr(dot_pos);
  }
}

static void binary_comp(std::ofstream& fout, std::vector<SegPair>& results) {
  int segN = results.size();
  fout << segN;
  for (auto& result : results) {
    fout << result.first;
    fout << result.second.size();
    for (auto loc : result.second) {
      fout << loc;
    }
  }
  int seq_len = 0;
  std::for_each(bufs_len, bufs_len + kThreadN,
                [&seq_len](int len) { seq_len += len; });
  fout << seq_len;
  u8 tt;
  int cnt = 0;
  int buf_idx = 0;
  int idx = 0;
  int buf_len = bufs_len[buf_idx];
  char* cursor = bufs[buf_idx];
  while (true) {
    if (cnt == 4) {
      fout << tt;
      cnt = 0;
    }
    if (idx == buf_len) {
      buf_idx++;
      if (buf_idx == kThreadN) {
        break;
      }
      idx = 0;
      buf_len = bufs_len[buf_idx];
      cursor = bufs[buf_idx];
    }
    tt <<= 2;
    tt += encode_one(*cursor);
    cursor++;
    idx++;
    cnt++;
  }
  fout << tt;
}

int main(int argc, char** argv) {
  if (argc != 2) {
    std::cerr << "usage: ./comp filename.\n";
  }
  std::string in_fname = argv[1];
  std::ifstream fin(in_fname);
  if (!fin.is_open()) {
    std::cerr << "[error]: cannot find the file.\n";
    return 1;
  }
  fin.seekg(0, std::ios::end);
  std::streampos fin_size = fin.tellg();
  fin.seekg(0, std::ios::beg);
  std::vector<std::thread> threads;
  int tid;
  int buf_size = fin_size / kThreadN + 1;
  if (buf_size > kMaxBuf) {
    std::cerr << "[error]: sequence two long. please enlarge the buf.\n";
    return 1;
  }
  for (tid = 0; tid < kThreadN; tid++) {
    threads.push_back(std::thread(thread_routine, tid));
    fin.read(bufs[tid], buf_size);
    bufs_len[tid] = fin.gcount();
  }
  fin.close();
  MutLock finishN_lck(finishN_mu);
  std::vector<SegPair> results;
  stage = 0;
  for (int i = 0; i < kFindN; i++) {
    finishN = 0;
    MutLock stage_lck(stage_mu);
    stage++;
    stage_cv.notify_all();
    stage_lck.unlock();
    finishN_cv.wait(finishN_lck, []() { return finishN >= kThreadN; });
    std::cout << "[main]: finish " << i << ".\n";

    SegMap segmap;
    for (int tid = 0; tid < kThreadN; tid++) {
      for (auto& result : freqs[tid]) {
        adds_map(segmap, result);
      }
    }
    auto result = std::max_element(segmap.begin(), segmap.end(),
                                   [](const SegPair& a, const SegPair& b) {
                                     return a.second.size() < b.second.size();
                                   });
    freq_result = result->first;
    results.push_back(std::move(*result));
  }
  finishN = 0;
  MutLock stage_lck(stage_mu);
  stage = -1;
  stage_cv.notify_all();
  stage_lck.unlock();
  finishN_cv.wait(finishN_lck, []() { return finishN >= kThreadN; });

  std::cout << "[main]: finish task!\n";
  finishN_lck.unlock();
  for (auto& thread : threads) {
    thread.join();
  }
  std::string comp_fname = make_comp_filename(in_fname);
  std::ofstream fout(comp_fname);
  binary_comp(fout, results);
  std::streampos fout_size = fout.tellp();
  fout.close();
  std::cout << "compress OK! compress rate is " << (double)fout_size / fin_size
            << "\n";
  return 0;
}
