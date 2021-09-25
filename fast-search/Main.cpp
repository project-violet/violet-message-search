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

std::map<std::string, std::string> cacheSimilar;
std::map<std::string, int> cacheSimilarHit;
std::map<std::string, std::string> cacheContains;
std::map<std::string, int> cacheContainsHit;

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
            << " | " << req.remote_addr << std::endl;

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
            << " | " << req.remote_addr << std::endl;

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

int main(int argc, char **argv) {
  setlocale(LC_ALL, "");
  std::wcout.imbue(std::locale(""));

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
  //  /<private-access-token>/*/ 라우팅
  //
  svr.Get("/" + token + R"(/similar/(.*?))",
          std::bind(route_similar, std::placeholders::_1, std::placeholders::_2,
                    false, 50));
  svr.Get("/" + token + R"(/contains/(.*?))",
          std::bind(route_contains, std::placeholders::_1,
                    std::placeholders::_2, false, 50));

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

    std::string json(std::istreambuf_iterator<char>(result), {});

    res.set_content(json, "text/json");
  });

  std::cout << "start server." << std::endl;

  svr.listen(argv[1], std::atoi(argv[2]));
}