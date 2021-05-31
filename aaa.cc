#include <assert.h>

#include <algorithm>
#include <cstring>
#include <map>
#include <utility>
#include <vector>

typedef unsigned short u16;
typedef std::map<u16, std::vector<int>> SegMap;
typedef std::pair<u16, std::vector<int>> SegPair;
typedef std::tuple<u16, std::vector<int>> CompResult;

const int kSegLen = 8;
const int kFindN = 6;

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
      assert(0);
  }
}

// 已有一段seq的 `key` ，和下一个字符 `ch` ，把下一个key求出（向后挪动一格）
static inline u16 encode_forward(int key, char ch) {
  key <<= 2;
  key += encode_one(ch);
  return key;
}

static u16 encode(char *seq) {
  u16 key = 0;
  for (int i = 0; i < kSegLen; i++) {
    key <<= 2;
    key += encode_one(seq[i]);
  }
  return key;
}

static inline void add_map(SegMap &seg_map, u16 key, int loc) {
  auto iter = seg_map.find(key);
  if (iter == seg_map.end()) {
    seg_map[key] = {loc};
  } else {
    int last_loc = *iter->second.rbegin();
    // 避免两个segment重叠在一起
    if (loc >= last_loc + kSegLen) {
      iter->second.push_back(loc);
    }
  }
}

// 遍历每个segment，求出key，放到segmap中，然后找到最多的那个。
static std::tuple<u16, std::vector<int>> find_most_freq(char *seq, int len) {
  SegMap seg_map;
  u16 key = encode(seq);
  add_map(seg_map, key, 0);
  for (int i = 1; i <= len - kSegLen; i++) {
    key = encode_forward(key, seq[i + kSegLen - 1]);
    add_map(seg_map, key, i);
  }
  // 选最大的segment
  auto pair = std::max_element(seg_map.begin(), seg_map.end(),
                               [](const SegPair &p1, const SegPair &p2) {
                                 return p1.second.size() < p2.second.size();
                               });  // 这里用了lamda表达式表示怎么判断最大
  return std::make_tuple(
      pair->first, std::move(pair->second));  // 用move移动，减少复制的开销
}

// 根据 `result` ，把 `seq` 的segment中去掉。在原地改变，并改变长度。
static void remove_repeat(char *seq, int *len, CompResult &result) {
  auto &locs = std::get<1>(result);
  if (locs.empty()) return;
  int prev_loc = -kSegLen;
  char *p = seq, *q = seq;
  for (int loc : locs) {
    int cp_len = loc - prev_loc - kSegLen;
    memcpy(q, p, cp_len);
    q += cp_len;
    p += cp_len + kSegLen;
    prev_loc = loc;
  }
  memcpy(q, p, *len - prev_loc - kSegLen);
  *len -= locs.size() * kSegLen;
}

// 在 `seq` 中找到 `kFindN` 个出现频率最高的长度为 `kSegLen` 的字符串。
// （通过调用find_most_freq一个一个找）
// 把它们从 `seq` 中去掉，同时更新 `len`。
// 并返回他们的内容（CompResult里的第一个）和所有位置（第二个）。
std::vector<CompResult> remove_repeat_n(char *seq, int *len) {
  std::vector<CompResult> result;
  for (int i = 0; i < kFindN; i++) {
    result.push_back(find_most_freq(seq, *len));
    remove_repeat(seq, len, *result.rbegin());
  }
  return result;
}

// 下面是解压缩还原的过程，仅用来测试压缩过程。可以供写那一部分的同学参考。
// 从标准输入读seq，压缩，再解压，输出segment和解压结果（没有读写文件）
#define DEBUG

#ifdef DEBUG

char buf11[1000000];
char buf22[1000000];

void fake_decode_seg(u16 key, char *seg) {
  u16 mask = 3;  // 0000000000000011
  for (int i = kSegLen - 1; i >= 0; i--) {
    u16 idx = mask & key;
    switch (idx) {
      case 0:
        seg[i] = 'A';
        break;
      case 1:
        seg[i] = 'T';
        break;
      case 2:
        seg[i] = 'C';
        break;
      default:
        seg[i] = 'G';
    }
    key >>= 2;
  }
}

// 解压缩时旧的 `seq` 会被加入的segment覆盖，所以不能在一个字符串里解压缩
// 这里交替用两个字符串作为新旧字符串
void fake_decompress(int *len, std::vector<CompResult> &results) {
  char seg[kSegLen];
  char *buf1 = buf11, *buf2 = buf22;
  memcpy(buf2, buf1, *len);
  for (int i = results.size() - 1; i >= 0; i--) {
    CompResult &result = results[i];
    u16 key;
    std::vector<int> locs;
    std::tie(key, locs) = std::move(result);
    fake_decode_seg(key, seg);
    char *p = buf1, *q = buf2;
    int prev_loc = -kSegLen;
    for (int loc : locs) {
      int cp_len = loc - prev_loc - kSegLen;
      memcpy(q, p, cp_len);
      q += cp_len;
      p += cp_len;
      memcpy(q, seg, kSegLen);
      q += kSegLen;
      prev_loc = loc;
    }
    memcpy(q, p, *len - prev_loc);
    // printf("%s\n", buf2);
    *len += locs.size() * kSegLen;
    std::swap(buf1, buf2);
  }
  if (results.size() % 2) {
    memcpy(buf11, buf22, *len);
  }
  buf11[*len] = '\0';
}

int main() {
  scanf("%s", buf11);
  int len = strlen(buf11);
  std::vector<CompResult> results = remove_repeat_n(buf11, &len);
  printf("\n");
  char seg[kSegLen + 1];
  seg[kSegLen] = '\0';
  for (auto &result : results) {
    auto key = std::get<0>(result);
    auto &locs = std::get<1>(result);
    fake_decode_seg(key, seg);
    printf("%s ", seg);
    for (auto loc : locs) {
      printf("%d ", loc);
    }
    printf("\n");
  }
  buf11[len] = '\0';
  printf("%s\n", buf11);
  printf("\n");
  fake_decompress(&len, results);
  printf("%s\n", buf11);
}
#endif
