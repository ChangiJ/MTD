// ws_beast_set_get_annotated.cpp                                                        // 파일 설명: WebSocket으로 채널 on/off 설정(set)과 상태 조회(get)를 지원하는 Beast 클라이언트
// Build: g++ -std=c++17 -O2 ws_beast_set_get_annotated.cpp -o kulgad-cli -lboost_system -lpthread  // 빌드 예시(보통 -lboost_thread는 불필요)
// Run  :                                                                                 // 실행 예시 요약
//   ./kulgad-cli set 3 on                                                                // 로컬(기본 localhost:3001)에서 채널 3을 켜기
//   ./kulgad-cli set 0-4,7,10-12 off                                                     // 범위/리스트 혼용으로 끄기
//   ./kulgad-cli set all on                                                              // 전체(0~255) 켜기
//   ./kulgad-cli get all                                                                 // 전체 상태 조회
//   ./kulgad-cli get 2-20                                                                // 2~20 상태 조회
//   ./kulgad-cli 210.119.41.68 3001 set 5,9 off                                          // 원격 호스트/포트 지정

#include <boost/beast/core.hpp>                                                          // Beast 핵심 타입(flat_buffer 등)
#include <boost/beast/websocket.hpp>                                                     // Beast WebSocket 스트림 API
#include <boost/asio/connect.hpp>                                                        // ASIO connect 유틸리티(엔드포인트 순차 연결)
#include <boost/asio/ip/tcp.hpp>                                                         // ASIO TCP 소켓/리졸버
#include <algorithm>                                                                     // std::sort, std::unique, std::remove_if
#include <cctype>                                                                        // std::tolower, std::isspace
#include <chrono>                                                                        // 전송 간격(50ms)을 위한 chrono
#include <iostream>                                                                      // 표준 입출력
#include <string>                                                                        // std::string
#include <thread>                                                                        // std::this_thread::sleep_for
#include <vector>                                                                        // 채널 목록 컨테이너

namespace beast = boost::beast;                                                          // beast:: 로 축약
namespace websocket = beast::websocket;                                                  // websocket:: 로 축약
namespace net = boost::asio;                                                             // net:: 로 축약
using tcp = boost::asio::ip::tcp;                                                        // tcp::resolver, tcp::socket 등 간결 표기

enum class Mode { SET, GET };                                                            // 동작 모드 열거형: 설정(SET) 또는 조회(GET)

static void usage() {                                                                    // 사용법 출력 함수
    std::cerr <<
      "Usage:\n"                                                                          // 첫 줄: 사용법 라벨
      "  kulgad-cli [host] [port] set <channels|all> <on|off>\n"                          // set 명령 형식
      "  kulgad-cli [host] [port] get <channels|all>\n"                                   // get 명령 형식
      "  <channels>: 0..255, 콤마/범위 혼용 가능 (예: 0-4,7,10-12)\n";                    // 채널 지정 규칙
}                                                                                         // usage 끝

static bool parse_channels(const std::string& raw, std::vector<int>& out) {              // 채널 문자열(raw)을 파싱해 out에 정수 채널 목록을 채워 넣음
    std::string s = raw;                                                                  // 가변 복사본 생성
    s.erase(std::remove_if(s.begin(), s.end(),                                          //
                           [](unsigned char c){ return std::isspace(c); }), s.end());     // 모든 공백 제거(파싱 단순화)
    if (s.empty()) return false;                                                          // 비어 있으면 실패

    std::string lower = s;                                                                // 소문자 버전 준비
    std::transform(lower.begin(), lower.end(), lower.begin(),
                   [](unsigned char c){ return std::tolower(c); });                       // 소문자로 변환

    if (lower == "all" || lower.find("all") != std::string::npos) {                       // 'all' 단독 또는 목록 내 포함 시
        out.reserve(256);                                                                 // 용량 예약
        for (int ch = 0; ch <= 255; ++ch) out.push_back(ch);                              // 0~255 전부 추가
    } else {                                                                              // 그렇지 않으면 콤마/범위 파싱
        size_t start = 0;                                                                 // 토큰 시작 인덱스
        auto add = [&](int ch)->bool {                                                    // 단일 채널 추가 헬퍼(유효성 검사 포함)
            if (ch < 0 || ch > 255) { std::cerr << "Channel out of range: " << ch << "\n"; return false; } // 범위 체크
            out.push_back(ch); return true;                                               // 추가 성공
        };
        while (start < s.size()) {                                                        // 문자열 끝까지 반복
            size_t pos = s.find(',', start);                                             // 다음 콤마 위치
            std::string tok = s.substr(start, (pos == std::string::npos) ? std::string::npos : pos - start); // 현재 토큰
            if (!tok.empty()) {                                                           // 토큰이 비어있지 않다면
                size_t dash = tok.find('-');                                              // 범위 구분자('-') 검색
                if (dash == std::string::npos) {                                          // '-' 없으면 단일 숫자
                    try { if (!add(std::stoi(tok))) return false; }                       // 정수 변환 후 추가
                    catch (...) { std::cerr << "Invalid channel: " << tok << "\n"; return false; } // 변환 실패
                } else {                                                                  // '-' 있으면 범위 a-b
                    std::string a_str = tok.substr(0, dash);                              // a 부분 추출
                    std::string b_str = tok.substr(dash + 1);                             // b 부분 추출
                    if (a_str.empty() || b_str.empty()) { std::cerr << "Invalid range: " << tok << "\n"; return false; } // 형식 오류
                    int a, b;                                                             // 범위 경계
                    try { a = std::stoi(a_str); b = std::stoi(b_str); }                   // 정수 변환
                    catch (...) { std::cerr << "Invalid range: " << tok << "\n"; return false; } // 실패
                    if (a > b) std::swap(a, b);                                           // a>b면 교환
                    if (a < 0 || b > 255) { std::cerr << "Range out of bounds: " << tok << "\n"; return false; } // 범위 검사
                    for (int ch = a; ch <= b; ++ch) out.push_back(ch);                    // a..b 전부 추가
                }
            }
            if (pos == std::string::npos) break;                                          // 더 이상 콤마 없으면 종료
            start = pos + 1;                                                              // 다음 토큰으로 이동
        }
        if (out.empty()) { std::cerr << "No valid channels parsed\n"; return false; }     // 유효 채널이 하나도 없으면 실패
    }
    std::sort(out.begin(), out.end());                                                    // 정렬
    out.erase(std::unique(out.begin(), out.end()), out.end());                            // 중복 제거
    return true;                                                                          // 성공
}                                                                                         // parse_channels 끝

static bool parse_args(int argc, char** argv,                                            //
                       std::string& host, std::string& port,                             // 결과: 호스트/포트
                       Mode& mode, std::vector<int>& channels, bool& val)                // 결과: 모드/채널/상태
{
    host = "localhost";                                                                   // 기본 호스트
    port = "3001";                                                                        // 기본 포트

    if (argc < 3) { usage(); return false; }                                              // 인자 부족 시 사용법

    int idx = 1;                                                                          // 명령(set|get)의 예상 위치
    std::string cmd = argv[1];                                                            // 첫 토큰
    if (cmd != "set" && cmd != "get") {                                                   // 첫 토큰이 set/get이 아니면
        if (argc < 5) { usage(); return false; }                                          // 호스트/포트/명령 최소 개수 확인
        host = argv[1];                                                                   // 호스트 지정
        port = argv[2];                                                                   // 포트 지정
        idx  = 3;                                                                         // 명령 위치는 3번째
        cmd  = argv[idx];                                                                 // 명령 토큰 갱신
    }

    if (cmd == "set") {                                                                   // SET 모드 파싱
        if (argc < idx + 3) { usage(); return false; }                                    // set <channels> <on|off> 최소 인자 확인
        std::string chanJoined;                                                           // 채널 문자열(여러 토큰 가능)
        for (int i = idx + 1; i < argc - 1; ++i) {                                        // 마지막(상태) 앞까지 합치기
            if (!chanJoined.empty()) chanJoined.push_back(' ');                           // 가독용 공백(나중에 제거)
            chanJoined += argv[i];                                                        // 누적
        }
        if (!parse_channels(chanJoined, channels)) return false;                          // 채널 파싱

        std::string state = argv[argc - 1];                                               // 마지막 인자: on/off
        std::transform(state.begin(), state.end(), state.begin(),
                       [](unsigned char c){ return std::tolower(c); });                   // 소문자화
        if (state == "on" || state == "true" || state == "1") val = true;                 // on 판정
        else if (state == "off" || state == "false" || state == "0") val = false;         // off 판정
        else { std::cerr << "State must be on/off\n"; return false; }                     // 그 외는 에러
        mode = Mode::SET;                                                                  // 모드 설정
        return true;                                                                       // 파싱 성공
    }

    if (cmd == "get") {                                                                   // GET 모드 파싱
        if (argc < idx + 2) { usage(); return false; }                                    // get <channels> 최소 인자 확인
        std::string chanJoined;                                                           // 채널 문자열
        for (int i = idx + 1; i < argc; ++i) {                                            // 끝까지 합치기
            if (!chanJoined.empty()) chanJoined.push_back(' ');                           // 공백(나중 제거)
            chanJoined += argv[i];                                                        // 누적
        }
        if (!parse_channels(chanJoined, channels)) return false;                          // 채널 파싱
        mode = Mode::GET;                                                                  // 모드 설정
        return true;                                                                       // 성공
    }

    usage();                                                                              // 그 외 명령이면 사용법
    return false;                                                                         // 실패
}                                                                                         // parse_args 끝

// 매우 단순한 JSON 파서: {"pins":[true,false,...]} 형태에서 pins 배열만 추출(정식 JSON 파서는 아님)
static std::vector<bool> parse_pins_from_json(const std::string& js) {                   // 서버 응답 문자열에서 pins 배열만 뽑아 bool 벡터로 반환
    std::vector<bool> pins;                                                               // 결과 컨테이너
    auto p = js.find("\"pins\"");                                                         // "pins" 키 검색
    if (p == std::string::npos) return pins;                                              // 없으면 빈 벡터 반환
    auto lb = js.find('[', p);                                                            // 좌 대괄호 위치
    if (lb == std::string::npos) return pins;                                             // 없으면 실패
    auto rb = js.find(']', lb);                                                           // 우 대괄호 위치
    if (rb == std::string::npos) return pins;                                             // 없으면 실패

    std::string arr = js.substr(lb + 1, rb - lb - 1);                                     // 대괄호 사이 부분 추출
    for (size_t i = 0; i < arr.size(); ++i) {                                             // 배열 문자열 스캔
        if (i + 4 <= arr.size() && arr.compare(i, 4, "true") == 0) {                      // "true" 발견
            pins.push_back(true); i += 3;                                                 // true 추가 후 인덱스 건너뛰기
        } else if (i + 5 <= arr.size() && arr.compare(i, 5, "false") == 0) {              // "false" 발견
            pins.push_back(false); i += 4;                                                // false 추가 후 인덱스 건너뛰기
        }
    }
    return pins;                                                                          // 결과 반환
}                                                                                         // parse_pins_from_json 끝

int main(int argc, char** argv) {                                                         // 프로그램 진입점
    try {                                                                                 // 예외 처리 시작
        std::string host, port;                                                           // 호스트/포트
        Mode mode;                                                                        // 동작 모드(SET/GET)
        std::vector<int> channels;                                                        // 채널 목록
        bool val = false;                                                                 // SET 모드에서 on(true)/off(false)

        if (!parse_args(argc, argv, host, port, mode, channels, val)) return 1;           // 인자 파싱 실패 시 종료

        net::io_context ioc;                                                              // ASIO I/O 컨텍스트
        tcp::resolver resolver{ioc};                                                      // DNS 리졸버
        auto const results = resolver.resolve(host, port);                                // host:port → 엔드포인트 목록

        websocket::stream<tcp::socket> ws{ioc};                                           // WebSocket 스트림 객체
        net::connect(ws.next_layer(), results.begin(), results.end());                    // TCP 연결(여러 엔드포인트 중 성공하는 곳에 연결)
        ws.handshake(host, "/");                                                          // WebSocket 핸드셰이크(경로가 다르면 "/ws" 등으로 변경)
        ws.text(true);                                                                    // 텍스트 모드 명시(가독성)
        std::cout << "Connected to " << host << ":" << port << "\n";                      // 연결 로그

        constexpr auto kDelay = std::chrono::milliseconds(50);                            // 전송 간격(약 50ms)

        if (mode == Mode::SET) {                                                          // SET 모드인 경우
            for (size_t i = 0; i < channels.size(); ++i) {                                // 지정 채널 전부 순회
                int ch = channels[i];                                                     // 현재 채널
                std::string payload = std::string("{\"cmd\": \"set\", \"ch\":") +         // JSON 페이로드 구성 시작
                                      std::to_string(ch) + ", \"val\": " +                // "ch": 숫자
                                      (val ? "true" : "false") + "}";                     // "val": true/false
                ws.write(net::buffer(payload));                                           // 서버에 전송(동기 write)
                std::cout << "Sent: set ch=" << ch                                        // 전송 로그
                          << " val=" << (val ? "on" : "off") << "\n";
                if (i + 1 < channels.size())                                              // 마지막 채널이 아니면
                    std::this_thread::sleep_for(kDelay);                                  // 50ms 대기 후 다음 채널 전송
            }
            std::this_thread::sleep_for(kDelay);                                          // 마지막 set 이후 약간 대기(서버/시리얼 처리 여유)
        }

        ws.write(net::buffer(std::string(R"({"cmd": "get"})")));                          // GET 모드이거나 SET 후 최종 상태 조회를 위해 get 전송
        beast::flat_buffer buffer;                                                        // 수신 버퍼
        ws.read(buffer);                                                                  // 서버 응답 1개 수신
        std::string body = beast::buffers_to_string(buffer.data());                       // 버퍼를 std::string으로 변환
        auto pins = parse_pins_from_json(body);                                           // pins 배열 파싱(성공 시 true/false 벡터)

        if (pins.empty()) {                                                               // pins 배열을 찾지 못한 경우
            std::cout << "Received (raw): " << body << "\n";                              // 원문 출력
            std::cerr << "Warning: 'pins' array not found in response.\n";                // 경고 메시지
        } else {                                                                           // pins 배열이 있는 경우
            std::cout << "Status:\n";                                                     // 헤더 출력
            int per_line = 16;                                                            // 한 줄에 16개씩 출력(가독성)
            int cnt = 0;                                                                  // 출력 카운터
            for (int ch : channels) {                                                     // 요청한 채널들만 출력
                std::string state = (ch >= 0 && ch < (int)pins.size())                    // 인덱스 유효성 검사
                                      ? (pins[ch] ? "on" : "off")                         // 유효하면 on/off 표시
                                      : "n/a";                                            // 범위를 벗어나면 n/a
                std::cout << ch << ":" << state << ((++cnt % per_line) ? "  " : "\n");    // 채널:상태 출력(열 정리)
            }
            if (cnt % per_line) std::cout << "\n";                                        // 마지막 줄 개행 보정
        }

        ws.close(websocket::close_code::normal);                                          // 정상 종료(클로즈 프레임 전송)
        return 0;                                                                         // 성공 코드
    } catch (const std::exception& e) {                                                   // 예외 처리(연결/핸드셰이크/IO 등)
        std::cerr << "Error: " << e.what() << "\n";                                       // 에러 메시지 출력
        return 1;                                                                         // 실패 코드
    }
}                                                                                         // main 끝
