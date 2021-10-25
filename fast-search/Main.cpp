//===----------------------------------------------------------------------===//
//
//                     Fast Search for Comic Message
//
//===----------------------------------------------------------------------===//
//
//  Copyright (C) 2021. violet-dev. All Rights Reserved.
//
//===----------------------------------------------------------------------===//

#include <algorithm>
#include <chrono>
#include <cmath>
#include <ctime>
#include <functional>
#include <iostream>
#include <locale>
#include <map>
#include <mutex>
#include <sstream>
#include <string>

#include "Displant.h"
#include "httplib.h"
#include "rapidfuzz/fuzz.hpp"
#include "rapidfuzz/utils.hpp"
#include "simdjson.h"

using namespace simdjson;

template <typename Sentence1, typename Iterable,
          typename Sentence2 = typename Iterable::value_type>
std::vector<std::pair<Sentence2, rapidfuzz::percent>>
extract_similar(const Sentence1 &query, const Iterable &choices,
                const rapidfuzz::percent score_cutoff = 0.0) {
  std::vector<std::pair<Sentence2, rapidfuzz::percent>> results(choices.size());

  auto scorer = rapidfuzz::fuzz::CachedRatio<Sentence1>(query);

#pragma omp parallel for
  for (std::size_t i = 0; i < choices.size(); ++i) {
    double score = scorer.ratio(choices[i]->message, score_cutoff);
    results[i] = std::make_pair(choices[i], score);
  }

  return results;
}

template <typename Sentence1, typename Iterable,
          typename Sentence2 = typename Iterable::value_type>
std::vector<std::pair<Sentence2, rapidfuzz::percent>>
extract_partial_contains(const Sentence1 &query, const Iterable &choices,
                         const rapidfuzz::percent score_cutoff = 0.0) {
  std::vector<std::pair<Sentence2, rapidfuzz::percent>> results(choices.size());
  auto query_len = query.length();

  // #pragma omp parallel for
  //   for (std::size_t i = 0; i < choices.size(); ++i) {
  //     if (choices[i]->message.length() < query_len) {
  //       results[i] = std::make_pair(choices[i], 0.0);
  //       continue;
  //     }
  //     double score = rapidfuzz::fuzz::partial_ratio(query,
  //     choices[i]->message,
  //                                                   score_cutoff);
  //     results[i] = std::make_pair(choices[i], score);
  //   }

  //   return results;

  auto scorer = rapidfuzz::fuzz::CachedPartialRatio<Sentence1>(query);

#pragma omp parallel for
  for (std::size_t i = 0; i < choices.size(); ++i) {
    if (choices[i]->message.length() < query_len) {
      results[i] = std::make_pair(choices[i], 0.0);
      continue;
    }
    double score = scorer.ratio(choices[i]->message, score_cutoff);
    results[i] = std::make_pair(choices[i], score);
  }

  return results;
}

template <typename Sentence1, typename Iterable,
          typename Sentence2 = typename Iterable::value_type>
std::vector<std::pair<Sentence2, rapidfuzz::percent>>
extract_lcs(const Sentence1 &query, const Iterable &choices,
                           const rapidfuzz::percent score_cutoff = 0.0) {
  std::vector<std::pair<Sentence2, rapidfuzz::percent>> results(choices.size());
  auto query_len = query.length();

  auto scorer = rapidfuzz::fuzz::CachedRatio<Sentence1>(query);

#pragma omp parallel for
  for (std::size_t i = 0; i < choices.size(); ++i) {
    auto s_len = choices[i]->message.length();
    if (s_len < query_len) {
      results[i] = std::make_pair(choices[i], 0.0);
      continue;
    }

    double score = scorer.ratio(choices[i]->message, score_cutoff);

    // s2길이 > s1길이 일때
    // score값 - (s2길이 - s1길이)이 실질적인 score값임
    // s2의 길이가 s1보다 1만큼 길면 score값이 무조건 1만큼 커지게됨
    // 이걸 보정해주는게 (s2길이 - s2길이)임
    double ed = (100 - score) / 100 * (query_len + s_len);
    double lcs = (query_len + s_len - ed) / 2;

    results[i] = std::make_pair(choices[i], lcs / query_len);
  }

  return results;
}

template <typename Sentence1, typename Iterable,
          typename Sentence2 = typename Iterable::value_type>
std::vector<std::pair<Sentence2, rapidfuzz::percent>>
extract_regional_partial_contains(const Sentence1 &query,
                                  const Iterable &choices,
                                  const rapidfuzz::percent score_cutoff = 0.0) {
  std::vector<std::pair<Sentence2, rapidfuzz::percent>> results(choices.size());
  auto query_len = query.length();

  auto scorer1 = rapidfuzz::fuzz::CachedRatio<Sentence1>(query);
  auto scorer2 = rapidfuzz::fuzz::CachedPartialRatio<Sentence1>(query);

#pragma omp parallel for
  for (std::size_t i = 0; i < choices.size(); ++i) {
    if (choices[i]->message.length() < query_len) {
      results[i] = std::make_pair(choices[i], 0.0);
      continue;
    }
    double score1 = scorer1.ratio(choices[i]->message, score_cutoff);
    double score2 = scorer2.ratio(choices[i]->message, score_cutoff);

    double score1_rev = (1 - score1 / 100) * (choices[i]->message + query_len);

    results[i] = std::make_pair(choices[i], score2);
  }

  return results;
}

int lcs(char *x, char *y, int m, int n) {
  int L[m + 1][n + 1];
  int i, j;

  for (i = 0; i <= m; i++)
    for (j = 0; j <= n; j++)
      if (i == 0 || j == 0)
        L[i][j] = 0;
      else if (x[i - 1] == y[j - 1])
        L[i][j] = L[i - 1][j - 1] + 1;
      else
        L[i][j] = std::max(L[i - 1][j], L[i][j - 1]);

  return L[m][n];
}

// std::vector<std::pair<std::string, double>>
// extract_lcs_contains(const std::string &query,
//                      const std::vector<MergedInfo *> &choices) {
//   // generate inverse string map

//   // calculate lcs
// }

std::map<std::string, std::string> cacheSimilar;
std::map<std::string, int> cacheSimilarHit;
std::map<std::string, std::string> cacheContains;
std::map<std::string, int> cacheContainsHit;
std::map<std::string, std::string> cacheLCS;
std::map<std::string, int> cacheLCSHit;

typedef struct _MergedInfo {
  int articleid;
  double page;
  std::string message;
  double score;
  std::vector<double> rects;

  _MergedInfo(int articleid, double page, std::string message, double score,
              std::vector<double> rects)
      : articleid(articleid), page(page), message(message), score(score),
        rects(rects) {}

  static struct _MergedInfo *create(int articleid, double page,
                                    std::string message, double score,
                                    std::vector<double> rects) {
    return new _MergedInfo(articleid, page, message, score, rects);
  }
} MergedInfo;

//
//  Merged 문장이 들어있는 벡터
//
std::vector<MergedInfo *> m_infos;

//
//  그냥 replace 함수
//
std::string replace_all(const std::string &message, const std::string &pattern,
                        const std::string &replace) {
  std::string result = message;
  std::string::size_type pos = 0;
  while ((pos = result.find(pattern, pos)) != std::string::npos) {
    result.replace(pos, pattern.size(), replace);
    pos += replace.length();
  }
  return result;
}

//
//  파일 이름에서 id를 가져옴
//  123456-merged.txt => 123456
//
std::string get_id(std::string filename) {
  return strtok((char *)filename.c_str(), "-");
}

void load_json() {
  //
  //  merged.json 파일 로드
  //
  ondemand::parser parser;
  padded_string json = padded_string::load("merged.json");
  ondemand::document tweets = parser.iterate(json);

  for (auto x : tweets) {
    auto id = x["ArticleId"].get_int64();
    auto page = x["Page"].get_double();
    auto msg = std::string_view(x["Message"]);
    auto score = x["Score"].get_double();
    std::vector<double> rects;
    rects.push_back(x["Rectangle"].at(0).get_double());
    rects.push_back(x["Rectangle"].at(1).get_double());
    rects.push_back(x["Rectangle"].at(2).get_double());
    rects.push_back(x["Rectangle"].at(3).get_double());

    m_infos.push_back(MergedInfo::create(
        (int)id.value(), page.value(), std::string(msg), score.value(), rects));
  }
}

std::mutex mutex_similar;
std::mutex mutex_contains;
std::mutex mutex_lcs;

// https://stackoverflow.com/questions/997946/how-to-get-current-time-and-date-in-c
const std::string currentDateTime() {
  time_t now = time(0);
  struct tm tstruct;
  char buf[80];
  tstruct = *localtime(&now);
  // Visit http://en.cppreference.com/w/cpp/chrono/c/strftime
  // for more information about date/time format
  strftime(buf, sizeof(buf), "%Y-%m-%d.%X", &tstruct);

  return buf;
}

void route_similar(const httplib::Request &req, httplib::Response &res,
                   bool use_cache = true, int count = 15) {
  auto query = req.matches[1];

  if (strlen(query.first.base()) == 0) {
    res.set_content("", "text/json");
    return;
  }

  std::cout << "(" << currentDateTime() << ") similar: " << query.first.base()
            << " | " << std::endl;

  //
  //  HangulConverter가 wchar_t 기반으로 구현되어 있어서
  //  utf-8 => unicode 해줘야함
  //
  auto search = std::string(query.first.base());
  wchar_t unicode[1024];
  std::mbstowcs(unicode, query.first.base(), 1024);
  char kor2engtypo[1024 * 3];
  Utility::HangulConverter::total_disassembly(unicode, kor2engtypo);
  auto target = std::string(kor2engtypo);

  if (use_cache && cacheSimilar.find(target) != cacheSimilar.end()) {
    mutex_similar.lock();
    cacheSimilarHit.find(search)->second =
        cacheSimilarHit.find(search)->second + 1;
    mutex_similar.unlock();
    res.set_content(cacheSimilar.find(target)->second, "text/json");
    return;
  }

  //
  //  비슷한 문장들 병렬로 빠르게 추출함
  //
  auto r = extract_similar(target, m_infos);
  std::sort(r.begin(), r.end(), [](auto first, auto second) -> bool {
    return first.second > second.second;
  });

  //
  //  결과는 5개만 보내자
  //
  std::stringstream result;
  int m = 0;
  result << "[";
  for (auto i : r) {
    result << "{";
    result << "\"MatchScore\":\"" << i.second << "\",";
    result << "\"Id\":" << i.first->articleid << ",";
    result << "\"Page\":" << (int)i.first->page << ",";
    result << "\"Correctness\":" << i.first->score << ",";
    result << "\"Rect\":[" << i.first->rects[0] << "," << i.first->rects[1]
           << "," << i.first->rects[2] << "," << i.first->rects[3] << "]";
    result << "}";
    if (m++ == count)
      break;
    result << ",";
  }
  result << "]";

  std::string json(std::istreambuf_iterator<char>(result), {});

  mutex_similar.lock();
  if (use_cache && cacheSimilar.find(target) == cacheSimilar.end()) {
    cacheSimilar.insert({target, json});
    cacheSimilarHit.insert({search, 0});
  }
  mutex_similar.unlock();

  res.set_content(json, "text/json");
}

void route_contains(const httplib::Request &req, httplib::Response &res,
                    bool use_cache = true, int count = 50) {
  auto query = req.matches[1];

  if (strlen(query.first.base()) == 0) {
    res.set_content("", "text/json");
    return;
  }

  std::cout << "(" << currentDateTime() << ") contains: " << query.first.base()
            << " | " << std::endl;

  auto search = std::string(query.first.base());
  wchar_t unicode[1024];
  std::mbstowcs(unicode, query.first.base(), 1024);
  char kor2engtypo[1024 * 3];
  Utility::HangulConverter::total_disassembly(unicode, kor2engtypo);
  auto target = std::string(kor2engtypo);

  if (use_cache && cacheContains.find(target) != cacheContains.end()) {
    mutex_contains.lock();
    cacheContainsHit.find(search)->second =
        cacheContainsHit.find(search)->second + 1;
    mutex_contains.unlock();
    res.set_content(cacheContains.find(target)->second, "text/json");
    return;
  }

  auto r = extract_partial_contains(target, m_infos);
  std::sort(r.begin(), r.end(), [](auto first, auto second) -> bool {
    if (first.second != second.second)
      return first.second > second.second;
    return first.first->message.length() < second.first->message.length();
  });

  std::stringstream result;
  int m = 0;
  result << "[";
  for (auto i : r) {
    result << "{";
    result << "\"MatchScore\":\"" << i.second << "\",";
    result << "\"Id\":" << i.first->articleid << ",";
    result << "\"Page\":" << (int)i.first->page << ",";
    result << "\"Correctness\":" << i.first->score << ",";
    result << "\"Rect\":[" << i.first->rects[0] << "," << i.first->rects[1]
           << "," << i.first->rects[2] << "," << i.first->rects[3] << "]";
    result << "}";
    if (m++ == count)
      break;
    result << ",";
  }
  result << "]";

  std::string json(std::istreambuf_iterator<char>(result), {});

  mutex_contains.lock();
  if (use_cache && cacheContains.find(target) == cacheContains.end()) {
    cacheContains.insert({target, json});
    cacheContainsHit.insert({search, 0});
  }
  mutex_contains.unlock();

  res.set_content(json, "text/json");
}

void route_lcs(const httplib::Request &req, httplib::Response &res,
                      bool use_cache = true, int count = 50) {
  auto query = req.matches[1];

  if (strlen(query.first.base()) == 0) {
    res.set_content("", "text/json");
    return;
  }

  std::cout << "(" << currentDateTime() << ") containsh: " << query.first.base()
            << " | " << std::endl;

  auto search = std::string(query.first.base());
  wchar_t unicode[1024];
  std::mbstowcs(unicode, query.first.base(), 1024);
  char kor2engtypo[1024 * 3];
  Utility::HangulConverter::total_disassembly(unicode, kor2engtypo);
  auto target = std::string(kor2engtypo);

  if (use_cache && cacheLCS.find(target) != cacheLCS.end()) {
    mutex_lcs.lock();
    cacheLCSHit.find(search)->second =
        cacheLCSHit.find(search)->second + 1;
    mutex_lcs.unlock();
    res.set_content(cacheLCS.find(target)->second, "text/json");
    return;
  }

  auto r = extract_lcs(target, m_infos);
  std::sort(r.begin(), r.end(), [](auto first, auto second) -> bool {
    if (first.second != second.second)
      return first.second > second.second;
    return first.first->message.length() < second.first->message.length();
  });

  std::stringstream result;
  int m = 0;
  result << "[";
  for (auto i : r) {
    result << "{";
    result << "\"MatchScore\":\"" << i.second << "\",";
    result << "\"Id\":" << i.first->articleid << ",";
    result << "\"Page\":" << (int)i.first->page << ",";
    result << "\"Correctness\":" << i.first->score << ",";
    result << "\"Rect\":[" << i.first->rects[0] << "," << i.first->rects[1]
           << "," << i.first->rects[2] << "," << i.first->rects[3] << "]";
    result << "}";
    if (m++ == count)
      break;
    result << ",";
  }
  result << "]";

  std::string json(std::istreambuf_iterator<char>(result), {});

  mutex_lcs.lock();
  if (use_cache && cacheLCS.find(target) == cacheLCS.end()) {
    cacheLCS.insert({target, json});
    cacheLCSHit.insert({search, 0});
  }
  mutex_lcs.unlock();

  res.set_content(json, "text/json");
}

void test() {
  std::string s1("dbrrowkdRhfExrl");
  std::string s2("dhwlddjdbrrowkdeornaudxoRhfEnrl");
  auto r = rapidfuzz::fuzz::ratio(s1, s2);
  auto pr = rapidfuzz::fuzz::partial_ratio(s1, s2);
  // std::cout << r << std::endl;

  // 7, 5
  // 7-5-2

  // s2길이 > s1길이 일때
  // r값 - (s2길이 - s1길이)이 실질적인 r값임
  // s2의 길이가 s1보다 1만큼 길면 r값이 무조건 1만큼 커지게됨
  // 이걸 보정해주는게 (s2길이 - s2길이)임

  std::cout << (100-r)/100*(s1.length() + s2.length()) << std::endl;

    double ed = (100-r)/100*(s1.length() + s2.length());
    double lcs = (s1.length() + s2.length() - ed) / 2;

    std::cout << lcs << std::endl;
    std::cout << lcs / (s1.length()) << std::endl;

  // https://velog.io/@ausg/%ED%98%91%EC%97%85%EC%9D%84-%EC%9C%84%ED%95%B4-%EC%9E%85%EC%82%AC-%EC%9D%B4%EC%A0%84%EC%97%90-%EC%95%8C%EA%B3%A0%EA%B0%80%EB%A9%B4-%EC%A2%8B%EC%9D%84-%EA%B2%83%EB%93%A4
  // std::cout << rapidfuzz::fuzz::WRatio("asdf", "aaaaaaaaaasdffff") << std::endl;
}

int main(int argc, char **argv) {
  setlocale(LC_ALL, "");
  std::wcout.imbue(std::locale(""));

  // test();

  if (argc < 4) {
    std::cout << "fast-search binary\n";
    std::cout << "use " << argv[0] << " <host> <port> <private-access-token>";
    return 0;
  }

  load_json();

  //
  //  Private Access Token 정의
  //
  std::string token = std::string(argv[3]);

  //
  //  Http 서버 정의
  //
  httplib::Server svr;

  //
  //  /similar/ 라우팅
  //
  svr.Get(R"(/similar/(.*?))", std::bind(route_similar, std::placeholders::_1,
                                         std::placeholders::_2, true, 15));

  //
  //  /contains/ 라우팅
  //
  svr.Get(R"(/contains/(.*?))", std::bind(route_contains, std::placeholders::_1,
                                          std::placeholders::_2, true, 15));

  //
  //  /containsh/ 라우팅
  //
  svr.Get(R"(/lcs/(.*?))",
          std::bind(route_lcs, std::placeholders::_1,
                    std::placeholders::_2, true, 15));

  //
  //  /<private-access-token>/*/ 라우팅
  //
  svr.Get("/" + token + R"(/similar/(.*?))",
          std::bind(route_similar, std::placeholders::_1, std::placeholders::_2,
                    false, 500));
  svr.Get("/" + token + R"(/contains/(.*?))",
          std::bind(route_contains, std::placeholders::_1,
                    std::placeholders::_2, false, 500));
  svr.Get("/" + token + R"(/lcs/(.*?))",
          std::bind(route_lcs, std::placeholders::_1,
                    std::placeholders::_2, false, 500));

  //
  //  /rank 라우팅
  //
  svr.Get(R"(/rank)", [](const httplib::Request &req, httplib::Response &res) {
    std::stringstream result;

    for (auto ss : cacheSimilarHit) {
      result << "(similar) " << ss.first << ": " << ss.second << "\n";
    }

    for (auto ss : cacheContainsHit) {
      result << "(contains) " << ss.first << ": " << ss.second << "\n";
    }

    for (auto ss : cacheLCS) {
      result << "(lcs) " << ss.first << ": " << ss.second << "\n";
    }

    std::string json(std::istreambuf_iterator<char>(result), {});

    res.set_content(json, "text/json");
  });

  std::cout << "start server." << std::endl;

  svr.listen(argv[1], std::atoi(argv[2]));
}